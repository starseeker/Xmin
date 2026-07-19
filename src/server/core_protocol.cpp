#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_value = 2,
    bad_window = 3,
    bad_atom = 5,
    bad_alloc = 11,
    bad_length = 16,
};

Result<void>
malformed_core(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

std::int16_t
signed_value(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

} // namespace

Result<void>
Connection::handle_circulate_window(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto parent = reader.u32();
    if (!parent)
        return malformed_core("truncated CirculateWindow request");
    const auto *parent_window = server_.window(*parent);
    if (parent_window == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *parent);
    const auto redirected = server_.redirect_circulate_request(
        config_.resource_base, *parent_window, context.data == 0);
    if (redirected == RedirectDelivery::queue_full)
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    if (redirected == RedirectDelivery::redirected)
        return drain_pending_events();
    if (server_.circulate_window(*parent, context.data == 0) ==
            EventDelivery::queue_full) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_convert_selection(const RequestContext &context)
{
    if (context.request.size() != 24)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 20, context.order);
    const auto requestor = reader.u32();
    const auto selection = reader.u32();
    const auto target = reader.u32();
    const auto property = reader.u32();
    const auto time = reader.u32();
    if (!requestor || !selection || !target || !property || !time)
        return malformed_core("truncated ConvertSelection request");
    if (server_.window(*requestor) == nullptr)
        return send_error(context.order, bad_window, context.opcode,
                          context.sequence, *requestor);
    for (const auto atom : {*selection, *target, *property}) {
        if (atom != 0 && !server_.atoms().name(atom))
            return send_error(context.order, bad_atom, context.opcode,
                              context.sequence, atom);
    }
    if (server_.convert_selection(
            config_.resource_base, *requestor, *selection, *target,
            *property, *time) == EventDelivery::queue_full) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return drain_pending_events();
}

Result<void>
Connection::handle_set_screen_saver(const RequestContext &context)
{
    if (context.request.size() != 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 8, context.order);
    const auto encoded_timeout = reader.u16();
    const auto encoded_interval = reader.u16();
    const auto prefer_blanking = reader.u8();
    const auto allow_exposures = reader.u8();
    if (!encoded_timeout || !encoded_interval || !prefer_blanking ||
        !allow_exposures || !reader.skip(2)) {
        return malformed_core("truncated SetScreenSaver request");
    }
    const std::int16_t timeout = signed_value(*encoded_timeout);
    const std::int16_t interval = signed_value(*encoded_interval);
    if (timeout < -1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *encoded_timeout);
    if (interval < -1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *encoded_interval);
    if (*prefer_blanking > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *prefer_blanking);
    if (*allow_exposures > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *allow_exposures);
    auto state = server_.screen_saver();
    state.timeout = timeout;
    state.interval = interval;
    state.prefer_blanking = *prefer_blanking;
    state.allow_exposures = *allow_exposures;
    server_.set_screen_saver(state);
    return Result<void>::success();
}

Result<void>
Connection::handle_get_screen_saver(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto &state = server_.screen_saver();
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(static_cast<std::uint16_t>(state.timeout));
    reply.u16(static_cast<std::uint16_t>(state.interval));
    reply.u8(state.prefer_blanking);
    reply.u8(state.allow_exposures);
    reply.pad(18);
    return queue(reply.data());
}

Result<void>
Connection::handle_change_hosts(const RequestContext &context)
{
    if (context.data > 1 || context.request.size() < 8)
        return send_error(context.order,
                          context.data > 1 ? bad_value : bad_length,
                          context.opcode, context.sequence, context.data);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto family = reader.u8();
    const bool padding = reader.skip(1);
    const auto length = reader.u16();
    if (!family || !padding || !length)
        return malformed_core("truncated ChangeHosts request");
    const auto padded = padded_to_four(*length);
    if (!padded || context.request.size() != 8 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    // The clean profile has no network transport, so the network host ACL is
    // deliberately inert while retaining the core protocol surface.
    return Result<void>::success();
}

Result<void>
Connection::handle_list_hosts(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(server_.access_control_enabled() ? 1 : 0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.u16(0);
    reply.pad(22);
    return queue(reply.data());
}

Result<void>
Connection::handle_set_access_control(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    server_.set_access_control(context.data != 0);
    return Result<void>::success();
}

Result<void>
Connection::handle_set_close_down_mode(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    // A one-generation test server always tears down resources at disconnect;
    // accepting the policy request is the protocol-defined inert behavior for
    // this explicitly ephemeral product profile.
    return Result<void>::success();
}

Result<void>
Connection::handle_kill_client(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto resource = reader.u32();
    if (!resource)
        return malformed_core("truncated KillClient request");
    if (!server_.request_client_termination(*resource))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *resource);
    return Result<void>::success();
}

Result<void>
Connection::handle_force_screen_saver(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (context.data > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, context.data);
    auto state = server_.screen_saver();
    state.active = context.data != 0;
    server_.set_screen_saver(state);
    return Result<void>::success();
}

} // namespace xmin::server
