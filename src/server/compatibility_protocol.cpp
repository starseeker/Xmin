#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"

#include <bitset>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_match = 8,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_compatibility(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

} // namespace

Result<void>
Connection::handle_xinerama(const RequestContext &context)
{
    WireWriter reply(context.order);
    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(1);
        reply.u16(1);
        reply.pad(20);
        return queue(reply.data());
    }
    case 1: // GetState
    case 2: { // GetScreenCount
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto window = reader.u32();
        if (!window)
            return malformed_compatibility("truncated Xinerama window");
        if (server_.window(*window) == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window, context.data);
        reply.u8(1);
        reply.u8(1);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(*window);
        reply.pad(20);
        return queue(reply.data());
    }
    case 3: { // GetScreenSize
        if (context.request.size() != 12)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto window = reader.u32();
        const auto screen = reader.u32();
        if (!window || !screen)
            return malformed_compatibility("truncated Xinerama screen size");
        if (server_.window(*window) == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window, context.data);
        if (*screen != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *screen, context.data);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.width());
        reply.u32(server_.height());
        reply.u32(*window);
        reply.u32(0);
        reply.pad(8);
        return queue(reply.data());
    }
    case 4: // IsActive
        if (context.request.size() != 4)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(1);
        reply.pad(20);
        return queue(reply.data());
    case 5: // QueryScreens
        if (context.request.size() != 4)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(2);
        reply.u32(1);
        reply.pad(20);
        reply.i16(0);
        reply.i16(0);
        reply.u16(server_.width());
        reply.u16(server_.height());
        return queue(reply.data());
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_screensaver(const RequestContext &context)
{
    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(1);
        reply.u16(1);
        reply.pad(20);
        return queue(reply.data());
    }
    case 1: { // QueryInfo
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto drawable = reader.u32();
        if (!drawable)
            return malformed_compatibility("truncated ScreenSaver QueryInfo");
        if (server_.drawable_surface(*drawable) == nullptr)
            return send_error(context.order, bad_drawable, context.opcode,
                              context.sequence, *drawable, context.data);
        const auto &state = server_.screen_saver();
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(state.active ? 1 : 0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(root_window_id);
        reply.u32(state.active || state.timeout <= 0
                      ? 0
                      : static_cast<std::uint32_t>(state.timeout) * 1000U);
        reply.u32(0);
        reply.u32(screensaver_drawable_ == *drawable
                      ? screensaver_event_mask_ : 0);
        reply.u8(1); // internal saver
        reply.pad(7);
        return queue(reply.data());
    }
    case 2: { // SelectInput
        if (context.request.size() != 12)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto drawable = reader.u32();
        const auto mask = reader.u32();
        if (!drawable || !mask)
            return malformed_compatibility("truncated ScreenSaver SelectInput");
        if (server_.drawable_surface(*drawable) == nullptr)
            return send_error(context.order, bad_drawable, context.opcode,
                              context.sequence, *drawable, context.data);
        if ((*mask & ~3U) != 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *mask, context.data);
        screensaver_drawable_ = *mask == 0 ? 0 : *drawable;
        screensaver_event_mask_ = *mask;
        return Result<void>::success();
    }
    case 3: { // SetAttributes
        if (context.request.size() < 28)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto drawable = reader.u32();
        if (!drawable || !reader.skip(16))
            return malformed_compatibility("truncated ScreenSaver attributes");
        const auto mask = reader.u32();
        if (!mask)
            return malformed_compatibility("truncated ScreenSaver mask");
        const auto bytes = checked_multiply(
            std::bitset<32>(*mask).count(), std::size_t{4});
        if (!bytes || context.request.size() != 28 + *bytes)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        if (server_.drawable_surface(*drawable) == nullptr)
            return send_error(context.order, bad_drawable, context.opcode,
                              context.sequence, *drawable, context.data);
        return Result<void>::success();
    }
    case 4: // UnsetAttributes
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        return Result<void>::success();
    case 5: { // Suspend
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto suspend = reader.u32();
        if (!suspend)
            return malformed_compatibility("truncated ScreenSaver Suspend");
        if (*suspend > 1)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *suspend, context.data);
        auto state = server_.screen_saver();
        if (*suspend != 0)
            state.active = false;
        server_.set_screen_saver(state);
        return Result<void>::success();
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

Result<void>
Connection::handle_dbe(const RequestContext &context)
{
    constexpr std::uint8_t bad_buffer = dbe_extension.first_error;
    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u8(1);
        reply.u8(0);
        reply.pad(22);
        return queue(reply.data());
    }
    case 1: { // AllocateBackBuffer
        if (context.request.size() != 16)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto window_id = reader.u32();
        const auto buffer = reader.u32();
        const auto action = reader.u8();
        if (!window_id || !buffer || !action || !reader.skip(3))
            return malformed_compatibility("truncated DBE allocation");
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return send_error(context.order, bad_window, context.opcode,
                              context.sequence, *window_id, context.data);
        if (*action > 3)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, *action, context.data);
        if (!server_.valid_client_resource(*buffer, config_.resource_base))
            return send_error(context.order, bad_id_choice, context.opcode,
                              context.sequence, *buffer, context.data);
        auto surface = Surface::create(window->width, window->height,
                                       window->depth);
        if (!surface)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        auto managed = server_.adopt_surface(std::move(*surface));
        if (!managed || !server_.add_dbe_buffer(
                {*buffer, *window_id, *action, std::move(managed)},
                config_.resource_base)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        return Result<void>::success();
    }
    case 2: { // DeallocateBackBuffer
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto buffer = reader.u32();
        if (!buffer)
            return malformed_compatibility("truncated DBE deallocation");
        if (!server_.erase_dbe_buffer(*buffer))
            return send_error(context.order, bad_buffer, context.opcode,
                              context.sequence, *buffer, context.data);
        return Result<void>::success();
    }
    case 3: { // SwapBuffers
        if (context.request.size() < 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto count = reader.u32();
        if (!count)
            return malformed_compatibility("truncated DBE swap count");
        const auto bytes = checked_multiply(
            static_cast<std::size_t>(*count), std::size_t{8});
        if (!bytes || context.request.size() != 8 + *bytes)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        std::vector<std::pair<std::uint32_t, std::uint8_t>> swaps;
        try {
            swaps.reserve(*count);
            for (std::size_t index = 0; index < *count; ++index) {
                const auto window = reader.u32();
                const auto action = reader.u8();
                if (!window || !action || !reader.skip(3))
                    return malformed_compatibility("truncated DBE swap list");
                if (server_.window(*window) == nullptr)
                    return send_error(context.order, bad_window,
                                      context.opcode, context.sequence,
                                      *window, context.data);
                if (*action > 3)
                    return send_error(context.order, bad_value,
                                      context.opcode, context.sequence,
                                      *action, context.data);
                swaps.emplace_back(*window, *action);
            }
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence, 0, context.data);
        }
        for (const auto &swap : swaps) {
            if (!server_.swap_dbe_buffer(swap.first, swap.second))
                return send_error(context.order, bad_match, context.opcode,
                                  context.sequence, swap.first, context.data);
            auto finished = finish_draw(context, swap.first);
            if (!finished)
                return finished;
        }
        return Result<void>::success();
    }
    case 4: // BeginIdiom
    case 5: // EndIdiom
        if (context.request.size() != 4)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        return Result<void>::success();
    case 6: { // GetVisualInfo
        if (context.request.size() < 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto count = reader.u32();
        if (!count)
            return malformed_compatibility("truncated DBE visual count");
        const auto bytes = checked_multiply(
            static_cast<std::size_t>(*count), std::size_t{4});
        if (!bytes || context.request.size() != 8 + *bytes)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        for (std::size_t index = 0; index < *count; ++index) {
            const auto drawable = reader.u32();
            if (!drawable)
                return malformed_compatibility("truncated DBE drawable list");
            if (server_.drawable_surface(*drawable) == nullptr)
                return send_error(context.order, bad_drawable,
                                  context.opcode, context.sequence,
                                  *drawable, context.data);
        }
        const std::uint32_t groups = *count == 0 ? 1 : *count;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(groups * 3);
        reply.u32(groups);
        reply.pad(20);
        for (std::uint32_t index = 0; index < groups; ++index) {
            reply.u32(1);
            reply.u32(root_visual_id);
            reply.u8(24);
            reply.u8(0);
            reply.pad(2);
        }
        return queue(reply.data());
    }
    case 7: { // GetBackBufferAttributes
        if (context.request.size() != 8)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence, 0, context.data);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto buffer = reader.u32();
        if (!buffer)
            return malformed_compatibility("truncated DBE attributes");
        const auto *record = server_.dbe_buffer(*buffer);
        if (record == nullptr)
            return send_error(context.order, bad_buffer, context.opcode,
                              context.sequence, *buffer, context.data);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(record->window);
        reply.pad(20);
        return queue(reply.data());
    }
    default:
        return send_error(context.order, bad_request, context.opcode,
                          context.sequence, 0, context.data);
    }
}

} // namespace xmin::server
