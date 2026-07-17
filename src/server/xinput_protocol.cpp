#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"
#include "xmin/server/property_data.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_atom = 5,
    bad_cursor = 6,
    bad_match = 8,
    bad_access = 10,
    bad_alloc = 11,
    bad_length = 16,
};

constexpr std::uint32_t supported_event_mask =
    0x0003ffffU | (1U << 25) | (1U << 26);
constexpr std::uint32_t unsupported_hardware_event_mask =
    ((1U << 25) - 1U) & ~((1U << 18) - 1U);

Result<void>
malformed_xinput(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

bool
valid_device(std::uint16_t device) noexcept
{
    return device == xi2_pointer_device_id ||
        device == xi2_keyboard_device_id;
}

bool
valid_device_selector(std::uint16_t device) noexcept
{
    return device == xi2_all_devices ||
        device == xi2_all_master_devices || valid_device(device);
}

std::int32_t
signed_fixed(std::uint32_t value) noexcept
{
    const std::int64_t widened = value;
    return static_cast<std::int32_t>(
        widened <= std::numeric_limits<std::int32_t>::max()
            ? widened
            : widened - 0x100000000LL);
}

std::int32_t
fixed_integer(std::uint32_t value) noexcept
{
    return signed_fixed(value) / 65536;
}

std::uint32_t
fp1616(std::int32_t value) noexcept
{
    const std::int32_t bounded = std::clamp<std::int32_t>(
        value, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max());
    return static_cast<std::uint32_t>(bounded) << 16;
}

std::uint64_t
fp3232(std::int32_t value) noexcept
{
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(value) * (std::int64_t{1} << 32));
}

std::uint32_t
core_grab_mask(std::uint32_t xi_mask, std::uint16_t device) noexcept
{
    if (device == xi2_keyboard_device_id)
        return (xi_mask & ((1U << 2) | (1U << 3))) >> 2;
    std::uint32_t core = 0;
    if ((xi_mask & (1U << 4)) != 0)
        core |= 1U << 2;
    if ((xi_mask & (1U << 5)) != 0)
        core |= 1U << 3;
    if ((xi_mask & (1U << 6)) != 0)
        core |= 1U << 6;
    if ((xi_mask & (1U << 7)) != 0)
        core |= 1U << 4;
    if ((xi_mask & (1U << 8)) != 0)
        core |= 1U << 5;
    return core;
}

void
write_reply_header(WireWriter &reply, std::uint16_t sequence,
                   std::uint32_t body_words)
{
    reply.u8(1);
    reply.u8(0);
    reply.u16(sequence);
    reply.u32(body_words);
}

void
write_device_info(WireWriter &body, std::uint16_t device,
                  std::uint16_t attachment, std::uint16_t type,
                  std::string_view name, const InputState &input,
                  AtomId x_label, AtomId y_label,
                  std::uint16_t width, std::uint16_t height)
{
    const bool pointer = device == xi2_pointer_device_id;
    body.u16(device);
    body.u16(type);
    body.u16(attachment);
    body.u16(pointer ? 3 : 1);
    body.u16(static_cast<std::uint16_t>(name.size()));
    body.u8(1);
    body.u8(0);
    body.bytes(name);
    body.pad_to_four();
    if (pointer) {
        constexpr std::uint16_t buttons = 10;
        body.u16(1); // ButtonClass
        body.u16(13);
        body.u16(device);
        body.u16(buttons);
        std::uint32_t state = 0;
        for (std::uint16_t button = 1; button <= buttons; ++button) {
            if (input.pressed_buttons.test(button))
                state |= 1U << button;
        }
        body.u32(state);
        for (std::uint16_t button = 0; button < buttons; ++button)
            body.u32(0); // no semantic button labels

        const auto valuator = [&](std::uint16_t number, AtomId label,
                                  std::uint16_t maximum,
                                  std::int32_t value) {
            body.u16(2); // ValuatorClass
            body.u16(11);
            body.u16(device);
            body.u16(number);
            body.u32(label);
            body.u64(fp3232(0));
            body.u64(fp3232(maximum == 0 ? 0 : maximum - 1));
            body.u64(fp3232(value));
            body.u32(1);
            body.u8(1); // absolute
            body.pad(3);
        };
        valuator(0, x_label, width, input.pointer_x);
        valuator(1, y_label, height, input.pointer_y);
        return;
    }

    constexpr std::uint16_t keys = maximum_keycode - minimum_keycode + 1;
    body.u16(0); // KeyClass
    body.u16(static_cast<std::uint16_t>(2 + keys));
    body.u16(device);
    body.u16(keys);
    for (std::uint16_t key = minimum_keycode; key <= maximum_keycode; ++key)
        body.u32(key);
}

} // namespace

