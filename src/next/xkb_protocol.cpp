#include "xmin/next/connection.hpp"

#include "xmin/next/extension_registry.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <string_view>

namespace xmin::next {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_atom = 5,
    bad_match = 8,
    bad_alloc = 11,
    bad_length = 16,
};

constexpr std::uint16_t use_core_keyboard = 0x0100;
constexpr std::uint16_t default_xi_device = 0x0400;
constexpr std::uint16_t all_xkb_events = 0x0fff;
constexpr std::uint16_t all_map_parts = 0x00ff;
constexpr std::uint32_t supported_name_parts =
    (1U << 0) | (1U << 2) | (1U << 4) | (1U << 5) |
    (1U << 6) | (1U << 7) | (1U << 8) | (1U << 9) |
    (1U << 11) | (1U << 12);
constexpr std::uint32_t controls_groups_wrap = 1U << 27;
constexpr std::uint32_t controls_internal_mods = 1U << 28;
constexpr std::uint32_t controls_ignore_lock_mods = 1U << 29;
constexpr std::uint32_t controls_per_key_repeat = 1U << 30;
constexpr std::uint32_t controls_enabled = 1U << 31;
constexpr std::uint32_t all_control_changes =
    controls_groups_wrap | controls_internal_mods |
    controls_ignore_lock_mods | controls_per_key_repeat |
    controls_enabled;

Result<void>
malformed_xkb(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

bool
valid_keyboard_device(std::uint16_t device) noexcept
{
    return device == use_core_keyboard ||
        device == default_xi_device || device == xkb_keyboard_device_id;
}

std::uint8_t
key_type(std::uint32_t first, std::uint32_t second,
         std::uint8_t width) noexcept
{
    if (width <= 1)
        return 0; // ONE_LEVEL
    if (first >= 'a' && first <= 'z' && second == first - 'a' + 'A')
        return 2; // ALPHABETIC
    return 1; // TWO_LEVEL
}

void
write_key_type(WireWriter &body, std::uint8_t type)
{
    if (type == 0) {
        body.u8(0);
        body.u8(0);
        body.u16(0);
        body.u8(1);
        body.u8(0);
        body.u8(0);
        body.u8(0);
        return;
    }
    const std::uint8_t mask = type == 1 ? 1 : 3;
    body.u8(mask);
    body.u8(mask);
    body.u16(0);
    body.u8(2);
    body.u8(type == 1 ? 1 : 2);
    body.u8(0);
    body.u8(0);
    const auto entry = [&body](std::uint8_t mods) {
        body.u8(1);
        body.u8(mods);
        body.u8(1);
        body.u8(mods);
        body.u16(0);
        body.pad(2);
    };
    entry(1); // Shift
    if (type == 2)
        entry(2); // Lock
}

std::uint8_t
indicator_modifier(std::size_t index) noexcept
{
    constexpr std::array<std::uint8_t, 3> modifiers{{
        1U << 1, 1U << 4, 1U << 5}};
    return modifiers[index];
}

void
write_indicator_map(WireWriter &writer, std::size_t index)
{
    writer.u8(0x80); // no automatic state changes
    writer.u8(0);
    writer.u8(0);
    writer.u8(1U << 2); // use locked modifiers
    writer.u8(indicator_modifier(index));
    writer.u8(indicator_modifier(index));
    writer.u16(0);
    writer.u32(0);
}

} // namespace

