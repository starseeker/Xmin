#include "xmin/config.h"
#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_match = 8,
    bad_drawable = 9,
    bad_access = 10,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_shm(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

std::optional<std::size_t>
zpixmap_size(std::uint16_t width, std::uint16_t height,
             std::uint8_t depth) noexcept
{
    const std::size_t bits_per_pixel = depth == 1
        ? 1
        : (depth <= 8 ? 8 : 32);
    const auto row_bits = checked_multiply(
        static_cast<std::size_t>(width), bits_per_pixel);
    const auto rounded_bits = row_bits
        ? checked_add(*row_bits, std::size_t{7})
        : std::optional<std::size_t>{};
    const auto stride = rounded_bits
        ? padded_to_four(*rounded_bits / 8)
        : std::optional<std::size_t>{};
    return stride
        ? checked_multiply(*stride, static_cast<std::size_t>(height))
        : std::optional<std::size_t>{};
}

std::int16_t
signed_word_shm(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

} // namespace

Result<void>
Connection::handle_shm(const RequestContext &context)
{
    constexpr std::uint8_t bad_segment = shm_extension.first_error;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };

    switch (context.data) {
    case 0: { // QueryVersion
        if (context.request.size() != 4)
            return error(bad_length);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(1);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u16(1);
        reply.u16(XMIN_HAVE_SCM_RIGHTS ? 2 : 1);
        reply.u16(static_cast<std::uint16_t>(::getuid()));
        reply.u16(static_cast<std::uint16_t>(::getgid()));
        reply.u8(2); // ZPixmap
        reply.pad(15);
        return queue(reply.data());
    }
    case 1: { // Attach
        if (context.request.size() != 16)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto system_id = reader.u32();
        const auto read_only = reader.u8();
        if (!id || !system_id || !read_only || !reader.skip(3))
            return malformed_shm("truncated MIT-SHM Attach request");
        if (*read_only > 1)
            return error(bad_value, *read_only);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        if (*system_id > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max())) {
            return error(bad_access, *system_id);
        }
        auto memory = SharedMemory::attach_sysv(
            static_cast<int>(*system_id), *read_only != 0);
        if (!memory)
            return error(bad_access, *system_id);
        if (!server_.add_shared_memory(
                *id, std::move(memory.value()), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
    }
    case 2: { // Detach
        if (context.request.size() != 8)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 4, context.order);
        const auto id = reader.u32();
        if (!id)
            return malformed_shm("truncated MIT-SHM Detach request");
        if (!server_.erase_shared_memory(*id))
            return error(bad_segment, *id);
        return Result<void>::success();
    }
    case 3: { // PutImage
        if (context.request.size() != 40)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 36, context.order);
        const auto drawable = reader.u32();
        const auto graphics = reader.u32();
        const auto total_width = reader.u16();
        const auto total_height = reader.u16();
        const auto source_x = reader.u16();
        const auto source_y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto destination_x = reader.u16();
        const auto destination_y = reader.u16();
        const auto depth = reader.u8();
        const auto format = reader.u8();
        const auto send_event = reader.u8();
        const auto padding = reader.u8();
        const auto segment_id = reader.u32();
        const auto offset = reader.u32();
        if (!drawable || !graphics || !total_width || !total_height ||
            !source_x || !source_y || !width || !height ||
            !destination_x || !destination_y || !depth || !format ||
            !send_event || !padding || !segment_id || !offset) {
            return malformed_shm("truncated MIT-SHM PutImage request");
        }
        if (*format != 2 || *send_event > 1)
            return error(bad_value, *format != 2 ? *format : *send_event);
        const auto *memory = server_.shared_memory(*segment_id);
        if (memory == nullptr)
            return error(bad_segment, *segment_id);
        const auto size = zpixmap_size(*total_width, *total_height, *depth);
        const auto end = size
            ? checked_add(static_cast<std::size_t>(*offset), *size)
            : std::optional<std::size_t>{};
        if (!end || *end > memory->size())
            return error(bad_access, *offset);
        auto drawn = draw_zpixmap(
            context, *drawable, *graphics, *total_width, *total_height,
            *source_x, *source_y, *width, *height,
            signed_word_shm(*destination_x), signed_word_shm(*destination_y),
            *depth,
            reinterpret_cast<const std::uint8_t *>(memory->data()) + *offset,
            *size, true);
        if (!drawn || *send_event == 0)
            return drawn;
        WireWriter event(context.order);
        event.u8(shm_extension.first_event);
        event.u8(0);
        event.u16(context.sequence);
        event.u32(*drawable);
        event.u16(3);
        event.u8(shm_extension.major_opcode);
        event.u8(0);
        event.u32(*segment_id);
        event.u32(*offset);
        event.pad(12);
        return queue(event.data());
    }
    case 4: { // GetImage
        if (context.request.size() != 32)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 28, context.order);
        const auto drawable = reader.u32();
        const auto x = reader.u16();
        const auto y = reader.u16();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto plane_mask = reader.u32();
        const auto format = reader.u8();
        const bool padding_ok = reader.skip(3);
        const auto segment_id = reader.u32();
        const auto offset = reader.u32();
        if (!drawable || !x || !y || !width || !height || !plane_mask ||
            !format || !padding_ok || !segment_id || !offset) {
            return malformed_shm("truncated MIT-SHM GetImage request");
        }
        const auto *surface = server_.drawable_surface(*drawable);
        if (surface == nullptr)
            return error(bad_drawable, *drawable);
        if (*format != 2)
            return error(bad_value, *format);
        const std::int32_t image_x = signed_word_shm(*x);
        const std::int32_t image_y = signed_word_shm(*y);
        if (*width == 0 || *height == 0 || image_x < 0 || image_y < 0 ||
            image_x + *width > surface->width() ||
            image_y + *height > surface->height()) {
            return error(bad_match);
        }
        surface = server_.readable_surface(
            *drawable,
            Rectangle{image_x, image_y, *width, *height});
        if (surface == nullptr)
            return error(bad_drawable, *drawable);
        auto *memory = server_.shared_memory(*segment_id);
        if (memory == nullptr)
            return error(bad_segment, *segment_id);
        auto *destination = memory->writable_data();
        if (destination == nullptr)
            return error(bad_access, *segment_id);
        const auto size = zpixmap_size(*width, *height, surface->depth());
        const auto end = size
            ? checked_add(static_cast<std::size_t>(*offset), *size)
            : std::optional<std::size_t>{};
        if (!end || *end > memory->size())
            return error(bad_access, *offset);
        destination += *offset;
        const auto stride = *size / *height;
        const bool little = host_byte_order() == ByteOrder::little;
        if (little && surface->depth() >= 24) {
            const std::uint32_t depth_mask = surface->depth() == 32
                ? 0xffffffffU
                : 0x00ffffffU;
            const std::uint32_t mask = *plane_mask & depth_mask;
            for (std::uint32_t row = 0; row < *height; ++row) {
                const auto *source = surface->data() +
                    static_cast<std::size_t>(image_y + row) *
                        surface->width() + image_x;
                auto *row_destination = destination +
                    static_cast<std::size_t>(row) * stride;
                for (std::uint32_t column = 0; column < *width; ++column) {
                    const std::uint32_t pixel = source[column] & mask;
                    std::memcpy(row_destination + column * sizeof(pixel),
                                &pixel, sizeof(pixel));
                }
            }
        }
        else {
            std::fill(destination, destination + *size, std::byte{0});
            for (std::uint32_t row = 0; row < *height; ++row) {
                for (std::uint32_t column = 0; column < *width; ++column) {
                    const std::uint32_t pixel = surface->pixel(
                        static_cast<std::uint16_t>(image_x + column),
                        static_cast<std::uint16_t>(image_y + row)) &
                        *plane_mask;
                    const std::size_t row_offset =
                        static_cast<std::size_t>(row) * stride;
                    if (surface->depth() == 1) {
                        const unsigned bit = little ? column & 7U
                                                    : 7U - (column & 7U);
                        destination[row_offset + column / 8] |=
                            static_cast<std::byte>((pixel & 1U) << bit);
                    }
                    else if (surface->depth() <= 8) {
                        destination[row_offset + column] =
                            static_cast<std::byte>(pixel);
                    }
                    else {
                        const std::size_t pixel_offset =
                            row_offset + column * 4;
                        for (unsigned index = 0; index < 4; ++index) {
                            const unsigned shift = little ? index * 8
                                                          : (3 - index) * 8;
                            destination[pixel_offset + index] =
                                static_cast<std::byte>(pixel >> shift);
                        }
                    }
                }
            }
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(surface->depth());
        reply.u16(context.sequence);
        reply.u32(0);
        reply.u32(server_.window(*drawable) == nullptr ? 0 : root_visual_id);
        reply.u32(static_cast<std::uint32_t>(*size));
        reply.pad(16);
        return queue(reply.data());
    }
    case 5: { // CreatePixmap
        if (context.request.size() != 28)
            return error(bad_length);
        WireReader reader(context.request.data() + 4, 24, context.order);
        const auto pixmap = reader.u32();
        const auto drawable = reader.u32();
        const auto width = reader.u16();
        const auto height = reader.u16();
        const auto depth = reader.u8();
        const bool padding_ok = reader.skip(3);
        const auto segment = reader.u32();
        const auto offset = reader.u32();
        if (!pixmap || !drawable || !width || !height || !depth ||
            !padding_ok || !segment || !offset) {
            return malformed_shm("truncated MIT-SHM CreatePixmap request");
        }
        const auto *reference = server_.readable_surface(*drawable);
        if (reference == nullptr)
            return error(bad_drawable, *drawable);
        if (*width == 0 || *height == 0 ||
            (*depth != 24 && *depth != 32) ||
            *depth != reference->depth()) {
            return error(bad_match);
        }
        if (!server_.valid_client_resource(*pixmap, config_.resource_base))
            return error(bad_id_choice, *pixmap);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        auto memory = server_.shared_memory_storage(*segment);
        if (!memory)
            return error(bad_segment, *segment);
        auto surface = Surface::create_shared(
            *width, *height, *depth, std::move(memory), *offset);
        if (!surface)
            return error(bad_access, *offset);
        // Shared pixmaps do not allocate their pixels from the server heap,
        // but retaining their mappings still consumes address space and file
        // descriptors.  Account their logical surface size against the same
        // aggregate budget as ordinary pixmaps so Detach/CreatePixmap cycles
        // cannot retain an unbounded number of mappings.
        auto stored = server_.adopt_surface(std::move(*surface));
        if (!stored)
            return error(bad_alloc);
        if (!server_.add_pixmap({*pixmap, std::move(stored)},
                                config_.resource_base)) {
            return error(server_.resource_exists(*pixmap)
                    ? bad_id_choice
                    : bad_alloc, *pixmap);
        }
        return Result<void>::success();
    }
    case 6: { // AttachFd
        UniqueFd descriptor = take_received_fd();
        if (context.request.size() != 12)
            return error(bad_length);
#if !XMIN_HAVE_SCM_RIGHTS
        return error(bad_request);
#else
        WireReader reader(context.request.data() + 4, 8, context.order);
        const auto id = reader.u32();
        const auto read_only = reader.u8();
        if (!id || !read_only || !reader.skip(3))
            return malformed_shm("truncated MIT-SHM AttachFd request");
        if (!descriptor)
            return error(bad_length);
        if (*read_only > 1)
            return error(bad_value, *read_only);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        auto memory = SharedMemory::attach_fd(
            std::move(descriptor), *read_only != 0);
        if (!memory) {
            return error(memory.error().code == ErrorCode::invalid_argument
                    ? bad_value
                    : bad_access);
        }
        if (!server_.add_shared_memory(
                *id, std::move(memory.value()), config_.resource_base)) {
            return error(bad_alloc);
        }
        return Result<void>::success();
#endif
    }
    case 7: { // CreateSegment
        if (context.request.size() != 16)
            return error(bad_length);
#if !XMIN_HAVE_SCM_RIGHTS
        return error(bad_request);
#else
        WireReader reader(context.request.data() + 4, 12, context.order);
        const auto id = reader.u32();
        const auto size = reader.u32();
        const auto read_only = reader.u8();
        if (!id || !size || !read_only || !reader.skip(3))
            return malformed_shm("truncated MIT-SHM CreateSegment request");
        if (*size == 0 || *read_only > 1)
            return error(bad_value, *size == 0 ? *size : *read_only);
        if (!server_.valid_client_resource(*id, config_.resource_base))
            return error(bad_id_choice, *id);
        if (server_.resource_limit_reached(config_.resource_base))
            return error(bad_alloc);
        UniqueFd client_descriptor;
        auto memory = SharedMemory::create(
            *size, *read_only != 0, client_descriptor);
        if (!memory) {
            return error(memory.error().code == ErrorCode::invalid_argument
                    ? bad_value
                    : bad_alloc, *size);
        }
        if (!server_.add_shared_memory(
                *id, std::move(memory.value()), config_.resource_base)) {
            return error(bad_alloc);
        }
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(1);
        reply.u16(context.sequence);
        reply.u32(0);
        reply.pad(24);
        auto queued = queue_with_fd(reply.data(), std::move(client_descriptor));
        if (!queued)
            static_cast<void>(server_.erase_shared_memory(*id));
        return queued;
#endif
    }
    default:
        return error(bad_request);
    }
}

} // namespace xmin::server