Result<void>
Connection::handle_xinput(const RequestContext &context)
{
    constexpr std::uint8_t bad_device = xinput_extension.first_error;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto device_error = [&](std::uint16_t device) {
        return error(bad_device, device);
    };
    const auto update = [&](Xi2Update result) {
        if (result == Xi2Update::updated)
            return drain_pending_events();
        return error(bad_alloc);
    };
    if (context.data < 40)
        return handle_xinput_legacy(context);
    if (context.data != 47 && !xi2_version_negotiated_)
        return error(bad_request);

    switch (context.data) {
    case 40: { // XIQueryPointer
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto device = reader.u16();
        if (!window_id || !device || !reader.skip(2))
            return malformed_xinput("truncated XIQueryPointer request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (*device != xi2_pointer_device_id)
            return device_error(*device);
        const auto &input = server_.input();
        const auto origin = server_.absolute_position(*window_id);
        std::uint32_t child = 0;
        if (server_.map_state(*window_id) == 2 &&
            input.pointer_x >= origin.first &&
            input.pointer_x < origin.first + window->width &&
            input.pointer_y >= origin.second &&
            input.pointer_y < origin.second + window->height) {
            child = server_.child_window_at(
                *window_id, input.pointer_x, input.pointer_y);
        }
        WireWriter reply(context.order);
        write_reply_header(reply, context.sequence, 7);
        reply.u32(root_window_id);
        reply.u32(child);
        reply.u32(fp1616(input.pointer_x));
        reply.u32(fp1616(input.pointer_y));
        reply.u32(fp1616(input.pointer_x - origin.first));
        reply.u32(fp1616(input.pointer_y - origin.second));
        reply.u8(1);
        reply.u8(0);
        reply.u16(1);
        const auto state = server_.xkb_state();
        reply.u32(state.base_mods);
        reply.u32(state.latched_mods);
        reply.u32(state.locked_mods);
        reply.u32(state.mods);
        reply.u8(static_cast<std::uint8_t>(state.base_group));
        reply.u8(static_cast<std::uint8_t>(state.latched_group));
        reply.u8(state.locked_group);
        reply.u8(state.group);
        std::uint32_t buttons = 0;
        for (std::uint32_t button = 1; button <= 10; ++button) {
            if (input.pressed_buttons.test(button))
                buttons |= 1U << button;
        }
        reply.u32(buttons);
        return queue(reply.data());
    }
    case 41: { // XIWarpPointer
        if (context.request.size() != 36)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 32, context.order);
        const auto source_id = reader.u32();
        const auto destination_id = reader.u32();
        const auto source_x = reader.u32();
        const auto source_y = reader.u32();
        const auto source_width = reader.u16();
        const auto source_height = reader.u16();
        const auto destination_x = reader.u32();
        const auto destination_y = reader.u32();
        const auto device = reader.u16();
        if (!source_id || !destination_id || !source_x || !source_y ||
            !source_width || !source_height || !destination_x ||
            !destination_y || !device || !reader.skip(2)) {
            return malformed_xinput("truncated XIWarpPointer request");
        }
        if (*device != xi2_pointer_device_id)
            return device_error(*device);
        const auto *source = *source_id == 0
            ? nullptr : server_.window(*source_id);
        const auto *destination = *destination_id == 0
            ? nullptr : server_.window(*destination_id);
        if (*source_id != 0 && source == nullptr)
            return error(bad_window, *source_id);
        if (*destination_id != 0 && destination == nullptr)
            return error(bad_window, *destination_id);
        auto &input = server_.input();
        if (source != nullptr) {
            const auto origin = server_.absolute_position(source->id);
            const std::int64_t left = origin.first + fixed_integer(*source_x);
            const std::int64_t top = origin.second + fixed_integer(*source_y);
            if (server_.map_state(source->id) != 2 ||
                input.pointer_x < left || input.pointer_y < top ||
                (*source_width != 0 &&
                 input.pointer_x >= left + *source_width) ||
                (*source_height != 0 &&
                 input.pointer_y >= top + *source_height)) {
                return Result<void>::success();
            }
        }
        std::int64_t x = input.pointer_x;
        std::int64_t y = input.pointer_y;
        if (destination != nullptr) {
            const auto origin = server_.absolute_position(destination->id);
            x = origin.first;
            y = origin.second;
        }
        x += fixed_integer(*destination_x);
        y += fixed_integer(*destination_y);
        const auto delivered = server_.inject_input(
            6, 0,
            static_cast<std::int32_t>(std::clamp<std::int64_t>(
                x, 0, server_.width() - 1)),
            static_cast<std::int32_t>(std::clamp<std::int64_t>(
                y, 0, server_.height() - 1)));
        if (delivered == EventDelivery::queue_full)
            return error(bad_alloc);
        return drain_pending_events();
    }
    case 42: { // XIChangeCursor
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto window_id = reader.u32();
        const auto cursor_id = reader.u32();
        const auto device = reader.u16();
        if (!window_id || !cursor_id || !device || !reader.skip(2))
            return malformed_xinput("truncated XIChangeCursor request");
        if (*device != xi2_pointer_device_id)
            return device_error(*device);
        auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        std::shared_ptr<CursorImage> cursor;
        if (*cursor_id != 0) {
            const auto *record = server_.cursor(*cursor_id);
            if (record == nullptr)
                return error(bad_cursor, *cursor_id);
            cursor = record->image;
        }
        if (server_.set_window_cursor(*window, std::move(cursor)) !=
            XFixesUpdate::updated) {
            return error(bad_alloc);
        }
        return drain_pending_events();
    }
    case 43: { // XIChangeHierarchy: fixed topology
        if (context.request.size() < 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto count = reader.u8();
        if (!count || !reader.skip(3))
            return malformed_xinput("truncated XIChangeHierarchy request");
        if (*count == 0)
            return context.request.size() == 8
                ? Result<void>::success() : error(bad_length);
        WireReader changes(
            context.request.data() + 8, context.request.size() - 8,
            context.order);
        for (std::uint8_t index = 0; index < *count; ++index) {
            const auto type = changes.u16();
            const auto words = changes.u16();
            if (!type || !words)
                return malformed_xinput("truncated XI hierarchy change");
            if (*type < 1 || *type > 4 || *words == 0)
                return error(bad_value, *type);
            const auto tail = checked_multiply(
                static_cast<std::size_t>(*words - 1), std::size_t{4});
            if (!tail || !changes.skip(*tail))
                return error(bad_length);
        }
        if (changes.remaining() != 0)
            return error(bad_length);
        return error(bad_match);
    }
    case 44: { // XISetClientPointer
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto device = reader.u16();
        if (!window_id || !device || !reader.skip(2))
            return malformed_xinput("truncated XISetClientPointer request");
        if (*window_id != 0 && server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (*device != xi2_pointer_device_id)
            return device_error(*device);
        xi2_client_pointer_ = *device;
        return Result<void>::success();
    }
    case 45: { // XIGetClientPointer
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed_xinput("truncated XIGetClientPointer request");
        if (*window_id != 0 && server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        WireWriter reply(context.order);
        write_reply_header(reply, context.sequence, 0);
        reply.u8(1);
        reply.u8(0);
        reply.u16(xi2_client_pointer_);
        reply.pad(20);
        return queue(reply.data());
    }
    case 46: { // XISelectEvents
        if (context.request.size() < 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window_id = reader.u32();
        const auto count = reader.u16();
        if (!window_id || !count || !reader.skip(2))
            return malformed_xinput("truncated XISelectEvents request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (*count > maximum_xi2_masks_per_selection)
            return error(bad_value, *count);
        Xi2EventSelection selection;
        selection.owner = config_.resource_base;
        selection.window = *window_id;
        try {
            selection.masks.reserve(*count);
            for (std::uint16_t index = 0; index < *count; ++index) {
                const auto device = reader.u16();
                const auto words = reader.u16();
                if (!device || !words)
                    return malformed_xinput("truncated XI event mask");
                if (!valid_device_selector(*device))
                    return device_error(*device);
                if (std::any_of(
                        selection.masks.begin(), selection.masks.end(),
                        [device](const Xi2EventMask &existing) {
                            return existing.device == *device;
                        })) {
                    return error(bad_value, *device);
                }
                if (*words > 1)
                    return error(bad_value, *words);
                Xi2EventMask mask;
                mask.device = *device;
                for (std::uint16_t word = 0; word < *words; ++word) {
                    const auto value = reader.u32();
                    if (!value)
                        return malformed_xinput("truncated XI event bits");
                    if ((*value & ~supported_event_mask) != 0 ||
                        (*value & unsupported_hardware_event_mask) != 0) {
                        return error(bad_value, *value);
                    }
                    mask.words.push_back(*value);
                }
                selection.masks.push_back(std::move(mask));
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (reader.remaining() != 0)
            return error(bad_length);
        return update(server_.select_xi2_events(std::move(selection)));
    }
    case 47: { // XIQueryVersion
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto major = reader.u16();
        const auto minor = reader.u16();
        if (!major || !minor)
            return malformed_xinput("truncated XIQueryVersion request");
        if (*major < 2)
            return error(bad_value, *major);
        const std::uint16_t negotiated_minor = *major == 2
            ? std::min<std::uint16_t>(*minor, xinput_extension.minor_version)
            : xinput_extension.minor_version;
        xi2_version_negotiated_ = true;
        xi2_minor_version_ = negotiated_minor;
        WireWriter reply(context.order);
        write_reply_header(reply, context.sequence, 0);
        reply.u16(xinput_extension.major_version);
        reply.u16(negotiated_minor);
        reply.pad(20);
        return queue(reply.data());
    }
    case 48: { // XIQueryDevice
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xinput("truncated XIQueryDevice request");
        if (!valid_device_selector(*device))
            return device_error(*device);
        const AtomId x_label = server_.atoms().intern("Abs X");
        const AtomId y_label = server_.atoms().intern("Abs Y");
        if (x_label == 0 || y_label == 0)
            return error(bad_alloc);
        const bool include_pointer = *device == xi2_all_devices ||
            *device == xi2_all_master_devices ||
            *device == xi2_pointer_device_id;
        const bool include_keyboard = *device == xi2_all_devices ||
            *device == xi2_all_master_devices ||
            *device == xi2_keyboard_device_id;
        WireWriter body(context.order);
        if (include_pointer) {
            write_device_info(
                body, xi2_pointer_device_id, xi2_keyboard_device_id, 1,
                "Xmin master pointer", server_.input(), x_label, y_label,
                server_.width(), server_.height());
        }
        if (include_keyboard) {
            write_device_info(
                body, xi2_keyboard_device_id, xi2_pointer_device_id, 2,
                "Xmin master keyboard", server_.input(), x_label, y_label,
                server_.width(), server_.height());
        }
        WireWriter reply(context.order);
        write_reply_header(
            reply, context.sequence,
            static_cast<std::uint32_t>(body.size() / 4));
        reply.u16(static_cast<std::uint16_t>(
            (include_pointer ? 1 : 0) + (include_keyboard ? 1 : 0)));
        reply.pad(22);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 49: { // XISetFocus
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto focus_id = reader.u32();
        const auto time = reader.u32();
        const auto device = reader.u16();
        if (!focus_id || !time || !device || !reader.skip(2))
            return malformed_xinput("truncated XISetFocus request");
        if (*device != xi2_keyboard_device_id)
            return device_error(*device);
        FocusKind kind = FocusKind::window;
        if (*focus_id == 0)
            kind = FocusKind::none;
        else if (*focus_id == pointer_root_id)
            kind = FocusKind::pointer_root;
        else if (server_.window(*focus_id) == nullptr)
            return error(bad_window, *focus_id);
        else if (server_.map_state(*focus_id) != 2)
            return error(bad_match);
        const auto result = server_.set_input_focus(kind, *focus_id, 0, *time);
        if (result == FocusUpdate::queue_full)
            return error(bad_alloc);
        return drain_pending_events();
    }
    case 50: { // XIGetFocus
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xinput("truncated XIGetFocus request");
        if (*device != xi2_keyboard_device_id)
            return device_error(*device);
        WireWriter reply(context.order);
        write_reply_header(reply, context.sequence, 0);
        reply.u32(server_.input().focus.wire_id());
        reply.pad(20);
        return queue(reply.data());
    }
    case 51: { // XIGrabDevice
        if (context.request.size() < 24)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window_id = reader.u32();
        const auto time = reader.u32();
        const auto cursor_id = reader.u32();
        const auto device = reader.u16();
        const auto mode = reader.u8();
        const auto paired_mode = reader.u8();
        const auto owner_events = reader.u8();
        const bool padding = reader.skip(1);
        const auto mask_words = reader.u16();
        if (!window_id || !time || !cursor_id || !device || !mode ||
            !paired_mode || !owner_events || !padding || !mask_words) {
            return malformed_xinput("truncated XIGrabDevice request");
        }
        if (!valid_device(*device))
            return device_error(*device);
        if (*mode > 1 || *paired_mode > 1 || *owner_events > 1 ||
            *mask_words > 1)
            return error(bad_value);
        if (*mode == 0 || *paired_mode == 0)
            return error(bad_match); // synchronous freezing is not in profile
        const auto mask = *mask_words == 0 ? std::optional<std::uint32_t>{0}
                                           : reader.u32();
        if (!mask || reader.remaining() != 0 ||
            (*mask & ~supported_event_mask) != 0)
            return error(bad_length);
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (*cursor_id != 0 && server_.cursor(*cursor_id) == nullptr)
            return error(bad_cursor, *cursor_id);
        auto &input = server_.input();
        auto &existing = *device == xi2_pointer_device_id
            ? input.pointer_grab : input.keyboard_grab;
        const std::uint32_t effective_time = *time == 0
            ? server_.current_time() : *time;
        std::uint8_t status = 0;
        if (existing && existing->owner != config_.resource_base)
            status = 1; // AlreadyGrabbed
        else if (server_.map_state(*window_id) != 2)
            status = 3; // GrabNotViewable
        else if (static_cast<std::int32_t>(
                     effective_time - server_.current_time()) > 0 ||
                 static_cast<std::int32_t>(
                     effective_time - (*device == xi2_pointer_device_id
                         ? input.pointer_grab_time
                         : input.keyboard_grab_time)) < 0) {
            status = 2; // GrabInvalidTime
        }
        else {
            std::shared_ptr<CursorImage> cursor;
            if (*cursor_id != 0)
                cursor = server_.cursor(*cursor_id)->image;
            ActiveGrab grab{
                config_.resource_base, *window_id, 0, effective_time,
                core_grab_mask(*mask, *device), *mode, *paired_mode,
                *owner_events != 0, false, 0, false, std::move(cursor)};
            grab.xi2 = true;
            grab.xi2_event_mask = *mask;
            const auto activated = *device == xi2_pointer_device_id
                ? server_.activate_pointer_grab(std::move(grab))
                : server_.activate_keyboard_grab(std::move(grab));
            if (activated == EventDelivery::queue_full)
                return error(bad_alloc);
        }
        auto drained = drain_pending_events();
        if (!drained)
            return drained;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(status);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        return queue(reply.data());
    }
    case 52: { // XIUngrabDevice
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto time = reader.u32();
        const auto device = reader.u16();
        if (!time || !device || !reader.skip(2))
            return malformed_xinput("truncated XIUngrabDevice request");
        if (!valid_device(*device))
            return device_error(*device);
        auto &grab = *device == xi2_pointer_device_id
            ? server_.input().pointer_grab : server_.input().keyboard_grab;
        const std::uint32_t effective_time = *time == 0
            ? server_.current_time() : *time;
        if (grab && grab->owner == config_.resource_base &&
            static_cast<std::int32_t>(
                effective_time - server_.current_time()) <= 0 &&
            static_cast<std::int32_t>(
                effective_time - grab->activated_at) >= 0) {
            const auto result = *device == xi2_pointer_device_id
                ? server_.deactivate_pointer_grab()
                : server_.deactivate_keyboard_grab();
            if (result == EventDelivery::queue_full)
                return error(bad_alloc);
        }
        return drain_pending_events();
    }
    case 53: { // XIAllowEvents
        if (context.request.size() != 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto time = reader.u32();
        const auto device = reader.u16();
        const auto mode = reader.u8();
        const bool padding = reader.skip(1);
        const auto touch = reader.u32();
        const auto window = reader.u32();
        if (!time || !device || !mode || !padding || !touch || !window)
            return malformed_xinput("truncated XIAllowEvents request");
        if (!valid_device(*device))
            return device_error(*device);
        if (*mode == 0 || *mode == 3 || *mode == 4)
            return Result<void>::success();
        return error(bad_match); // sync/replay/touch modes are outside profile
    }
    case 54: { // XIPassiveGrabDevice
        if (context.request.size() < 32)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto time = reader.u32();
        const auto window_id = reader.u32();
        const auto cursor_id = reader.u32();
        const auto detail = reader.u32();
        const auto device = reader.u16();
        const auto modifier_count = reader.u16();
        const auto mask_words = reader.u16();
        const auto grab_type = reader.u8();
        const auto mode = reader.u8();
        const auto paired_mode = reader.u8();
        const auto owner_events = reader.u8();
        if (!time || !window_id || !cursor_id || !detail || !device ||
            !modifier_count || !mask_words || !grab_type || !mode ||
            !paired_mode || !owner_events || !reader.skip(2)) {
            return malformed_xinput("truncated XIPassiveGrabDevice request");
        }
        if (!valid_device(*device))
            return device_error(*device);
        if ((*grab_type == 0 && *device != xi2_pointer_device_id) ||
            (*grab_type == 1 && *device != xi2_keyboard_device_id) ||
            *grab_type > 1) {
            return error(bad_match);
        }
        if (*detail > 255 || *mask_words > 1 || *mode > 1 ||
            *paired_mode > 1 || *owner_events > 1)
            return error(bad_value);
        if (*mode == 0 || *paired_mode == 0)
            return error(bad_match); // synchronous freezing is not in profile
        const std::uint32_t effective_time = *time == 0
            ? server_.current_time() : *time;
        if (static_cast<std::int32_t>(
                effective_time - server_.current_time()) > 0)
            return error(bad_value, *time);
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (*cursor_id != 0 && server_.cursor(*cursor_id) == nullptr)
            return error(bad_cursor, *cursor_id);
        std::uint32_t mask = 0;
        if (*mask_words != 0) {
            const auto value = reader.u32();
            if (!value)
                return malformed_xinput("truncated passive-grab mask");
            mask = *value;
        }
        std::vector<std::uint32_t> modifiers;
        try {
            modifiers.reserve(*modifier_count);
            for (std::uint16_t index = 0; index < *modifier_count; ++index) {
                const auto value = reader.u32();
                if (!value)
                    return malformed_xinput("truncated passive-grab modifiers");
                if (*value != 0x80000000U && (*value & ~0xffU) != 0)
                    return error(bad_value, *value);
                modifiers.push_back(*value);
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (reader.remaining() != 0)
            return error(bad_length);
        WireWriter body(context.order);
        for (const auto modifier : modifiers) {
            PassiveGrab grab;
            grab.kind = *grab_type == 0
                ? PassiveGrabKind::button : PassiveGrabKind::key;
            grab.details = passive_grab_details(
                grab.kind, static_cast<std::uint8_t>(*detail));
            grab.modifiers = passive_grab_modifiers(
                modifier == 0x80000000U
                    ? any_modifier : static_cast<std::uint16_t>(modifier));
            grab.owner = config_.resource_base;
            grab.window = *window_id;
            grab.event_mask = static_cast<std::uint16_t>(
                core_grab_mask(mask, *device));
            grab.pointer_mode = *device == xi2_pointer_device_id
                ? *mode : *paired_mode;
            grab.keyboard_mode = *device == xi2_keyboard_device_id
                ? *mode : *paired_mode;
            grab.owner_events = *owner_events != 0;
            grab.xi2 = true;
            grab.xi2_event_mask = mask;
            if (*cursor_id != 0)
                grab.cursor = server_.cursor(*cursor_id)->image;
            const auto result = server_.add_passive_grab(std::move(grab));
            body.u32(modifier);
            body.u8(result == PassiveGrabUpdate::updated ? 0 : 1);
            body.pad(3);
            if (result == PassiveGrabUpdate::resource_exhausted)
                return error(bad_alloc);
        }
        WireWriter reply(context.order);
        write_reply_header(
            reply, context.sequence,
            static_cast<std::uint32_t>(body.size() / 4));
        reply.u16(static_cast<std::uint16_t>(modifiers.size()));
        reply.pad(22);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 55: { // XIPassiveUngrabDevice
        if (context.request.size() < 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window_id = reader.u32();
        const auto detail = reader.u32();
        const auto device = reader.u16();
        const auto count = reader.u16();
        const auto grab_type = reader.u8();
        if (!window_id || !detail || !device || !count || !grab_type ||
            !reader.skip(3)) {
            return malformed_xinput("truncated XIPassiveUngrabDevice request");
        }
        if (!valid_device(*device))
            return device_error(*device);
        if (*grab_type > 1 || *detail > 255)
            return error(bad_match);
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        for (std::uint16_t index = 0; index < *count; ++index) {
            const auto modifier = reader.u32();
            if (!modifier)
                return malformed_xinput("truncated passive-ungrab modifiers");
            if (*modifier != 0x80000000U && (*modifier & ~0xffU) != 0)
                return error(bad_value, *modifier);
            const auto result = server_.remove_passive_grab(
                *grab_type == 0 ? PassiveGrabKind::button
                                : PassiveGrabKind::key,
                config_.resource_base, *window_id,
                passive_grab_details(
                    *grab_type == 0 ? PassiveGrabKind::button
                                    : PassiveGrabKind::key,
                    static_cast<std::uint8_t>(*detail)),
                passive_grab_modifiers(
                    *modifier == 0x80000000U
                        ? any_modifier
                        : static_cast<std::uint16_t>(*modifier)));
            if (result == PassiveGrabUpdate::resource_exhausted)
                return error(bad_alloc);
        }
        return reader.remaining() == 0
            ? Result<void>::success() : error(bad_length);
    }
    case 56: { // XIListProperties
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xinput("truncated XIListProperties request");
        if (!valid_device(*device))
            return device_error(*device);
        std::vector<AtomId> atoms;
        try {
            for (const auto &entry : server_.xi2_properties(*device))
                atoms.push_back(entry.first);
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        std::sort(atoms.begin(), atoms.end());
        WireWriter reply(context.order);
        write_reply_header(
            reply, context.sequence, static_cast<std::uint32_t>(atoms.size()));
        reply.u16(static_cast<std::uint16_t>(atoms.size()));
        reply.pad(22);
        for (const auto atom : atoms)
            reply.u32(atom);
        return queue(reply.data());
    }
    case 57: { // XIChangeProperty
        if (context.request.size() < 20)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 16, context.order);
        const auto device = reader.u16();
        const auto mode = reader.u8();
        const auto format = reader.u8();
        const auto property = reader.u32();
        const auto type = reader.u32();
        const auto count = reader.u32();
        if (!device || !mode || !format || !property || !type || !count)
            return malformed_xinput("truncated XIChangeProperty request");
        if (!valid_device(*device))
            return device_error(*device);
        if (*mode > 2 || (*format != 8 && *format != 16 && *format != 32))
            return error(bad_value);
        if (!server_.atoms().name(*property) || !server_.atoms().name(*type))
            return error(bad_atom,
                         !server_.atoms().name(*property) ? *property : *type);
        const auto bytes = checked_multiply(
            static_cast<std::size_t>(*count),
            static_cast<std::size_t>(*format / 8));
        const auto padded = bytes ? padded_to_four(*bytes)
                                  : std::optional<std::size_t>{};
        if (!bytes || !padded || context.request.size() != 20 + *padded)
            return error(bad_length);
        const auto canonical = canonical_property_data(
            context.request.data() + 20, *bytes, *format, context.order);
        if (!canonical)
            return malformed_xinput("misaligned XI property data");
        const auto &properties = server_.xi2_properties(*device);
        const auto found = properties.find(*property);
        if (found != properties.end() && *mode != 0 &&
            (found->second.type != *type || found->second.format != *format))
            return error(bad_match);
        PropertyValue value{*type, *format, {}};
        try {
            const std::size_t existing = found == properties.end()
                ? 0 : found->second.data.size();
            value.data.reserve(*mode == 0
                ? canonical->size() : existing + canonical->size());
            if (*mode == 1)
                value.data.insert(value.data.end(), canonical->begin(),
                                  canonical->end());
            if (*mode != 0 && found != properties.end())
                value.data.insert(value.data.end(), found->second.data.begin(),
                                  found->second.data.end());
            if (*mode != 1)
                value.data.insert(value.data.end(), canonical->begin(),
                                  canonical->end());
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        return update(server_.set_xi2_property(
            *device, *property, std::move(value)));
    }
    case 58: { // XIDeleteProperty
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto device = reader.u16();
        const bool padding = reader.skip(2);
        const auto property = reader.u32();
        if (!device || !padding || !property)
            return malformed_xinput("truncated XIDeleteProperty request");
        if (!valid_device(*device))
            return device_error(*device);
        if (!server_.atoms().name(*property))
            return error(bad_atom, *property);
        return update(server_.delete_xi2_property(*device, *property));
    }
    case 59: { // XIGetProperty
        if (context.request.size() != 24)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 20, context.order);
        const auto device = reader.u16();
        const auto erase = reader.u8();
        const bool padding = reader.skip(1);
        const auto property = reader.u32();
        const auto requested_type = reader.u32();
        const auto long_offset = reader.u32();
        const auto long_length = reader.u32();
        if (!device || !erase || !padding || !property || !requested_type ||
            !long_offset || !long_length)
            return malformed_xinput("truncated XIGetProperty request");
        if (!valid_device(*device))
            return device_error(*device);
        if (*erase > 1)
            return error(bad_value, *erase);
        if (!server_.atoms().name(*property) ||
            (*requested_type != 0 && !server_.atoms().name(*requested_type)))
            return error(bad_atom);
        const auto &properties = server_.xi2_properties(*device);
        const auto found = properties.find(*property);
        const PropertyValue *value = found == properties.end()
            ? nullptr : &found->second;
        std::size_t offset = 0;
        std::size_t returned = 0;
        std::size_t after = 0;
        const bool matching = value != nullptr &&
            (*requested_type == 0 || *requested_type == value->type);
        if (value != nullptr) {
            const auto byte_offset = checked_multiply(
                static_cast<std::size_t>(*long_offset), std::size_t{4});
            const auto maximum = checked_multiply(
                static_cast<std::size_t>(*long_length), std::size_t{4});
            if (!byte_offset || !maximum || *byte_offset > value->data.size())
                return error(bad_value, *long_offset);
            offset = *byte_offset;
            if (matching) {
                returned = std::min(*maximum, value->data.size() - offset);
                after = value->data.size() - offset - returned;
            }
            else {
                after = value->data.size();
            }
        }
        const auto encoded = matching && returned != 0
            ? wire_property_data(value->data.data() + offset, returned,
                                 value->format, context.order)
            : std::vector<std::uint8_t>{};
        WireWriter reply(context.order);
        write_reply_header(
            reply, context.sequence,
            static_cast<std::uint32_t>((encoded.size() + 3) / 4));
        reply.u32(value == nullptr ? 0 : value->type);
        reply.u32(static_cast<std::uint32_t>(after));
        const std::size_t unit = value == nullptr ? 1 : value->format / 8;
        reply.u32(static_cast<std::uint32_t>(encoded.size() / unit));
        reply.u8(value == nullptr ? 0 : value->format);
        reply.pad(11);
        reply.bytes(encoded);
        reply.pad_to_four();
        const bool delete_after_reply = *erase != 0 && matching && after == 0;
        if (delete_after_reply) {
            const auto result = server_.delete_xi2_property(
                *device, *property);
            if (result != Xi2Update::updated)
                return error(bad_alloc);
        }
        auto queued = queue(reply.data());
        if (!queued || !delete_after_reply)
            return queued;
        return drain_pending_events();
    }
    case 60: { // XIGetSelectedEvents
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed_xinput("truncated XIGetSelectedEvents request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        const auto *selection = server_.xi2_selection(
            config_.resource_base, *window_id);
        WireWriter body(context.order);
        if (selection != nullptr) {
            for (const auto &mask : selection->masks) {
                body.u16(mask.device);
                body.u16(static_cast<std::uint16_t>(mask.words.size()));
                for (const auto word : mask.words)
                    body.u32(word);
            }
        }
        WireWriter reply(context.order);
        write_reply_header(
            reply, context.sequence,
            static_cast<std::uint32_t>(body.size() / 4));
        reply.u16(selection == nullptr
            ? 0 : static_cast<std::uint16_t>(selection->masks.size()));
        reply.pad(22);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 61: { // XIBarrierReleasePointer
        if (context.request.size() < 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto count = reader.u32();
        if (!count)
            return malformed_xinput("truncated XIBarrierReleasePointer request");
        const auto expected = checked_multiply(
            static_cast<std::size_t>(*count), std::size_t{12});
        if (!expected || reader.remaining() != *expected)
            return error(bad_length);
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto device = reader.u16();
            const bool padding = reader.skip(2);
            const auto barrier = reader.u32();
            const auto event = reader.u32();
            if (!device || !padding || !barrier || !event)
                return malformed_xinput("truncated barrier release");
            if (*device != xi2_pointer_device_id)
                return device_error(*device);
            if (server_.xfixes_barrier(*barrier) == nullptr)
                return error(bad_value, *barrier);
        }
        return Result<void>::success();
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