Result<void>
Connection::handle_xkb(const RequestContext &context)
{
    constexpr std::uint8_t bad_keyboard = xkb_extension.first_error;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto keyboard_error = [&](std::uint16_t device) {
        return error(bad_keyboard,
                     0xff000000U | static_cast<std::uint32_t>(device));
    };
    const auto update = [&](XkbUpdate result) {
        switch (result) {
        case XkbUpdate::updated:
            return drain_pending_events();
        case XkbUpdate::invalid:
            return error(bad_value);
        case XkbUpdate::resource_exhausted:
        case XkbUpdate::queue_full:
            return error(bad_alloc);
        }
        return error(bad_request);
    };
    if (context.data != 0 && !xkb_version_negotiated_)
        return error(bad_request);

    switch (context.data) {
    case 0: { // UseExtension
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto major = reader.u16();
        const auto minor = reader.u16();
        if (!major || !minor)
            return malformed_xkb("truncated XKB UseExtension request");
        const bool supported = *major < xkb_extension.major_version ||
            (*major == xkb_extension.major_version &&
             *minor <= xkb_extension.minor_version);
        xkb_version_negotiated_ = supported;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(supported ? 1 : 0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(xkb_extension.major_version);
        reply.u16(xkb_extension.minor_version);
        reply.pad(20);
        return queue(reply.data());
    }
    case 1: { // SelectEvents
        if (context.request.size() < 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto device = reader.u16();
        const auto affect = reader.u16();
        const auto clear = reader.u16();
        const auto select_all = reader.u16();
        const auto affect_map = reader.u16();
        const auto map = reader.u16();
        if (!device || !affect || !clear || !select_all ||
            !affect_map || !map) {
            return malformed_xkb("truncated XKB SelectEvents request");
        }
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if (((*affect | *clear | *select_all) & ~all_xkb_events) != 0 ||
            ((*affect_map | *map) & ~all_map_parts) != 0 ||
            (*clear & *select_all) != 0) {
            return error(bad_value);
        }

        XkbEventSelection selection;
        if (const auto *existing =
                server_.xkb_selection(config_.resource_base)) {
            selection = *existing;
        }
        selection.owner = config_.resource_base;
        selection.events = static_cast<std::uint16_t>(
            (selection.events & ~*affect) | (*select_all & *affect));
        selection.events = static_cast<std::uint16_t>(
            selection.events & ~(*clear & *affect));
        const std::uint16_t detailed = static_cast<std::uint16_t>(
            *affect & ~*clear & ~*select_all);
        const auto detail16 = [&](std::uint16_t bit,
                                  std::uint16_t &selected) -> bool {
            if ((detailed & bit) == 0)
                return true;
            const auto detail_affect = reader.u16();
            const auto value = reader.u16();
            if (!detail_affect || !value)
                return false;
            selected = static_cast<std::uint16_t>(
                (selected & ~*detail_affect) | (*value & *detail_affect));
            if (selected == 0)
                selection.events &= static_cast<std::uint16_t>(~bit);
            else
                selection.events |= bit;
            return true;
        };
        const auto detail32 = [&](std::uint16_t bit,
                                  std::uint32_t &selected) -> bool {
            if ((detailed & bit) == 0)
                return true;
            const auto detail_affect = reader.u32();
            const auto value = reader.u32();
            if (!detail_affect || !value)
                return false;
            selected = (selected & ~*detail_affect) |
                (*value & *detail_affect);
            if (selected == 0)
                selection.events &= static_cast<std::uint16_t>(~bit);
            else
                selection.events |= bit;
            return true;
        };
        const auto detail8 = [&](std::uint16_t bit,
                                 std::uint8_t &selected) -> bool {
            if ((detailed & bit) == 0)
                return true;
            const auto detail_affect = reader.u8();
            const auto value = reader.u8();
            if (!detail_affect || !value)
                return false;
            selected = static_cast<std::uint8_t>(
                (selected & ~*detail_affect) | (*value & *detail_affect));
            if (selected == 0)
                selection.events &= static_cast<std::uint16_t>(~bit);
            else
                selection.events |= bit;
            return true;
        };
        if (!detail16(1U << 0, selection.new_keyboard))
            return malformed_xkb("truncated XKB NewKeyboard details");
        if ((detailed & (1U << 1)) != 0) {
            selection.map = static_cast<std::uint16_t>(
                (selection.map & ~*affect_map) | (*map & *affect_map));
            if (selection.map == 0)
                selection.events &= static_cast<std::uint16_t>(~(1U << 1));
            else
                selection.events |= 1U << 1;
        }
        if (!detail16(1U << 2, selection.state) ||
            !detail32(1U << 3, selection.controls) ||
            !detail32(1U << 4, selection.indicator_state) ||
            !detail32(1U << 5, selection.indicator_map) ||
            !detail16(1U << 6, selection.names) ||
            !detail8(1U << 7, selection.compatibility) ||
            !detail8(1U << 8, selection.bell) ||
            !detail8(1U << 9, selection.action_message) ||
            !detail16(1U << 10, selection.access_x) ||
            !detail16(1U << 11, selection.extension_device)) {
            return malformed_xkb("truncated XKB event details");
        }
        if (reader.remaining() != 0)
            return error(bad_length);
        return update(server_.select_xkb_events(selection));
    }
    case 3: { // Bell: this headless profile has no audible sink.
        if (context.request.size() != 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 24, context.order);
        const auto device = reader.u16();
        if (!device)
            return malformed_xkb("truncated XKB Bell request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        return Result<void>::success();
    }
    case 4: { // GetState
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB GetState request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const auto state = server_.xkb_state();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(state.mods);
        reply.u8(state.base_mods);
        reply.u8(state.latched_mods);
        reply.u8(state.locked_mods);
        reply.u8(state.group);
        reply.u8(state.locked_group);
        reply.i16(state.base_group);
        reply.i16(state.latched_group);
        reply.u8(state.mods);
        reply.u8(state.mods);
        reply.u8(state.mods);
        reply.u8(state.mods);
        reply.u8(state.mods);
        reply.pad(1);
        reply.u16(state.pointer_buttons);
        reply.pad(6);
        return queue(reply.data());
    }
    case 5: { // LatchLockState
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto device = reader.u16();
        const auto affect_locks = reader.u8();
        const auto locks = reader.u8();
        const auto lock_group = reader.u8();
        const auto group_lock = reader.u8();
        const auto affect_latches = reader.u8();
        if (!device || !affect_locks || !locks || !lock_group ||
            !group_lock || !affect_latches || !reader.skip(1) ||
            !reader.skip(1)) {
            return malformed_xkb("truncated XKB LatchLockState request");
        }
        const auto latch_group = reader.u8();
        const auto group_latch = reader.u16();
        if (!latch_group || !group_latch)
            return malformed_xkb("truncated XKB group latch");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if (*lock_group > 1 || *latch_group > 1 || *group_lock > 3)
            return error(bad_value);
        return update(server_.latch_lock_xkb(
            *affect_locks, *locks, *lock_group != 0, *group_lock,
            *affect_latches, *latch_group != 0,
            static_cast<std::int16_t>(*group_latch), context.opcode,
            context.data));
    }
    case 6: { // GetControls
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB GetControls request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const auto &controls = server_.input().xkb.controls;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(15);
        reply.u8(controls.mouse_keys_default_button);
        reply.u8(1);
        reply.u8(controls.groups_wrap);
        reply.u8(controls.internal_mods);
        reply.u8(controls.ignore_lock_mods);
        reply.u8(controls.internal_mods);
        reply.u8(controls.ignore_lock_mods);
        reply.pad(1);
        reply.u16(controls.internal_virtual_mods);
        reply.u16(controls.ignore_lock_virtual_mods);
        reply.u16(controls.repeat_delay);
        reply.u16(controls.repeat_interval);
        reply.u16(controls.slow_keys_delay);
        reply.u16(controls.debounce_delay);
        reply.u16(controls.mouse_keys_delay);
        reply.u16(controls.mouse_keys_interval);
        reply.u16(controls.mouse_keys_time_to_max);
        reply.u16(controls.mouse_keys_max_speed);
        reply.i16(controls.mouse_keys_curve);
        reply.u16(controls.access_x_options);
        reply.u16(controls.access_x_timeout);
        reply.u16(controls.access_x_timeout_options_mask);
        reply.u16(controls.access_x_timeout_options_values);
        reply.pad(2);
        reply.u32(controls.access_x_timeout_mask);
        reply.u32(controls.access_x_timeout_values);
        reply.u32(controls.enabled);
        for (const auto value : controls.per_key_repeat)
            reply.u8(value);
        return queue(reply.data());
    }
    case 7: { // SetControls
        if (context.request.size() != 100)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 96, context.order);
        const auto device = reader.u16();
        const auto affect_internal = reader.u8();
        const auto internal = reader.u8();
        const auto affect_ignore = reader.u8();
        const auto ignore = reader.u8();
        const auto affect_internal_virtual = reader.u16();
        const auto internal_virtual = reader.u16();
        const auto affect_ignore_virtual = reader.u16();
        const auto ignore_virtual = reader.u16();
        const auto default_button = reader.u8();
        const auto groups_wrap = reader.u8();
        const auto access_options = reader.u16();
        if (!device || !affect_internal || !internal || !affect_ignore ||
            !ignore || !affect_internal_virtual || !internal_virtual ||
            !affect_ignore_virtual || !ignore_virtual || !default_button ||
            !groups_wrap || !access_options || !reader.skip(2)) {
            return malformed_xkb("truncated XKB SetControls request");
        }
        const auto affect_enabled = reader.u32();
        const auto enabled = reader.u32();
        const auto changed = reader.u32();
        const auto repeat_delay = reader.u16();
        const auto repeat_interval = reader.u16();
        const auto slow_delay = reader.u16();
        const auto debounce = reader.u16();
        const auto mouse_delay = reader.u16();
        const auto mouse_interval = reader.u16();
        const auto mouse_to_max = reader.u16();
        const auto mouse_speed = reader.u16();
        const auto mouse_curve = reader.u16();
        const auto access_timeout = reader.u16();
        const auto access_mask = reader.u32();
        const auto access_values = reader.u32();
        const auto access_options_mask = reader.u16();
        const auto access_options_values = reader.u16();
        if (!affect_enabled || !enabled || !changed || !repeat_delay ||
            !repeat_interval || !slow_delay || !debounce || !mouse_delay ||
            !mouse_interval || !mouse_to_max || !mouse_speed ||
            !mouse_curve || !access_timeout || !access_mask ||
            !access_values || !access_options_mask ||
            !access_options_values) {
            return malformed_xkb("truncated XKB control values");
        }
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if ((*changed & ~all_control_changes) != 0 ||
            ((*changed & controls_enabled) != 0 &&
             (*affect_enabled & ~xkb_supported_boolean_controls) != 0)) {
            return error(bad_match);
        }
        XkbControls controls = server_.input().xkb.controls;
        if ((*changed & controls_internal_mods) != 0) {
            controls.internal_mods = static_cast<std::uint8_t>(
                (controls.internal_mods & ~*affect_internal) |
                (*internal & *affect_internal));
            controls.internal_virtual_mods = static_cast<std::uint16_t>(
                (controls.internal_virtual_mods & ~*affect_internal_virtual) |
                (*internal_virtual & *affect_internal_virtual));
        }
        if ((*changed & controls_ignore_lock_mods) != 0) {
            controls.ignore_lock_mods = static_cast<std::uint8_t>(
                (controls.ignore_lock_mods & ~*affect_ignore) |
                (*ignore & *affect_ignore));
            controls.ignore_lock_virtual_mods = static_cast<std::uint16_t>(
                (controls.ignore_lock_virtual_mods &
                 ~*affect_ignore_virtual) |
                (*ignore_virtual & *affect_ignore_virtual));
        }
        if ((*changed & controls_groups_wrap) != 0) {
            controls.groups_wrap = *groups_wrap;
            controls.mouse_keys_default_button = *default_button;
            controls.access_x_options = *access_options;
        }
        if ((*changed & controls_per_key_repeat) != 0) {
            if (*repeat_delay == 0 || *repeat_interval == 0)
                return error(bad_value);
            controls.repeat_delay = *repeat_delay;
            controls.repeat_interval = *repeat_interval;
            controls.slow_keys_delay = *slow_delay;
            controls.debounce_delay = *debounce;
            controls.mouse_keys_delay = *mouse_delay;
            controls.mouse_keys_interval = *mouse_interval;
            controls.mouse_keys_time_to_max = *mouse_to_max;
            controls.mouse_keys_max_speed = *mouse_speed;
            controls.mouse_keys_curve = static_cast<std::int16_t>(*mouse_curve);
            controls.access_x_timeout = *access_timeout;
            controls.access_x_timeout_mask = *access_mask;
            controls.access_x_timeout_values = *access_values;
            controls.access_x_timeout_options_mask = *access_options_mask;
            controls.access_x_timeout_options_values =
                *access_options_values;
            for (auto &value : controls.per_key_repeat) {
                const auto byte = reader.u8();
                if (!byte)
                    return malformed_xkb("truncated XKB repeat bitmap");
                value = *byte;
            }
        }
        else if (!reader.skip(32)) {
            return malformed_xkb("truncated XKB repeat bitmap");
        }
        std::uint32_t enabled_changes = 0;
        if ((*changed & controls_enabled) != 0) {
            const std::uint32_t previous = controls.enabled;
            controls.enabled = (controls.enabled & ~*affect_enabled) |
                (*enabled & *affect_enabled);
            enabled_changes = previous ^ controls.enabled;
        }
        return update(server_.set_xkb_controls(
            controls, *changed, enabled_changes,
            context.opcode, context.data));
    }
    case 8: { // GetMap
        if (context.request.size() != 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 24, context.order);
        const auto device = reader.u16();
        const auto full = reader.u16();
        const auto partial = reader.u16();
        const auto first_type = reader.u8();
        const auto type_count = reader.u8();
        const auto first_sym = reader.u8();
        const auto sym_count = reader.u8();
        const auto first_action = reader.u8();
        const auto action_count = reader.u8();
        const auto first_behavior = reader.u8();
        const auto behavior_count = reader.u8();
        const auto virtual_mods = reader.u16();
        const auto first_explicit = reader.u8();
        const auto explicit_count = reader.u8();
        const auto first_modmap = reader.u8();
        const auto modmap_count = reader.u8();
        const auto first_vmodmap = reader.u8();
        const auto vmodmap_count = reader.u8();
        if (!device || !full || !partial || !first_type || !type_count ||
            !first_sym || !sym_count || !first_action || !action_count ||
            !first_behavior || !behavior_count || !virtual_mods ||
            !first_explicit || !explicit_count || !first_modmap ||
            !modmap_count || !first_vmodmap || !vmodmap_count ||
            !reader.skip(2)) {
            return malformed_xkb("truncated XKB GetMap request");
        }
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const std::uint16_t present = static_cast<std::uint16_t>(
            (*full | *partial) & all_map_parts);
        if (((*full | *partial) & ~all_map_parts) != 0)
            return error(bad_value);
        const auto key_range = [&](bool complete, std::uint8_t requested_first,
                                   std::uint8_t requested_count) {
            std::pair<std::uint8_t, std::uint8_t> result{
                requested_first, requested_count};
            if (complete) {
                result.first = minimum_keycode;
                result.second = static_cast<std::uint8_t>(
                    maximum_keycode - minimum_keycode + 1U);
            }
            return result;
        };
        const auto types = (*full & (1U << 0)) != 0
            ? std::pair<std::uint8_t, std::uint8_t>{0, 3}
            : std::pair<std::uint8_t, std::uint8_t>{*first_type, *type_count};
        if ((present & (1U << 0)) != 0 &&
            (types.first > 3 || types.second > 3 - types.first)) {
            return error(bad_value);
        }
        const auto syms = key_range((*full & (1U << 1)) != 0,
                                    *first_sym, *sym_count);
        const auto actions = key_range((*full & (1U << 4)) != 0,
                                       *first_action, *action_count);
        const auto behaviors = key_range((*full & (1U << 5)) != 0,
                                         *first_behavior, *behavior_count);
        const auto explicit_parts = key_range((*full & (1U << 3)) != 0,
                                              *first_explicit,
                                              *explicit_count);
        const auto modmap = key_range((*full & (1U << 2)) != 0,
                                      *first_modmap, *modmap_count);
        const auto vmodmap = key_range((*full & (1U << 7)) != 0,
                                       *first_vmodmap, *vmodmap_count);
        const auto valid_keys = [](const auto &range) {
            return range.second == 0 ||
                (range.first >= minimum_keycode &&
                 static_cast<unsigned>(range.first) + range.second - 1U <=
                     maximum_keycode);
        };
        if (!valid_keys(syms) || !valid_keys(actions) ||
            !valid_keys(behaviors) || !valid_keys(explicit_parts) ||
            !valid_keys(modmap) || !valid_keys(vmodmap)) {
            return error(bad_value);
        }

        WireWriter body(context.order);
        if ((present & (1U << 0)) != 0) {
            for (std::uint8_t index = types.first;
                 index < types.first + types.second; ++index) {
                write_key_type(body, index);
            }
        }
        std::uint16_t total_syms = 0;
        if ((present & (1U << 1)) != 0) {
            const auto &input = server_.input();
            for (unsigned offset = 0; offset < syms.second; ++offset) {
                const std::uint8_t key = static_cast<std::uint8_t>(
                    syms.first + offset);
                const auto row = input.keymap.begin() +
                    static_cast<std::size_t>(key) * input.keymap_width;
                std::uint8_t width = static_cast<std::uint8_t>(std::min<
                    std::size_t>(2, input.keymap_row_widths[key]));
                while (width != 0 && row[width - 1] == 0)
                    --width;
                const std::uint32_t first = width == 0 ? 0 : row[0];
                const std::uint32_t second = width < 2 ? 0 : row[1];
                body.u8(key_type(first, second, width));
                body.u8(0);
                body.u8(0);
                body.u8(0);
                body.u8(width == 0 ? 0 : 1); // one group
                body.u8(width);
                body.u16(width);
                for (std::uint8_t level = 0; level < width; ++level)
                    body.u32(row[level]);
                total_syms = static_cast<std::uint16_t>(
                    total_syms + width);
            }
        }
        if ((present & (1U << 4)) != 0) {
            body.pad(actions.second); // no server-side key actions
            body.pad_to_four();
        }
        std::vector<std::pair<std::uint8_t, std::uint8_t>> modifier_entries;
        if ((present & (1U << 2)) != 0) {
            try {
                for (unsigned offset = 0; offset < modmap.second; ++offset) {
                    const std::uint8_t key = static_cast<std::uint8_t>(
                        modmap.first + offset);
                    std::uint8_t mask = 0;
                    const auto &input = server_.input();
                    for (std::size_t group = 0; group < 8; ++group) {
                        for (std::size_t index = 0;
                             index < input.modifier_keys_per_group; ++index) {
                            if (input.modifier_map[
                                    group * input.modifier_keys_per_group +
                                    index] == key) {
                                mask |= static_cast<std::uint8_t>(1U << group);
                            }
                        }
                    }
                    if (mask != 0)
                        modifier_entries.emplace_back(key, mask);
                }
            }
            catch (const std::bad_alloc &) {
                return error(bad_alloc);
            }
            for (const auto &entry : modifier_entries) {
                body.u8(entry.first);
                body.u8(entry.second);
            }
            body.pad_to_four();
        }

        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>((8 + body.size()) / 4));
        reply.pad(2);
        reply.u8(minimum_keycode);
        reply.u8(maximum_keycode);
        reply.u16(present);
        reply.u8(types.first);
        reply.u8((present & (1U << 0)) != 0 ? types.second : 0);
        reply.u8(3);
        reply.u8(syms.first);
        reply.u16(total_syms);
        reply.u8((present & (1U << 1)) != 0 ? syms.second : 0);
        reply.u8(actions.first);
        reply.u16(0);
        reply.u8((present & (1U << 4)) != 0 ? actions.second : 0);
        reply.u8(behaviors.first);
        reply.u8((present & (1U << 5)) != 0 ? behaviors.second : 0);
        reply.u8(0);
        reply.u8(explicit_parts.first);
        reply.u8((present & (1U << 3)) != 0 ? explicit_parts.second : 0);
        reply.u8(0);
        reply.u8(modmap.first);
        reply.u8((present & (1U << 2)) != 0 ? modmap.second : 0);
        reply.u8(static_cast<std::uint8_t>(modifier_entries.size()));
        reply.u8(vmodmap.first);
        reply.u8((present & (1U << 7)) != 0 ? vmodmap.second : 0);
        reply.u8(0);
        reply.pad(1);
        reply.u16(0);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 9: // SetMap: the embedded product map is immutable.
    case 11: // SetCompatMap
    case 14: // SetIndicatorMap
    case 18: // SetNames
    case 25: // SetDeviceInfo
        if (context.request.size() < 8 ||
            (context.request.size() & 3U) != 0) {
            return error(bad_length);
        }
        return error(bad_match);
    case 10: { // GetCompatMap: no compatibility actions are required.
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto device = reader.u16();
        const auto groups = reader.u8();
        const auto all = reader.u8();
        const auto first = reader.u16();
        const auto count = reader.u16();
        if (!device || !groups || !all || !first || !count)
            return malformed_xkb("truncated XKB GetCompatMap request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if ((*groups & ~0x0fU) != 0)
            return error(bad_value);
        const std::uint8_t returned_groups = *groups;
        const std::size_t group_count =
            std::bitset<8>(returned_groups).count();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(group_count));
        reply.u8(returned_groups);
        reply.pad(1);
        reply.u16(*first);
        reply.u16(0);
        reply.u16(0);
        reply.pad(16);
        for (std::size_t index = 0; index < group_count; ++index) {
            reply.u8(0);
            reply.u8(0);
            reply.u16(0);
        }
        return queue(reply.data());
    }
    case 12: { // GetIndicatorState
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB GetIndicatorState request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.xkb_indicator_state());
        reply.pad(20);
        return queue(reply.data());
    }
    case 13: { // GetIndicatorMap
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB GetIndicatorMap request");
        const auto requested = reader.u32();
        if (!requested)
            return malformed_xkb("truncated XKB indicator mask");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const std::uint32_t which = *requested & 0x7U;
        const auto count = std::bitset<32>(which).count();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(count * 3));
        reply.u32(which);
        reply.u32(0x7);
        reply.u8(static_cast<std::uint8_t>(count));
        reply.pad(15);
        for (std::size_t index = 0; index < 3; ++index) {
            if ((which & (1U << index)) != 0)
                write_indicator_map(reply, index);
        }
        return queue(reply.data());
    }
    case 15: { // GetNamedIndicator
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(6))
            return malformed_xkb("truncated XKB GetNamedIndicator request");
        const auto atom = reader.u32();
        if (!atom)
            return malformed_xkb("truncated XKB indicator atom");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const auto name = server_.atoms().name(*atom);
        if (!name)
            return error(bad_atom, *atom);
        constexpr std::array<std::string_view, 3> names{{
            "Caps Lock", "Num Lock", "Scroll Lock"}};
        const auto found = std::find(names.begin(), names.end(), *name);
        const bool exists = found != names.end();
        const std::size_t index = exists
            ? static_cast<std::size_t>(found - names.begin())
            : 0;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(*atom);
        reply.u8(exists ? 1 : 0);
        reply.u8(exists &&
                         (server_.xkb_indicator_state() & (1U << index)) != 0
                     ? 1
                     : 0);
        reply.u8(exists ? 1 : 0);
        reply.u8(exists ? static_cast<std::uint8_t>(index + 1) : 0);
        if (exists) {
            reply.u8(0x80);
            reply.u8(0);
            reply.u8(0);
            reply.u8(1U << 2);
            reply.u8(indicator_modifier(index));
            reply.u8(indicator_modifier(index));
        }
        else {
            reply.pad(6);
        }
        reply.u16(0);
        reply.u32(0);
        reply.u8(exists ? 1 : 0);
        reply.pad(3);
        return queue(reply.data());
    }
    case 16: { // SetNamedIndicator state; maps remain immutable.
        if (context.request.size() != 32)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 28, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(6))
            return malformed_xkb("truncated XKB SetNamedIndicator request");
        const auto atom = reader.u32();
        const auto set_state = reader.u8();
        const auto on = reader.u8();
        const auto set_map = reader.u8();
        const auto create_map = reader.u8();
        if (!atom || !set_state || !on || !set_map || !create_map)
            return malformed_xkb("truncated XKB named indicator values");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if (*set_map != 0 || *create_map != 0)
            return error(bad_match);
        if (*set_state == 0)
            return Result<void>::success();
        const auto name = server_.atoms().name(*atom);
        if (!name)
            return error(bad_atom, *atom);
        constexpr std::array<std::string_view, 3> names{{
            "Caps Lock", "Num Lock", "Scroll Lock"}};
        const auto found = std::find(names.begin(), names.end(), *name);
        if (found == names.end())
            return error(bad_match);
        const std::size_t index = static_cast<std::size_t>(
            found - names.begin());
        const std::uint8_t mask = indicator_modifier(index);
        return update(server_.latch_lock_xkb(
            mask, *on != 0 ? mask : 0, false, 0, 0, false, 0,
            context.opcode, context.data));
    }
    case 17: { // GetNames
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB GetNames request");
        const auto requested = reader.u32();
        if (!requested)
            return malformed_xkb("truncated XKB names mask");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        const std::uint32_t which = *requested & supported_name_parts;
        struct Names {
            AtomId keycodes = 0;
            AtomId symbols = 0;
            AtomId types = 0;
            AtomId compatibility = 0;
            std::array<AtomId, 3> type_names{};
            std::array<AtomId, 5> level_names{};
            std::array<AtomId, 3> indicators{};
            AtomId group = 0;
        } names;
        try {
            names.keycodes = server_.atoms().intern("xmin(pc105)");
            names.symbols = server_.atoms().intern("pc+us");
            names.types = server_.atoms().intern("xmin");
            names.compatibility = server_.atoms().intern("xmin");
            names.type_names = {{
                server_.atoms().intern("ONE_LEVEL"),
                server_.atoms().intern("TWO_LEVEL"),
                server_.atoms().intern("ALPHABETIC")}};
            const AtomId level1 = server_.atoms().intern("Level1");
            const AtomId level2 = server_.atoms().intern("Level2");
            names.level_names = {{
                level1, level1, level2, level1, level2}};
            names.indicators = {{
                server_.atoms().intern("Caps Lock"),
                server_.atoms().intern("Num Lock"),
                server_.atoms().intern("Scroll Lock")}};
            names.group = server_.atoms().intern("English (US)");
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        if (names.keycodes == 0 || names.symbols == 0 ||
            names.types == 0 || names.compatibility == 0 ||
            names.group == 0) {
            return error(bad_alloc);
        }

        WireWriter body(context.order);
        if ((which & (1U << 0)) != 0)
            body.u32(names.keycodes);
        if ((which & (1U << 2)) != 0)
            body.u32(names.symbols);
        if ((which & (1U << 4)) != 0)
            body.u32(names.types);
        if ((which & (1U << 5)) != 0)
            body.u32(names.compatibility);
        if ((which & (1U << 6)) != 0) {
            for (const auto atom : names.type_names)
                body.u32(atom);
        }
        if ((which & (1U << 7)) != 0) {
            body.u8(1);
            body.u8(2);
            body.u8(2);
            body.pad_to_four();
            for (const auto atom : names.level_names)
                body.u32(atom);
        }
        if ((which & (1U << 8)) != 0) {
            for (const auto atom : names.indicators)
                body.u32(atom);
        }
        if ((which & (1U << 12)) != 0)
            body.u32(names.group);
        if ((which & (1U << 9)) != 0) {
            constexpr std::string_view digits =
                "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            std::array<char, 4> name{{'K', '0', '0', '\0'}};
            for (unsigned key = minimum_keycode;
                 key <= maximum_keycode; ++key) {
                name[1] = digits[(key / digits.size()) % digits.size()];
                name[2] = digits[key % digits.size()];
                body.bytes(std::string_view{name.data(), name.size()});
            }
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(body.size() / 4));
        reply.u32(which);
        reply.u8(minimum_keycode);
        reply.u8(maximum_keycode);
        reply.u8(3);
        reply.u8(1);
        reply.u16(0);
        reply.u8(minimum_keycode);
        reply.u8(static_cast<std::uint8_t>(
            maximum_keycode - minimum_keycode + 1U));
        reply.u32(0x7);
        reply.u8(0);
        reply.u8(0);
        reply.u16(5);
        reply.pad(4);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 21: { // PerClientFlags
        if (context.request.size() != 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 24, context.order);
        const auto device = reader.u16();
        if (!device || !reader.skip(2))
            return malformed_xkb("truncated XKB PerClientFlags request");
        const auto change = reader.u32();
        const auto value = reader.u32();
        const auto controls_change = reader.u32();
        const auto auto_controls = reader.u32();
        const auto auto_values = reader.u32();
        if (!change || !value || !controls_change || !auto_controls ||
            !auto_values) {
            return malformed_xkb("truncated XKB client flags");
        }
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        if ((*change & ~xkb_supported_client_flags) != 0 ||
            *controls_change != 0 || *auto_controls != 0 ||
            *auto_values != 0) {
            return error(bad_match);
        }
        const std::uint32_t flags =
            (server_.xkb_client_flags(config_.resource_base) & ~*change) |
            (*value & *change);
        const auto result = server_.set_xkb_client_flags(
            config_.resource_base, flags);
        if (result != XkbUpdate::updated)
            return update(result);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(xkb_supported_client_flags);
        reply.u32(flags);
        reply.u32(0);
        reply.u32(0);
        reply.pad(8);
        return queue(reply.data());
    }
    case 22: { // ListComponents: there is no runtime component database.
        if (context.request.size() < 12 ||
            (context.request.size() & 3U) != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto device = reader.u16();
        if (!device)
            return malformed_xkb("truncated XKB ListComponents request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        return queue(reply.data());
    }
    case 23: { // GetKbdByName: fixed maps cannot be loaded by name.
        if (context.request.size() < 12 ||
            (context.request.size() & 3U) != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto device = reader.u16();
        if (!device)
            return malformed_xkb("truncated XKB GetKbdByName request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(minimum_keycode);
        reply.u8(maximum_keycode);
        reply.u8(0);
        reply.u8(0);
        reply.u16(0);
        reply.u16(0);
        reply.pad(16);
        return queue(reply.data());
    }
    case 24: { // GetDeviceInfo
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto device = reader.u16();
        const auto wanted = reader.u16();
        if (!device || !wanted)
            return malformed_xkb("truncated XKB GetDeviceInfo request");
        if (!valid_keyboard_device(*device))
            return keyboard_error(*device);
        constexpr std::string_view name = "Xmin virtual core keyboard";
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(xkb_keyboard_device_id);
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>((name.size() + 3) / 4));
        reply.u16(*wanted & 1U);
        reply.u16(1);
        reply.u16(*wanted & ~1U);
        reply.u16(0);
        reply.u8(0);
        reply.u8(0);
        reply.u8(0);
        reply.u8(0);
        reply.u8(0);
        reply.u8(0);
        reply.u16(0);
        reply.u16(0);
        reply.pad(2);
        reply.u32(0);
        reply.u16(static_cast<std::uint16_t>(name.size()));
        reply.bytes(name);
        reply.pad_to_four();
        return queue(reply.data());
    }
    case 101: { // SetDebuggingFlags: no runtime debugging controls.
        if (context.request.size() < 24 ||
            (context.request.size() & 3U) != 0) {
            return error(bad_length);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        return queue(reply.data());
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::next
