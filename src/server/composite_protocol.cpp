#include "xmin/server/connection.hpp"

#include "xmin/server/extension_registry.hpp"

#include <algorithm>
#include <string_view>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_match = 8,
    bad_access = 10,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_composite(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

} // namespace

Result<void>
Connection::handle_composite(const RequestContext &context)
{
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto redirect_result = [&](CompositeUpdate result,
                                     bool unredirect) {
        switch (result) {
        case CompositeUpdate::updated:
            return Result<void>::success();
        case CompositeUpdate::invalid:
            return error(unredirect ? bad_value : bad_match);
        case CompositeUpdate::access_denied:
            return error(bad_access);
        case CompositeUpdate::resource_exhausted:
            return error(bad_alloc);
        }
        return error(bad_request);
    };

    if (context.data != 0 && !composite_version_negotiated_)
        return error(bad_request);

    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return malformed_composite(
                "truncated Composite QueryVersion request");
        const std::uint32_t negotiated_minor = *major == 0
            ? std::min<std::uint32_t>(
                  *minor, composite_extension.minor_version)
            : composite_extension.minor_version;
        composite_version_negotiated_ = true;
        composite_minor_version_ = static_cast<std::uint16_t>(
            negotiated_minor);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(composite_extension.major_version);
        reply.u32(negotiated_minor);
        reply.pad(16);
        return queue(reply.data());
    }
    case 1: // RedirectWindow
    case 2: // RedirectSubwindows
    case 3: // UnredirectWindow
    case 4: { // UnredirectSubwindows
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto update = reader.u8();
        if (!window_id || !update || !reader.skip(3))
            return malformed_composite(
                "truncated Composite redirect request");
        if (*update > 1)
            return error(bad_value, *update);
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        const bool subwindows = context.data == 2 || context.data == 4;
        const bool unredirect = context.data == 3 || context.data == 4;
        const auto result = unredirect
            ? server_.unredirect_window(
                  config_.resource_base, *window_id, subwindows, *update)
            : server_.redirect_window(
                  config_.resource_base, *window_id, subwindows, *update);
        return redirect_result(result, unredirect);
    }
    case 5: { // CreateRegionFromBorderClip
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto region_id = reader.u32();
        const auto window_id = reader.u32();
        if (!region_id || !window_id)
            return malformed_composite(
                "truncated Composite CreateRegionFromBorderClip request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        if (!server_.valid_client_resource(
                *region_id, config_.resource_base)) {
            return error(bad_id_choice, *region_id);
        }
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        Region border_clip;
        bool created = false;
        if (server_.composite_window_manually_redirected(*window_id)) {
            created = Region::canonicalize({}, border_clip);
        }
        else if (window->shapes[0]) {
            created = Region::combine(
                RegionOperation::set, *window->shapes[0],
                *window->shapes[0], border_clip);
        }
        else {
            created = Region::canonicalize(
                {window->default_shape(0)}, border_clip);
        }
        if (!created || !server_.add_xfixes_region(
                *region_id, std::move(border_clip),
                config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 6: { // NameWindowPixmap
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window_id = reader.u32();
        const auto pixmap_id = reader.u32();
        if (!window_id || !pixmap_id)
            return malformed_composite(
                "truncated Composite NameWindowPixmap request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (!server_.valid_client_resource(
                *pixmap_id, config_.resource_base)) {
            return error(bad_id_choice, *pixmap_id);
        }
        const auto result = server_.name_window_pixmap(
            *window_id, *pixmap_id, config_.resource_base);
        if (result == CompositeUpdate::resource_exhausted)
            return error(bad_alloc);
        if (result != CompositeUpdate::updated)
            return error(bad_match);
        return Result<void>::success();
    }
    case 7: // GetOverlayWindow
    case 8: { // ReleaseOverlayWindow
        if (composite_minor_version_ < 3)
            return error(bad_request);
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window_id = reader.u32();
        if (!window_id)
            return malformed_composite(
                "truncated Composite overlay request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        return error(bad_match, *window_id);
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
