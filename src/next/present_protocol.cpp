#include "xmin/next/connection.hpp"

#include "xmin/next/extension_registry.hpp"

#include <algorithm>
#include <string_view>

namespace xmin::next {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_window = 3,
    bad_pixmap = 4,
    bad_match = 8,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_present(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

} // namespace

Result<void>
Connection::handle_present(const RequestContext &context)
{
    constexpr std::uint8_t bad_fence = sync_extension.first_error + 2;
    constexpr std::uint8_t bad_region = xfixes_extension.first_error;
    constexpr std::uint8_t bad_crtc = randr_extension.first_error + 1;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto finish = [&](PresentUpdate result) {
        switch (result) {
        case PresentUpdate::updated:
            return drain_pending_events();
        case PresentUpdate::invalid:
            return error(bad_value);
        case PresentUpdate::match:
            return error(bad_match);
        case PresentUpdate::resource_exhausted:
        case PresentUpdate::queue_full:
            return error(bad_alloc);
        }
        return error(bad_request);
    };
    const auto valid_modulo = [&](std::uint64_t divisor,
                                  std::uint64_t remainder) {
        return divisor == 0 ? remainder == 0 : remainder < divisor;
    };
    if (context.data != 0 && !present_version_negotiated_)
        return error(bad_request);
    if (context.data == 5 && present_minor_version_ < 4)
        return error(bad_request);

    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 12)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto major = reader.u32();
        const auto minor = reader.u32();
        if (!major || !minor)
            return malformed_present(
                "truncated Present QueryVersion request");
        const bool client_is_older =
            *major < present_extension.major_version ||
            (*major == present_extension.major_version &&
             *minor < present_extension.minor_version);
        const std::uint32_t negotiated_major = client_is_older
            ? *major
            : present_extension.major_version;
        const std::uint32_t negotiated_minor = client_is_older
            ? *minor
            : present_extension.minor_version;
        present_version_negotiated_ = true;
        present_minor_version_ = negotiated_major == 1
            ? static_cast<std::uint16_t>(negotiated_minor)
            : 0;
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(negotiated_major);
        reply.u32(negotiated_minor);
        reply.pad(16);
        return queue(reply.data());
    }
    case 1: { // Pixmap
        if (context.request.size() < 72 ||
            (context.request.size() - 72) % 8 != 0) {
            return error(bad_length);
        }
        const std::size_t notify_count =
            (context.request.size() - 72) / 8;
        if (notify_count > maximum_present_notifies)
            return error(bad_alloc);
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window_id = reader.u32();
        const auto pixmap_id = reader.u32();
        const auto serial = reader.u32();
        const auto valid_id = reader.u32();
        const auto update_id = reader.u32();
        const auto x_off = reader.u16();
        const auto y_off = reader.u16();
        const auto target_crtc = reader.u32();
        const auto wait_fence = reader.u32();
        const auto idle_fence = reader.u32();
        const auto options = reader.u32();
        if (!window_id || !pixmap_id || !serial || !valid_id ||
            !update_id || !x_off || !y_off || !target_crtc ||
            !wait_fence || !idle_fence || !options || !reader.skip(4)) {
            return malformed_present("truncated Present Pixmap request");
        }
        const auto target_msc = reader.u64();
        const auto divisor = reader.u64();
        const auto remainder = reader.u64();
        if (!target_msc || !divisor || !remainder)
            return malformed_present("truncated Present Pixmap timing");

        auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        const auto *pixmap = server_.pixmap(*pixmap_id);
        if (pixmap == nullptr)
            return error(bad_pixmap, *pixmap_id);
        if (!window->surface || !pixmap->surface ||
            window->depth != pixmap->surface->depth()) {
            return error(bad_match);
        }
        if (*valid_id != 0 && server_.xfixes_region(*valid_id) == nullptr)
            return error(bad_region, *valid_id);
        if (*update_id != 0 && server_.xfixes_region(*update_id) == nullptr)
            return error(bad_region, *update_id);
        if (*target_crtc != 0 && *target_crtc != randr_crtc_id)
            return error(bad_crtc, *target_crtc);
        if (*wait_fence != 0 && server_.sync_fence(*wait_fence) == nullptr)
            return error(bad_fence, *wait_fence);
        if (*idle_fence != 0 && server_.sync_fence(*idle_fence) == nullptr)
            return error(bad_fence, *idle_fence);
        if ((*options & ~std::uint32_t{0x1f}) != 0)
            return error(bad_value, *options);
        if (!valid_modulo(*divisor, *remainder))
            return error(bad_value, static_cast<std::uint32_t>(*remainder));

        PresentOperation operation;
        operation.kind = PresentKind::pixmap;
        operation.owner = config_.resource_base;
        operation.window = *window_id;
        operation.pixmap = *pixmap_id;
        operation.serial = *serial;
        operation.pixmap_surface = pixmap->surface;
        if (*update_id != 0)
            operation.update = *server_.xfixes_region(*update_id);
        operation.x_off = static_cast<std::int16_t>(*x_off);
        operation.y_off = static_cast<std::int16_t>(*y_off);
        operation.wait_fence = *wait_fence;
        operation.idle_fence = *idle_fence;
        operation.options = *options;
        operation.target_msc = *target_msc;
        operation.divisor = *divisor;
        operation.remainder = *remainder;
        try {
            operation.notifies.reserve(notify_count);
            for (std::size_t index = 0; index < notify_count; ++index) {
                const auto notify_window = reader.u32();
                const auto notify_serial = reader.u32();
                if (!notify_window || !notify_serial)
                    return malformed_present(
                        "truncated Present notify list");
                if (server_.window(*notify_window) == nullptr)
                    return error(bad_window, *notify_window);
                operation.notifies.push_back(
                    PresentNotify{*notify_window, *notify_serial});
            }
        }
        catch (const std::bad_alloc &) {
            return error(bad_alloc);
        }
        return finish(server_.submit_present(std::move(operation)));
    }
    case 2: { // NotifyMSC
        if (context.request.size() != 40)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 36, context.order);
        const auto window_id = reader.u32();
        const auto serial = reader.u32();
        if (!window_id || !serial || !reader.skip(4))
            return malformed_present("truncated Present NotifyMSC request");
        const auto target_msc = reader.u64();
        const auto divisor = reader.u64();
        const auto remainder = reader.u64();
        if (!target_msc || !divisor || !remainder)
            return malformed_present("truncated Present NotifyMSC timing");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        if (!valid_modulo(*divisor, *remainder))
            return error(bad_value, static_cast<std::uint32_t>(*remainder));
        PresentOperation operation;
        operation.kind = PresentKind::notify_msc;
        operation.owner = config_.resource_base;
        operation.window = *window_id;
        operation.serial = *serial;
        operation.target_msc = *target_msc;
        operation.divisor = *divisor;
        operation.remainder = *remainder;
        return finish(server_.submit_present(std::move(operation)));
    }
    case 3: { // SelectInput
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto window_id = reader.u32();
        const auto mask = reader.u32();
        if (!id || !window_id || !mask)
            return malformed_present("truncated Present SelectInput request");
        if (server_.window(*window_id) == nullptr)
            return error(bad_window, *window_id);
        // RedirectNotify is tied to the compositor interception machinery
        // deliberately absent from this software-only profile.
        if ((*mask & ~std::uint32_t{0x7}) != 0)
            return error(bad_value, *mask);
        const auto result = server_.select_present_input(
            config_.resource_base, *id, *window_id, *mask);
        if (result == PresentUpdate::invalid)
            return error(bad_id_choice, *id);
        return finish(result);
    }
    case 4: { // QueryCapabilities
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto target = reader.u32();
        if (!target)
            return malformed_present(
                "truncated Present QueryCapabilities request");
        if (server_.window(*target) == nullptr && *target != randr_crtc_id)
            return error(bad_window, *target);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(0);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(0); // no flips, hardware fences, UST, or syncobjs
        reply.pad(20);
        return queue(reply.data());
    }
    case 5: { // PixmapSynced
        if (context.request.size() < 88 ||
            (context.request.size() - 88) % 8 != 0) {
            return error(bad_length);
        }
        WireReader reader(context.request.data() + 4,
                          context.request.size() - 4, context.order);
        const auto window_id = reader.u32();
        const auto pixmap_id = reader.u32();
        if (!window_id || !pixmap_id)
            return malformed_present(
                "truncated Present PixmapSynced request");
        const auto *window = server_.window(*window_id);
        if (window == nullptr)
            return error(bad_window, *window_id);
        const auto *pixmap = server_.pixmap(*pixmap_id);
        if (pixmap == nullptr)
            return error(bad_pixmap, *pixmap_id);
        if (!window->surface || !pixmap->surface ||
            window->depth != pixmap->surface->depth()) {
            return error(bad_match);
        }
        // Present 1.4 framing is recognized, but DRI3 syncobj semantics are
        // deliberately outside this software-only server.
        return error(bad_match);
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::next
