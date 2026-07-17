#include "xmin/next/connection.hpp"

#include "xmin/next/checked.hpp"
#include "xmin/next/core_raster.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace xmin::next {
namespace {

enum : std::uint8_t {
    bad_value = 2,
    bad_match = 8,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_graphics_context = 13,
    bad_length = 16,
};

Result<void>
malformed_raster(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

std::int16_t
signed_word_raster(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

RasterArc
read_arc(WireReader &reader, bool &valid)
{
    const auto x = reader.u16();
    const auto y = reader.u16();
    const auto width = reader.u16();
    const auto height = reader.u16();
    const auto start = reader.u16();
    const auto angle = reader.u16();
    valid = x && y && width && height && start && angle;
    return valid
        ? RasterArc{signed_word_raster(*x), signed_word_raster(*y),
                    *width, *height, signed_word_raster(*start),
                    signed_word_raster(*angle)}
        : RasterArc{};
}

} // namespace

Result<void>
Connection::handle_set_dashes(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto graphics_id = reader.u32();
    const auto offset = reader.u16();
    const auto count = reader.u16();
    if (!graphics_id || !offset || !count)
        return malformed_raster("truncated SetDashes request");
    const auto padded = padded_to_four(*count);
    if (!padded || *count == 0 || context.request.size() != 12 + *padded)
        return send_error(context.order,
                          *count == 0 ? bad_value : bad_length,
                          context.opcode, context.sequence, *count);
    auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    std::optional<GraphicsContextRecord> updated;
    try {
        updated.emplace(*graphics);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    updated->dash_offset = *offset;
    updated->dash_count = static_cast<std::uint16_t>(
        (*count & 1U) == 0 ? *count : *count * 2U);
    for (std::size_t index = 0; index < *count; ++index) {
        const auto value = reader.u8();
        if (!value)
            return malformed_raster("truncated SetDashes list");
        if (*value == 0)
            return send_error(context.order, bad_value, context.opcode,
                              context.sequence, 0);
        updated->dashes[index] = *value;
        if ((*count & 1U) != 0)
            updated->dashes[index + *count] = *value;
    }
    *graphics = std::move(*updated);
    return Result<void>::success();
}

Result<void>
Connection::handle_poly_arcs(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        (context.request.size() - 12) % 12 != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable || !graphics_id)
        return malformed_raster("truncated PolyArc request");
    auto *surface = server_.drawable_surface(*drawable);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    while (reader.remaining() != 0) {
        bool valid = false;
        const auto arc = read_arc(reader, valid);
        if (!valid)
            return malformed_raster("truncated PolyArc list");
        draw_gc_arc(*surface, *graphics, arc);
    }
    return finish_draw(context, *drawable);
}

Result<void>
Connection::handle_fill_polygon(const RequestContext &context)
{
    if (context.request.size() < 16 ||
        ((context.request.size() - 16) & 3U) != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable = reader.u32();
    const auto graphics_id = reader.u32();
    const auto shape = reader.u8();
    const auto coordinate_mode = reader.u8();
    if (!drawable || !graphics_id || !shape || !coordinate_mode ||
        !reader.skip(2)) {
        return malformed_raster("truncated FillPoly request");
    }
    if (*shape > 2)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *shape);
    if (*coordinate_mode > 1)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *coordinate_mode);
    auto *surface = server_.drawable_surface(*drawable);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    std::vector<RasterPoint> points;
    try {
        points.reserve(reader.remaining() / 4);
        std::int64_t x = 0;
        std::int64_t y = 0;
        while (reader.remaining() != 0) {
            const auto encoded_x = reader.u16();
            const auto encoded_y = reader.u16();
            if (!encoded_x || !encoded_y)
                return malformed_raster("truncated FillPoly point list");
            const std::int32_t decoded_x = signed_word_raster(*encoded_x);
            const std::int32_t decoded_y = signed_word_raster(*encoded_y);
            if (*coordinate_mode == 1 && !points.empty()) {
                x += decoded_x;
                y += decoded_y;
            }
            else {
                x = decoded_x;
                y = decoded_y;
            }
            points.push_back({
                static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    x, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max())),
                static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    y, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()))});
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    fill_gc_polygon(*surface, *graphics, points);
    return finish_draw(context, *drawable);
}

Result<void>
Connection::handle_fill_arcs(const RequestContext &context)
{
    if (context.request.size() < 12 ||
        (context.request.size() - 12) % 12 != 0) {
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    }
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable = reader.u32();
    const auto graphics_id = reader.u32();
    if (!drawable || !graphics_id)
        return malformed_raster("truncated PolyFillArc request");
    auto *surface = server_.drawable_surface(*drawable);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, *drawable);
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    while (reader.remaining() != 0) {
        bool valid = false;
        const auto arc = read_arc(reader, valid);
        if (!valid)
            return malformed_raster("truncated PolyFillArc list");
        fill_gc_arc(*surface, *graphics, arc);
    }
    return finish_draw(context, *drawable);
}

} // namespace xmin::next
