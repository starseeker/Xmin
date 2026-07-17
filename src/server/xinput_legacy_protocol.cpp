#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_atom = 5,
    bad_alloc = 11,
    bad_length = 16,
};

Result<void>
malformed_xi1(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

bool
xi1_device(std::uint8_t device) noexcept
{
    return device == 4 || device == 5;
}

void
reply_header(WireWriter &reply, std::uint16_t sequence,
             std::uint32_t words = 0)
{
    reply.u8(1);
    reply.u8(0);
    reply.u16(sequence);
    reply.u32(words);
}

} // namespace

Result<void>
Connection::handle_xinput_legacy(const RequestContext &context)
{
    constexpr std::uint8_t bad_device = xinput_extension.first_error;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto require_device = [&](std::uint8_t device) -> Result<void> {
        return xi1_device(device)
            ? Result<void>::success()
            : error(bad_device, device);
    };
    const auto simple_status_reply = [&]() {
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    };

    switch (context.data) {
    case 1: { // GetExtensionVersion
        if (context.request.size() < 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto length = reader.u16();
        if (!length || !reader.skip(2))
            return malformed_xi1("truncated XI1 version request");
        const auto padded = padded_to_four(*length);
        if (!padded || context.request.size() != 8 + *padded)
            return error(bad_length);
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u16(2);
        reply.u16(4);
        reply.u8(1);
        reply.pad(19);
        return queue(reply.data());
    }
    case 2: { // ListInputDevices
        if (context.request.size() != 4)
            return error(bad_length);
        const AtomId pointer_type = server_.atoms().intern("Xmin Pointer");
        const AtomId keyboard_type = server_.atoms().intern("Xmin Keyboard");
        if (pointer_type == 0 || keyboard_type == 0)
            return error(bad_alloc);
        constexpr std::string_view pointer_name = "Xmin virtual pointer";
        constexpr std::string_view keyboard_name = "Xmin virtual keyboard";
        WireWriter body(context.order);
        body.u32(pointer_type);
        body.u8(4);
        body.u8(0);
        body.u8(4);
        body.u8(0);
        body.u32(keyboard_type);
        body.u8(5);
        body.u8(0);
        body.u8(3);
        body.u8(0);
        body.u8(static_cast<std::uint8_t>(pointer_name.size()));
        body.bytes(pointer_name);
        body.u8(static_cast<std::uint8_t>(keyboard_name.size()));
        body.bytes(keyboard_name);
        body.pad_to_four();
        WireWriter reply(context.order);
        reply_header(reply, context.sequence,
                     static_cast<std::uint32_t>(body.size() / 4));
        reply.u8(2);
        reply.pad(23);
        reply.bytes(body.data());
        return queue(reply.data());
    }
    case 3: { // OpenDevice
        if (context.request.size() != 8)
            return error(bad_length);
        const std::uint8_t device = context.request[4];
        auto valid = require_device(device);
        if (!valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 4: // CloseDevice
        if (context.request.size() != 8)
            return error(bad_length);
        return require_device(context.request[4]);
    case 5: // SetDeviceMode
        if (context.request.size() != 8)
            return error(bad_length);
        if (context.request[5] > 1)
            return error(bad_value, context.request[5]);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        return simple_status_reply();
    case 6: // SelectExtensionEvent
    case 8: { // ChangeDeviceDontPropagateList
        if (context.request.size() < 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window = reader.u32();
        const auto count = reader.u16();
        if (!window || !count)
            return malformed_xi1("truncated XI1 event-class request");
        if (server_.window(*window) == nullptr)
            return error(bad_window, *window);
        const auto bytes = checked_multiply(
            static_cast<std::size_t>(*count), std::size_t{4});
        if (!bytes || context.request.size() != 12 + *bytes)
            return error(bad_length);
        return Result<void>::success();
    }
    case 7: // GetSelectedExtensionEvents
    case 9: { // GetDeviceDontPropagateList
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_xi1("truncated XI1 event-class query");
        if (server_.window(*window) == nullptr)
            return error(bad_window, *window);
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u16(0);
        if (context.data == 7)
            reply.u16(0);
        reply.pad(context.data == 7 ? 20 : 22);
        return queue(reply.data());
    }
    case 10: { // GetDeviceMotionEvents
        if (context.request.size() != 16)
            return error(bad_length);
        const std::uint8_t device = context.request[12];
        if (auto valid = require_device(device); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u32(0);
        reply.u8(device == 4 ? 2 : 0);
        reply.u8(device == 4 ? 1 : 0);
        reply.pad(18);
        return queue(reply.data());
    }
    case 11: // ChangeKeyboardDevice
    case 12: // ChangePointerDevice
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(
                context.request[context.data == 11 ? 4 : 6]); !valid)
            return valid;
        return simple_status_reply();
    case 13: // GrabDevice
        if (context.request.size() < 20 || (context.request.size() & 3U) != 0)
            return error(bad_length);
        return simple_status_reply();
    case 14: // UngrabDevice
        if (context.request.size() != 12)
            return error(bad_length);
        return require_device(context.request[8]);
    case 15: // GrabDeviceKey
    case 16: // UngrabDeviceKey
    case 17: // GrabDeviceButton
    case 18: // UngrabDeviceButton
    case 19: // AllowDeviceEvents
        if (context.request.size() < 12 || (context.request.size() & 3U) != 0)
            return error(bad_length);
        return Result<void>::success();
    case 20: { // GetDeviceFocus
        if (context.request.size() != 8)
            return error(bad_length);
        const std::uint8_t device = context.request[4];
        if (auto valid = require_device(device); !valid)
            return valid;
        const auto &focus = server_.input().focus;
        const std::uint32_t focus_id = focus.kind == FocusKind::window
            ? focus.window
            : focus.kind == FocusKind::pointer_root ? pointer_root_id : 0;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u32(focus_id);
        reply.u32(server_.current_time());
        reply.u8(focus.revert_to);
        reply.pad(15);
        return queue(reply.data());
    }
    case 21: { // SetDeviceFocus
        if (context.request.size() != 16)
            return error(bad_length);
        const std::uint8_t device = context.request[13];
        return require_device(device);
    }
    case 22: { // GetFeedbackControl
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u16(0);
        reply.pad(22);
        return queue(reply.data());
    }
    case 23: // ChangeFeedbackControl
    case 25: // ChangeDeviceKeyMapping
    case 31: // SendExtensionEvent
    case 37: // ChangeDeviceProperty
        if (context.request.size() < 8 || (context.request.size() & 3U) != 0)
            return error(bad_length);
        return Result<void>::success();
    case 24: { // GetDeviceKeyMapping
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 26: // GetDeviceModifierMapping
    case 28: { // GetDeviceButtonMapping
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 27: // SetDeviceModifierMapping
    case 29: // SetDeviceButtonMapping
    case 33: // SetDeviceValuators
    case 35: // ChangeDeviceControl
        if (context.request.size() < 8 || (context.request.size() & 3U) != 0)
            return error(bad_length);
        return simple_status_reply();
    case 30: { // QueryDeviceState
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 32: // DeviceBell
        if (context.request.size() != 8)
            return error(bad_length);
        if (context.request[7] > 100 && context.request[7] < 156)
            return error(bad_value, context.request[7]);
        return require_device(context.request[4]);
    case 34: { // GetDeviceControl
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[6]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u8(0);
        reply.pad(23);
        return queue(reply.data());
    }
    case 36: { // ListDeviceProperties
        if (context.request.size() != 8)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u16(0);
        reply.pad(22);
        return queue(reply.data());
    }
    case 38: // DeleteDeviceProperty
        if (context.request.size() != 12)
            return error(bad_length);
        if (auto valid = require_device(context.request[4]); !valid)
            return valid;
        return Result<void>::success();
    case 39: { // GetDeviceProperty
        if (context.request.size() != 24)
            return error(bad_length);
        const std::uint8_t device = context.request[4];
        if (auto valid = require_device(device); !valid)
            return valid;
        WireReader reader(context.request.data() + 8, 8, context.order);
        const auto property = reader.u32();
        const auto type = reader.u32();
        if (!property || !type)
            return malformed_xi1("truncated XI1 property query");
        if (!server_.atoms().name(*property) ||
            (*type != 0 && !server_.atoms().name(*type)))
            return error(bad_atom, !server_.atoms().name(*property)
                                      ? *property : *type);
        WireWriter reply(context.order);
        reply_header(reply, context.sequence);
        reply.u32(0);
        reply.u32(0);
        reply.u32(0);
        reply.u8(0);
        reply.pad(11);
        return queue(reply.data());
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
