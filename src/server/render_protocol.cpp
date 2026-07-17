#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/extension_registry.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_request = 1,
    bad_value = 2,
    bad_pixmap = 4,
    bad_match = 8,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_name = 15,
    bad_length = 16,
};

std::int16_t
signed_word(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

std::int32_t
signed_dword(std::uint32_t value) noexcept
{
    const std::int64_t widened = value;
    return static_cast<std::int32_t>(
        widened <= std::numeric_limits<std::int32_t>::max()
            ? widened
            : widened - (std::int64_t{1} << 32));
}

Result<void>
malformed(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

struct AttributeError {
    std::uint8_t code;
    std::uint32_t value;
};

std::optional<AttributeError>
pixmap_clip_region(ServerState &server, std::uint32_t pixmap_id,
                   std::optional<Region> &clip)
{
    if (pixmap_id == 0) {
        clip.reset();
        return std::nullopt;
    }
    const auto *pixmap = server.pixmap(pixmap_id);
    if (pixmap == nullptr)
        return AttributeError{bad_pixmap, pixmap_id};
    if (pixmap->surface->depth() != 1)
        return AttributeError{bad_match, pixmap_id};

    std::vector<Rectangle> runs;
    try {
        for (std::uint16_t y = 0; y < pixmap->surface->height(); ++y) {
            std::uint16_t x = 0;
            while (x < pixmap->surface->width()) {
                while (x < pixmap->surface->width() &&
                       pixmap->surface->pixel(x, y) == 0) {
                    ++x;
                }
                const std::uint16_t start = x;
                while (x < pixmap->surface->width() &&
                       pixmap->surface->pixel(x, y) != 0) {
                    ++x;
                }
                if (start == x)
                    continue;
                if (runs.size() == maximum_shape_rectangles)
                    return AttributeError{bad_alloc, 0};
                runs.push_back(Rectangle{
                    start, y, static_cast<std::uint32_t>(x - start), 1});
            }
        }
        Region canonical;
        if (!Region::canonicalize(runs, canonical))
            return AttributeError{bad_alloc, 0};
        clip = std::move(canonical);
    }
    catch (const std::bad_alloc &) {
        return AttributeError{bad_alloc, 0};
    }
    return std::nullopt;
}

std::optional<AttributeError>
apply_picture_attributes(ServerState &server, WireReader &values,
                         std::uint32_t mask,
                         RenderPictureAttributes &attributes,
                         bool has_drawable)
{
    constexpr std::uint32_t valid_mask = 0x1fffU;
    if ((mask & ~valid_mask) != 0)
        return AttributeError{bad_value, mask};
    for (std::uint32_t bit = 0; bit < 13; ++bit) {
        if ((mask & (1U << bit)) == 0)
            continue;
        const auto value = values.u32();
        if (!value)
            return AttributeError{bad_length, 0};
        switch (bit) {
        case 0:
            if (*value > 3)
                return AttributeError{bad_value, *value};
            attributes.repeat = static_cast<RenderRepeat>(*value);
            break;
        case 1:
            if (*value != 0) {
                auto alpha = server.render_picture_handle(*value);
                if (!alpha)
                    return AttributeError{
                        static_cast<std::uint8_t>(
                            render_extension.first_error + 1),
                        *value};
                const auto *drawable =
                    std::get_if<RenderDrawableSource>(&alpha->source);
                if (drawable == nullptr || !drawable->pixmap) {
                    return AttributeError{bad_match, *value};
                }
                attributes.alpha_map_picture = std::move(alpha);
            }
            else
                attributes.alpha_map_picture.reset();
            attributes.alpha_map = *value;
            break;
        case 2:
            attributes.alpha_x_origin = signed_word(
                static_cast<std::uint16_t>(*value));
            break;
        case 3:
            attributes.alpha_y_origin = signed_word(
                static_cast<std::uint16_t>(*value));
            break;
        case 4:
            attributes.clip_x_origin = signed_word(
                static_cast<std::uint16_t>(*value));
            break;
        case 5:
            attributes.clip_y_origin = signed_word(
                static_cast<std::uint16_t>(*value));
            break;
        case 6:
            if (!has_drawable)
                return AttributeError{bad_drawable, 0};
            if (const auto error =
                    pixmap_clip_region(server, *value, attributes.clip)) {
                return error;
            }
            break;
        case 7:
            if (*value > 1)
                return AttributeError{bad_value, *value};
            attributes.graphics_exposures = *value != 0;
            break;
        case 8:
            if (*value > 1)
                return AttributeError{bad_value, *value};
            attributes.subwindow_mode = static_cast<std::uint8_t>(*value);
            break;
        case 9:
            if (*value > 1)
                return AttributeError{bad_value, *value};
            attributes.poly_edge = static_cast<std::uint8_t>(*value);
            break;
        case 10:
            if (*value > 1)
                return AttributeError{bad_value, *value};
            attributes.poly_mode = static_cast<std::uint8_t>(*value);
            break;
        case 11:
            attributes.dither = *value;
            break;
        case 12:
            if (*value > 1)
                return AttributeError{bad_value, *value};
            attributes.component_alpha = *value != 0;
            break;
        }
    }
    if (values.remaining() != 0)
        return AttributeError{bad_length, 0};
    return std::nullopt;
}

std::optional<RenderColor>
read_color(WireReader &reader)
{
    const auto red = reader.u16();
    const auto green = reader.u16();
    const auto blue = reader.u16();
    const auto alpha = reader.u16();
    if (!red || !green || !blue || !alpha)
        return std::nullopt;
    return RenderColor{*red, *green, *blue, *alpha};
}

std::optional<RenderPoint>
read_point(WireReader &reader)
{
    const auto x = reader.u32();
    const auto y = reader.u32();
    if (!x || !y)
        return std::nullopt;
    return RenderPoint{signed_dword(*x), signed_dword(*y)};
}

bool
read_gradient_stops(WireReader &reader, std::uint32_t count,
                    std::vector<RenderGradientStop> &stops)
{
    if (count == 0 || count > maximum_render_gradient_stops)
        return false;
    std::vector<std::int32_t> positions;
    positions.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto position = reader.u32();
        if (!position)
            return false;
        const std::int32_t value = signed_dword(*position);
        if (value < 0 || value > 65536)
            return false;
        positions.push_back(value);
    }
    stops.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto color = read_color(reader);
        if (!color)
            return false;
        stops.push_back({positions[index], *color});
    }
    return std::is_sorted(
        stops.begin(), stops.end(),
        [](const RenderGradientStop &left,
           const RenderGradientStop &right) {
            return left.position < right.position;
        });
}

} // namespace

Result<void>
Connection::handle_render(const RequestContext &context)
{
    constexpr std::uint8_t bad_pict_format =
        render_extension.first_error;
    constexpr std::uint8_t bad_picture =
        render_extension.first_error + 1;
    constexpr std::uint8_t bad_glyph_set =
        render_extension.first_error + 3;
    constexpr std::uint8_t bad_glyph =
        render_extension.first_error + 4;
    const auto error = [&](std::uint8_t code, std::uint32_t value = 0) {
        return send_error(context.order, code, context.opcode,
                          context.sequence, value, context.data);
    };
    const auto render_result = [&](RenderStatus status,
                                   std::uint32_t value = 0) {
        switch (status) {
        case RenderStatus::success:
            return drain_pending_events();
        case RenderStatus::bad_picture:
            return error(bad_picture, value);
        case RenderStatus::bad_format:
            return error(bad_pict_format, value);
        case RenderStatus::bad_operator:
            return error(bad_value, value);
        case RenderStatus::bad_glyph_set:
            return error(bad_glyph_set, value);
        case RenderStatus::bad_glyph:
            return error(bad_glyph, value);
        case RenderStatus::bad_drawable:
            return error(bad_drawable, value);
        case RenderStatus::bad_match:
            return error(bad_match, value);
        case RenderStatus::bad_alloc:
            return error(bad_alloc);
        }
        return error(bad_request);
    };
    const auto check_new_resource = [&](std::uint32_t id) {
        if (!server_.valid_client_resource(id, config_.resource_base))
            return std::optional<Result<void>>(error(bad_id_choice, id));
        if (server_.resource_limit_reached(config_.resource_base))
            return std::optional<Result<void>>(error(bad_alloc));
        return std::optional<Result<void>>{};
    };

    try {
        switch (context.data) {
        case 0: { // QueryVersion
            if (context.request.size() != 12)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 8, context.order);
            if (!reader.u32() || !reader.u32())
                return malformed("truncated RENDER QueryVersion request");
            WireWriter reply(context.order);
            reply.u8(1);
            reply.pad(1);
            reply.u16(context.sequence);
            reply.u32(0);
            reply.u32(render_extension.major_version);
            reply.u32(render_extension.minor_version);
            reply.pad(16);
            return queue(reply.data());
        }
        case 1: { // QueryPictFormats
            if (context.request.size() != 4)
                return error(bad_length);
            constexpr std::uint32_t depth_count = 4;
            constexpr std::uint32_t visual_count = 1;
            WireWriter payload(context.order);
            for (const auto &format : render_formats()) {
                payload.u32(format.id);
                payload.u8(1); // Direct
                payload.u8(format.depth);
                payload.pad(2);
                payload.u16(format.direct.red_shift);
                payload.u16(format.direct.red_mask);
                payload.u16(format.direct.green_shift);
                payload.u16(format.direct.green_mask);
                payload.u16(format.direct.blue_shift);
                payload.u16(format.direct.blue_mask);
                payload.u16(format.direct.alpha_shift);
                payload.u16(format.direct.alpha_mask);
                payload.u32(0);
            }
            payload.u32(depth_count);
            payload.u32(render_xrgb32_format);
            for (const auto &format : render_formats()) {
                payload.u8(format.depth);
                payload.pad(1);
                payload.u16(format.depth == 24 ? 1 : 0);
                payload.pad(4);
                if (format.depth == 24) {
                    payload.u32(root_visual_id);
                    payload.u32(render_xrgb32_format);
                }
            }
            payload.u32(5); // SubPixelNone
            WireWriter reply(context.order);
            reply.u8(1);
            reply.pad(1);
            reply.u16(context.sequence);
            reply.u32(static_cast<std::uint32_t>(payload.size() / 4));
            reply.u32(static_cast<std::uint32_t>(render_formats().size()));
            reply.u32(1);
            reply.u32(depth_count);
            reply.u32(visual_count);
            reply.u32(1);
            reply.pad(4);
            reply.bytes(payload.data());
            return queue(reply.data());
        }
        case 2: { // QueryPictIndexValues
            if (context.request.size() != 8)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 4, context.order);
            const auto format = reader.u32();
            if (!format)
                return malformed(
                    "truncated RENDER QueryPictIndexValues request");
            if (render_format(*format) == nullptr)
                return error(bad_pict_format, *format);
            WireWriter reply(context.order);
            reply.u8(1);
            reply.pad(1);
            reply.u16(context.sequence);
            reply.u32(0);
            reply.u32(0);
            reply.pad(20);
            return queue(reply.data());
        }
        case 4: { // CreatePicture
            if (context.request.size() < 20 ||
                (context.request.size() & 3U) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            const auto drawable = reader.u32();
            const auto format_id = reader.u32();
            const auto value_mask = reader.u32();
            if (!id || !drawable || !format_id || !value_mask)
                return malformed("truncated RENDER CreatePicture request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            const auto *format = render_format(*format_id);
            if (format == nullptr)
                return error(bad_pict_format, *format_id);
            const auto *surface = server_.drawable_surface(*drawable);
            if (surface == nullptr)
                return error(bad_drawable, *drawable);
            if (surface->depth() != format->depth)
                return error(bad_match, *drawable);
            RenderPicture picture{
                *id, *format_id, RenderDrawableSource{*drawable}, {}};
            if (const auto invalid = apply_picture_attributes(
                    server_, reader, *value_mask, picture.attributes,
                    true)) {
                return error(invalid->code, invalid->value);
            }
            if (!server_.add_render_picture(
                    std::move(picture), config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 5: { // ChangePicture
            if (context.request.size() < 12 ||
                (context.request.size() & 3U) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            const auto value_mask = reader.u32();
            if (!id || !value_mask)
                return malformed("truncated RENDER ChangePicture request");
            auto *picture = server_.render_picture(*id);
            if (picture == nullptr)
                return error(bad_picture, *id);
            RenderPictureAttributes revised = picture->attributes;
            if (const auto invalid = apply_picture_attributes(
                    server_, reader, *value_mask, revised,
                    std::holds_alternative<RenderDrawableSource>(
                        picture->source))) {
                return error(invalid->code, invalid->value);
            }
            auto alpha = revised.alpha_map_picture;
            for (std::size_t depth = 0; alpha; ++depth) {
                if (alpha.get() == picture || depth == 64)
                    return error(bad_match, revised.alpha_map);
                alpha = alpha->attributes.alpha_map_picture;
            }
            picture->attributes = std::move(revised);
            return Result<void>::success();
        }
        case 6: { // SetPictureClipRectangles
            if (context.request.size() < 12 ||
                ((context.request.size() - 12) % 8) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            const auto x = reader.u16();
            const auto y = reader.u16();
            if (!id || !x || !y)
                return malformed(
                    "truncated RENDER SetPictureClipRectangles request");
            auto *picture = server_.render_picture(*id);
            if (picture == nullptr)
                return error(bad_picture, *id);
            if (!std::holds_alternative<RenderDrawableSource>(
                    picture->source)) {
                return error(bad_picture, *id);
            }
            const std::size_t count = reader.remaining() / 8;
            if (count > maximum_shape_rectangles)
                return error(bad_alloc);
            std::vector<Rectangle> rectangles;
            rectangles.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto left = reader.u16();
                const auto top = reader.u16();
                const auto width = reader.u16();
                const auto height = reader.u16();
                if (!left || !top || !width || !height)
                    return malformed("truncated RENDER clip rectangle");
                rectangles.push_back({
                    signed_word(*left), signed_word(*top), *width, *height});
            }
            Region clip;
            if (!Region::canonicalize(rectangles, clip))
                return error(bad_alloc);
            picture->attributes.clip_x_origin = signed_word(*x);
            picture->attributes.clip_y_origin = signed_word(*y);
            picture->attributes.clip = std::move(clip);
            return Result<void>::success();
        }
        case 7: { // FreePicture
            if (context.request.size() != 8)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 4, context.order);
            const auto id = reader.u32();
            if (!id)
                return malformed("truncated RENDER FreePicture request");
            if (!server_.erase_render_picture(*id))
                return error(bad_picture, *id);
            return Result<void>::success();
        }
        case 8: { // Composite
            if (context.request.size() != 36)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 32, context.order);
            const auto operation = reader.u8();
            if (!operation || !reader.skip(3))
                return malformed("truncated RENDER Composite operator");
            const auto source = reader.u32();
            const auto mask = reader.u32();
            const auto destination = reader.u32();
            const auto source_x = reader.u16();
            const auto source_y = reader.u16();
            const auto mask_x = reader.u16();
            const auto mask_y = reader.u16();
            const auto destination_x = reader.u16();
            const auto destination_y = reader.u16();
            const auto width = reader.u16();
            const auto height = reader.u16();
            if (!source || !mask || !destination || !source_x || !source_y ||
                !mask_x || !mask_y || !destination_x || !destination_y ||
                !width || !height) {
                return malformed("truncated RENDER Composite request");
            }
            if (!render_operator_valid(*operation))
                return error(bad_value, *operation);
            const auto *destination_picture =
                server_.render_picture(*destination);
            if (destination_picture == nullptr)
                return error(bad_picture, *destination);
            if (!std::holds_alternative<RenderDrawableSource>(
                    destination_picture->source)) {
                return error(bad_drawable, *destination);
            }
            if (server_.render_picture(*source) == nullptr)
                return error(bad_picture, *source);
            if (*mask != 0 && server_.render_picture(*mask) == nullptr)
                return error(bad_picture, *mask);
            RenderEngine render(server_);
            return render_result(render.composite(
                *operation, *source, *mask, *destination,
                signed_word(*source_x), signed_word(*source_y),
                signed_word(*mask_x), signed_word(*mask_y),
                signed_word(*destination_x), signed_word(*destination_y),
                *width, *height));
        }
        case 10: // Trapezoids
        case 11: // Triangles
        case 12: // TriStrip
        case 13: { // TriFan
            if (context.request.size() < 24)
                return error(bad_length);
            const std::size_t item_size = context.data == 10
                ? 40
                : (context.data == 11 ? 24 : 8);
            if (((context.request.size() - 24) % item_size) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto operation = reader.u8();
            if (!operation || !reader.skip(3))
                return malformed("truncated RENDER geometry operator");
            const auto source = reader.u32();
            const auto destination = reader.u32();
            const auto format = reader.u32();
            const auto source_x = reader.u16();
            const auto source_y = reader.u16();
            if (!source || !destination || !format || !source_x || !source_y)
                return malformed("truncated RENDER geometry request");
            if (!render_operator_valid(*operation))
                return error(bad_value, *operation);
            if (server_.render_picture(*source) == nullptr)
                return error(bad_picture, *source);
            const auto *destination_picture =
                server_.render_picture(*destination);
            if (destination_picture == nullptr)
                return error(bad_picture, *destination);
            if (!std::holds_alternative<RenderDrawableSource>(
                    destination_picture->source)) {
                return error(bad_drawable, *destination);
            }
            if (*format != 0 && render_format(*format) == nullptr)
                return error(bad_pict_format, *format);
            RenderEngine render(server_);
            if (context.data == 10) {
                std::vector<RenderTrapezoid> trapezoids;
                trapezoids.reserve(reader.remaining() / 40);
                while (reader.remaining() != 0) {
                    const auto top = reader.u32();
                    const auto bottom = reader.u32();
                    const auto left1 = read_point(reader);
                    const auto left2 = read_point(reader);
                    const auto right1 = read_point(reader);
                    const auto right2 = read_point(reader);
                    if (!top || !bottom || !left1 || !left2 ||
                        !right1 || !right2) {
                        return malformed("truncated RENDER trapezoid");
                    }
                    trapezoids.push_back({
                        signed_dword(*top), signed_dword(*bottom),
                        {*left1, *left2}, {*right1, *right2}});
                }
                return render_result(render.composite_trapezoids(
                    *operation, *source, *destination, *format,
                    signed_word(*source_x), signed_word(*source_y),
                    trapezoids));
            }
            std::vector<RenderTriangle> triangles;
            if (context.data == 11) {
                triangles.reserve(reader.remaining() / 24);
                while (reader.remaining() != 0) {
                    const auto p1 = read_point(reader);
                    const auto p2 = read_point(reader);
                    const auto p3 = read_point(reader);
                    if (!p1 || !p2 || !p3)
                        return malformed("truncated RENDER triangle");
                    triangles.push_back({*p1, *p2, *p3});
                }
            }
            else {
                std::vector<RenderPoint> points;
                points.reserve(reader.remaining() / 8);
                while (reader.remaining() != 0) {
                    const auto point = read_point(reader);
                    if (!point)
                        return malformed("truncated RENDER triangle point");
                    points.push_back(*point);
                }
                if (points.size() >= 3) {
                    triangles.reserve(points.size() - 2);
                    for (std::size_t index = 2; index < points.size();
                         ++index) {
                        if (context.data == 12) {
                            triangles.push_back(index % 2 == 0
                                ? RenderTriangle{
                                      points[index - 2], points[index - 1],
                                      points[index]}
                                : RenderTriangle{
                                      points[index - 1], points[index - 2],
                                      points[index]});
                        }
                        else {
                            triangles.push_back({
                                points[0], points[index - 1], points[index]});
                        }
                    }
                }
            }
            return render_result(render.composite_triangles(
                *operation, *source, *destination, *format,
                signed_word(*source_x), signed_word(*source_y), triangles));
        }
        case 17: { // CreateGlyphSet
            if (context.request.size() != 12)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 8, context.order);
            const auto id = reader.u32();
            const auto format = reader.u32();
            if (!id || !format)
                return malformed("truncated RENDER CreateGlyphSet request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            if (render_format(*format) == nullptr)
                return error(bad_pict_format, *format);
            auto storage = std::make_shared<RenderGlyphStorage>();
            storage->format = *format;
            if (!server_.add_render_glyph_set(
                    {*id, std::move(storage)}, config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 18: { // ReferenceGlyphSet
            if (context.request.size() != 12)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 8, context.order);
            const auto id = reader.u32();
            const auto existing = reader.u32();
            if (!id || !existing)
                return malformed(
                    "truncated RENDER ReferenceGlyphSet request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            const auto *source = server_.render_glyph_set(*existing);
            if (source == nullptr)
                return error(bad_glyph_set, *existing);
            if (!server_.add_render_glyph_set(
                    {*id, source->storage}, config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 19: { // FreeGlyphSet
            if (context.request.size() != 8)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 4, context.order);
            const auto id = reader.u32();
            if (!id)
                return malformed("truncated RENDER FreeGlyphSet request");
            if (!server_.erase_render_glyph_set(*id))
                return error(bad_glyph_set, *id);
            return Result<void>::success();
        }
        case 20: { // AddGlyphs
            if (context.request.size() < 12)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto glyph_set_id = reader.u32();
            const auto count = reader.u32();
            if (!glyph_set_id || !count)
                return malformed("truncated RENDER AddGlyphs request");
            auto *glyph_set = server_.render_glyph_set(*glyph_set_id);
            if (glyph_set == nullptr || !glyph_set->storage)
                return error(bad_glyph_set, *glyph_set_id);
            if (*count > maximum_render_glyphs_per_set ||
                *count > (reader.remaining() / 16U)) {
                return error(bad_alloc);
            }
            std::vector<std::uint32_t> ids;
            ids.reserve(*count);
            for (std::uint32_t index = 0; index < *count; ++index) {
                const auto id = reader.u32();
                if (!id)
                    return malformed("truncated RENDER glyph id list");
                ids.push_back(*id);
            }
            std::vector<RenderGlyphInfo> infos;
            infos.reserve(*count);
            for (std::uint32_t index = 0; index < *count; ++index) {
                const auto width = reader.u16();
                const auto height = reader.u16();
                const auto x = reader.u16();
                const auto y = reader.u16();
                const auto x_offset = reader.u16();
                const auto y_offset = reader.u16();
                if (!width || !height || !x || !y ||
                    !x_offset || !y_offset) {
                    return malformed("truncated RENDER glyph info list");
                }
                infos.push_back({
                    *width, *height, signed_word(*x), signed_word(*y),
                    signed_word(*x_offset), signed_word(*y_offset)});
            }

            const auto *format = render_format(glyph_set->storage->format);
            if (format == nullptr)
                return error(bad_pict_format, glyph_set->storage->format);
            std::vector<std::pair<std::uint32_t, RenderGlyph>> additions;
            additions.reserve(*count);
            const bool least_significant_bits =
                host_byte_order() == ByteOrder::little;
            for (std::uint32_t index = 0; index < *count; ++index) {
                const auto &info = infos[index];
                std::size_t stride = 0;
                if (format->depth == 1) {
                    stride =
                        ((static_cast<std::size_t>(info.width) + 31U) / 32U) *
                        4U;
                }
                else if (format->depth == 8) {
                    stride =
                        (static_cast<std::size_t>(info.width) + 3U) & ~3U;
                }
                else {
                    stride = static_cast<std::size_t>(info.width) * 4U;
                }
                const auto bytes = checked_multiply(
                    stride, static_cast<std::size_t>(info.height));
                const auto pixels = checked_multiply(
                    static_cast<std::size_t>(info.width),
                    static_cast<std::size_t>(info.height));
                if (!bytes || !pixels || *bytes > reader.remaining() ||
                    *pixels > maximum_render_glyph_bytes / 4U) {
                    return error(bad_length);
                }
                const std::size_t data_offset =
                    context.request.size() - reader.remaining();
                const auto *data = context.request.data() + data_offset;
                RenderGlyph glyph;
                glyph.info = info;
                glyph.pixels.resize(*pixels);
                for (std::uint16_t row = 0; row < info.height; ++row) {
                    for (std::uint16_t column = 0;
                         column < info.width; ++column) {
                        std::uint32_t pixel = 0;
                        if (format->depth == 1) {
                            const std::uint8_t byte = data[
                                static_cast<std::size_t>(row) * stride +
                                column / 8U];
                            const std::uint8_t mask = least_significant_bits
                                ? static_cast<std::uint8_t>(
                                      1U << (column & 7U))
                                : static_cast<std::uint8_t>(
                                      0x80U >> (column & 7U));
                            pixel = (byte & mask) != 0 ? 1 : 0;
                        }
                        else if (format->depth == 8) {
                            pixel = data[
                                static_cast<std::size_t>(row) * stride +
                                column];
                        }
                        else {
                            std::memcpy(
                                &pixel,
                                data + static_cast<std::size_t>(row) * stride +
                                    static_cast<std::size_t>(column) * 4U,
                                sizeof(pixel));
                            if (format->depth == 24)
                                pixel &= 0x00ffffffU;
                        }
                        glyph.pixels[
                            static_cast<std::size_t>(row) * info.width +
                            column] = pixel;
                    }
                }
                if (!reader.skip(*bytes))
                    return malformed("truncated RENDER glyph bitmap");
                additions.emplace_back(ids[index], std::move(glyph));
            }
            if (reader.remaining() != 0)
                return error(bad_length);

            auto revised = glyph_set->storage->glyphs;
            for (auto &addition : additions)
                revised[addition.first] = std::move(addition.second);
            if (revised.size() > maximum_render_glyphs_per_set)
                return error(bad_alloc);
            std::size_t revised_bytes = 0;
            for (const auto &entry : revised) {
                const auto bytes = checked_multiply(
                    entry.second.pixels.size(), sizeof(std::uint32_t));
                if (!bytes ||
                    revised_bytes > maximum_render_glyph_bytes - *bytes) {
                    return error(bad_alloc);
                }
                revised_bytes += *bytes;
            }
            if (!server_.render_glyph_storage_fits(
                    *glyph_set->storage, revised_bytes)) {
                return error(bad_alloc);
            }
            glyph_set->storage->glyphs = std::move(revised);
            glyph_set->storage->bytes = revised_bytes;
            return Result<void>::success();
        }
        case 22: { // FreeGlyphs
            if (context.request.size() < 8 ||
                ((context.request.size() - 8) & 3U) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto glyph_set_id = reader.u32();
            if (!glyph_set_id)
                return malformed("truncated RENDER FreeGlyphs request");
            auto *glyph_set = server_.render_glyph_set(*glyph_set_id);
            if (glyph_set == nullptr || !glyph_set->storage)
                return error(bad_glyph_set, *glyph_set_id);
            std::vector<std::uint32_t> ids;
            ids.reserve(reader.remaining() / 4);
            while (reader.remaining() != 0) {
                const auto id = reader.u32();
                if (!id)
                    return malformed("truncated RENDER FreeGlyphs list");
                if (glyph_set->storage->glyphs.count(*id) == 0)
                    return error(bad_glyph, *id);
                ids.push_back(*id);
            }
            for (const auto id : ids)
                glyph_set->storage->glyphs.erase(id);
            std::size_t bytes = 0;
            for (const auto &entry : glyph_set->storage->glyphs)
                bytes += entry.second.pixels.size() * sizeof(std::uint32_t);
            glyph_set->storage->bytes = bytes;
            return Result<void>::success();
        }
        case 23: // CompositeGlyphs8
        case 24: // CompositeGlyphs16
        case 25: { // CompositeGlyphs32
            if (context.request.size() < 28)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto operation = reader.u8();
            if (!operation || !reader.skip(3))
                return malformed("truncated RENDER glyph operator");
            const auto source = reader.u32();
            const auto destination = reader.u32();
            const auto format = reader.u32();
            const auto initial_glyph_set = reader.u32();
            const auto source_x = reader.u16();
            const auto source_y = reader.u16();
            if (!source || !destination || !format || !initial_glyph_set ||
                !source_x || !source_y) {
                return malformed("truncated RENDER CompositeGlyphs request");
            }
            if (!render_operator_valid(*operation))
                return error(bad_value, *operation);
            if (server_.render_picture(*source) == nullptr)
                return error(bad_picture, *source);
            const auto *destination_picture =
                server_.render_picture(*destination);
            if (destination_picture == nullptr)
                return error(bad_picture, *destination);
            if (!std::holds_alternative<RenderDrawableSource>(
                    destination_picture->source)) {
                return error(bad_drawable, *destination);
            }
            if (*format != 0 && render_format(*format) == nullptr)
                return error(bad_pict_format, *format);
            if (server_.render_glyph_set(*initial_glyph_set) == nullptr)
                return error(bad_glyph_set, *initial_glyph_set);
            const std::size_t glyph_size =
                context.data == 23 ? 1U : (context.data == 24 ? 2U : 4U);
            std::uint32_t current_glyph_set = *initial_glyph_set;
            std::vector<RenderGlyphRun> runs;
            std::size_t glyph_count = 0;
            while (reader.remaining() != 0) {
                if (reader.remaining() < 8)
                    return error(bad_length);
                const auto count = reader.u8();
                if (!count || !reader.skip(3))
                    return malformed("truncated RENDER glyph element");
                const auto x_offset = reader.u16();
                const auto y_offset = reader.u16();
                if (!x_offset || !y_offset)
                    return malformed("truncated RENDER glyph delta");
                if (*count == 0xff) {
                    const auto glyph_set_id = reader.u32();
                    if (!glyph_set_id)
                        return error(bad_length);
                    if (server_.render_glyph_set(*glyph_set_id) == nullptr)
                        return error(bad_glyph_set, *glyph_set_id);
                    current_glyph_set = *glyph_set_id;
                    continue;
                }
                const std::size_t bytes =
                    static_cast<std::size_t>(*count) * glyph_size;
                const std::size_t padded = (bytes + 3U) & ~3U;
                if (reader.remaining() < padded)
                    return error(bad_length);
                if (glyph_count > maximum_render_glyphs_per_set - *count)
                    return error(bad_alloc);
                RenderGlyphRun run{
                    signed_word(*x_offset), signed_word(*y_offset),
                    current_glyph_set, {}};
                run.glyphs.reserve(*count);
                for (std::uint8_t index = 0; index < *count; ++index) {
                    if (glyph_size == 1) {
                        const auto id = reader.u8();
                        if (!id)
                            return malformed("truncated RENDER 8-bit glyph");
                        run.glyphs.push_back(*id);
                    }
                    else if (glyph_size == 2) {
                        const auto id = reader.u16();
                        if (!id)
                            return malformed("truncated RENDER 16-bit glyph");
                        run.glyphs.push_back(*id);
                    }
                    else {
                        const auto id = reader.u32();
                        if (!id)
                            return malformed("truncated RENDER 32-bit glyph");
                        run.glyphs.push_back(*id);
                    }
                }
                if (!reader.skip(padded - bytes))
                    return error(bad_length);
                glyph_count += *count;
                runs.push_back(std::move(run));
            }
            RenderEngine render(server_);
            return render_result(render.composite_glyphs(
                *operation, *source, *destination, *format,
                signed_word(*source_x), signed_word(*source_y), runs));
        }
        case 26: { // FillRectangles
            if (context.request.size() < 20 ||
                ((context.request.size() - 20) % 8) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto operation = reader.u8();
            if (!operation || !reader.skip(3))
                return malformed("truncated RENDER FillRectangles operator");
            const auto destination = reader.u32();
            const auto color = read_color(reader);
            if (!destination || !color)
                return malformed("truncated RENDER FillRectangles request");
            if (!render_operator_valid(*operation))
                return error(bad_value, *operation);
            const auto *destination_picture =
                server_.render_picture(*destination);
            if (destination_picture == nullptr)
                return error(bad_picture, *destination);
            if (!std::holds_alternative<RenderDrawableSource>(
                    destination_picture->source)) {
                return error(bad_drawable, *destination);
            }
            std::vector<Rectangle> rectangles;
            rectangles.reserve(reader.remaining() / 8);
            while (reader.remaining() != 0) {
                const auto x = reader.u16();
                const auto y = reader.u16();
                const auto width = reader.u16();
                const auto height = reader.u16();
                if (!x || !y || !width || !height)
                    return malformed("truncated RENDER fill rectangle");
                rectangles.push_back({
                    signed_word(*x), signed_word(*y), *width, *height});
            }
            RenderEngine render(server_);
            return render_result(render.fill_rectangles(
                *operation, *destination, *color, rectangles));
        }
        case 27: { // CreateCursor
            if (context.request.size() != 16)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 12, context.order);
            const auto id = reader.u32();
            const auto source_id = reader.u32();
            const auto x_hot = reader.u16();
            const auto y_hot = reader.u16();
            if (!id || !source_id || !x_hot || !y_hot)
                return malformed("truncated RENDER CreateCursor request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            const auto *source = server_.render_picture(*source_id);
            if (source == nullptr)
                return error(bad_picture, *source_id);
            std::uint16_t width = 0;
            std::uint16_t height = 0;
            std::vector<std::uint32_t> pixels;
            RenderEngine render(server_);
            const RenderStatus captured = render.snapshot(
                *source_id, width, height, pixels);
            if (captured != RenderStatus::success)
                return render_result(captured, *source_id);
            if (*x_hot > width || *y_hot > height)
                return error(bad_match, *source_id);
            auto image = std::make_shared<CursorImage>();
            image->width = width;
            image->height = height;
            image->x_hot = *x_hot;
            image->y_hot = *y_hot;
            image->pixels = std::move(pixels);
            if (!server_.add_cursor(
                    {*id, std::move(image)}, config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 28: { // SetPictureTransform
            if (context.request.size() != 44)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 40, context.order);
            const auto id = reader.u32();
            if (!id)
                return malformed(
                    "truncated RENDER SetPictureTransform request");
            auto *picture = server_.render_picture(*id);
            if (picture == nullptr)
                return error(bad_picture, *id);
            std::array<std::int32_t, 9> transform;
            for (auto &element : transform) {
                const auto value = reader.u32();
                if (!value)
                    return malformed("truncated RENDER transform");
                element = signed_dword(*value);
            }
            picture->attributes.transform = transform;
            return Result<void>::success();
        }
        case 29: { // QueryFilters
            if (context.request.size() != 8)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 4, context.order);
            const auto drawable = reader.u32();
            if (!drawable)
                return malformed("truncated RENDER QueryFilters request");
            if (server_.drawable_surface(*drawable) == nullptr)
                return error(bad_drawable, *drawable);
            constexpr std::array<std::string_view, 2> filters{
                "nearest", "bilinear"};
            WireWriter payload(context.order);
            payload.u16(0xffff);
            payload.u16(0xffff);
            for (const auto filter : filters) {
                payload.u8(static_cast<std::uint8_t>(filter.size()));
                payload.bytes(filter);
            }
            payload.pad_to_four();
            WireWriter reply(context.order);
            reply.u8(1);
            reply.pad(1);
            reply.u16(context.sequence);
            reply.u32(static_cast<std::uint32_t>(payload.size() / 4));
            reply.u32(static_cast<std::uint32_t>(filters.size()));
            reply.u32(static_cast<std::uint32_t>(filters.size()));
            reply.pad(16);
            reply.bytes(payload.data());
            return queue(reply.data());
        }
        case 30: { // SetPictureFilter
            if (context.request.size() < 12)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            const auto name_length = reader.u16();
            if (!id || !name_length || !reader.skip(2))
                return malformed("truncated RENDER SetPictureFilter request");
            auto *picture = server_.render_picture(*id);
            if (picture == nullptr)
                return error(bad_picture, *id);
            const std::size_t padded =
                (static_cast<std::size_t>(*name_length) + 3U) & ~3U;
            if (reader.remaining() < padded)
                return error(bad_length);
            const std::size_t name_offset = context.request.size() -
                reader.remaining();
            const std::string_view name(
                reinterpret_cast<const char *>(
                    context.request.data() + name_offset),
                *name_length);
            if (!reader.skip(padded) || reader.remaining() != 0)
                return error(bad_match);
            if (name == "nearest")
                picture->attributes.filter = RenderFilter::nearest;
            else if (name == "bilinear")
                picture->attributes.filter = RenderFilter::bilinear;
            else
                return error(bad_name);
            return Result<void>::success();
        }
        case 31: { // CreateAnimCursor
            if (context.request.size() < 8 ||
                ((context.request.size() - 8) % 8) != 0)
                return error(bad_length);
            if (context.request.size() == 8)
                return error(bad_value);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            if (!id)
                return malformed("truncated RENDER CreateAnimCursor request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            auto image = std::make_shared<CursorImage>();
            while (reader.remaining() != 0) {
                const auto cursor_id = reader.u32();
                const auto delay = reader.u32();
                if (!cursor_id || !delay)
                    return malformed("truncated RENDER animated cursor");
                const auto *cursor = server_.cursor(*cursor_id);
                if (cursor == nullptr || !cursor->image)
                    return error(6, *cursor_id); // core BadCursor
                if (!cursor->image->frames.empty())
                    return error(bad_match, *cursor_id);
                if (image->frames.empty()) {
                    image->width = cursor->image->width;
                    image->height = cursor->image->height;
                    image->x_hot = cursor->image->x_hot;
                    image->y_hot = cursor->image->y_hot;
                    image->foreground = cursor->image->foreground;
                    image->background = cursor->image->background;
                }
                image->frames.emplace_back(cursor->image, *delay);
            }
            if (!server_.add_cursor(
                    {*id, std::move(image)}, config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 32: { // AddTraps
            if (context.request.size() < 12 ||
                ((context.request.size() - 12) % 24) != 0)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto picture = reader.u32();
            const auto x_offset = reader.u16();
            const auto y_offset = reader.u16();
            if (!picture || !x_offset || !y_offset)
                return malformed("truncated RENDER AddTraps request");
            const auto *destination_picture = server_.render_picture(*picture);
            if (destination_picture == nullptr)
                return error(bad_picture, *picture);
            if (!std::holds_alternative<RenderDrawableSource>(
                    destination_picture->source)) {
                return error(bad_drawable, *picture);
            }
            std::vector<RenderTrap> traps;
            traps.reserve(reader.remaining() / 24);
            while (reader.remaining() != 0) {
                RenderTrap trap;
                const auto top_left = reader.u32();
                const auto top_right = reader.u32();
                const auto top_y = reader.u32();
                const auto bottom_left = reader.u32();
                const auto bottom_right = reader.u32();
                const auto bottom_y = reader.u32();
                if (!top_left || !top_right || !top_y || !bottom_left ||
                    !bottom_right || !bottom_y) {
                    return malformed("truncated RENDER trap");
                }
                trap.top = {
                    signed_dword(*top_left), signed_dword(*top_right),
                    signed_dword(*top_y)};
                trap.bottom = {
                    signed_dword(*bottom_left), signed_dword(*bottom_right),
                    signed_dword(*bottom_y)};
                traps.push_back(trap);
            }
            RenderEngine render(server_);
            return render_result(render.add_traps(
                *picture, signed_word(*x_offset), signed_word(*y_offset),
                traps));
        }
        case 33: { // CreateSolidFill
            if (context.request.size() != 16)
                return error(bad_length);
            WireReader reader(context.request.data() + 4, 12, context.order);
            const auto id = reader.u32();
            const auto color = read_color(reader);
            if (!id || !color)
                return malformed("truncated RENDER CreateSolidFill request");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            if (!server_.add_render_picture(
                    {*id, render_argb32_format, RenderSolidSource{*color}, {}},
                    config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        case 34: // CreateLinearGradient
        case 35: // CreateRadialGradient
        case 36: { // CreateConicalGradient
            const std::size_t minimum =
                context.data == 34 ? 28 : (context.data == 35 ? 36 : 24);
            if (context.request.size() < minimum)
                return error(bad_length);
            WireReader reader(context.request.data() + 4,
                              context.request.size() - 4, context.order);
            const auto id = reader.u32();
            if (!id)
                return malformed("truncated RENDER gradient id");
            if (const auto invalid = check_new_resource(*id))
                return *invalid;
            RenderPicture picture;
            picture.id = *id;
            picture.format = render_argb32_format;
            std::uint32_t count = 0;
            if (context.data == 34) {
                const auto p1 = read_point(reader);
                const auto p2 = read_point(reader);
                const auto count_wire = reader.u32();
                if (!p1 || !p2 || !count_wire)
                    return malformed("truncated RENDER linear gradient");
                count = *count_wire;
                RenderLinearGradient gradient{*p1, *p2, {}};
                if (reader.remaining() != static_cast<std::size_t>(count) * 12U ||
                    !read_gradient_stops(reader, count, gradient.stops)) {
                    return error(bad_value, count);
                }
                picture.source = std::move(gradient);
            }
            else if (context.data == 35) {
                const auto inner = read_point(reader);
                const auto outer = read_point(reader);
                const auto inner_radius = reader.u32();
                const auto outer_radius = reader.u32();
                const auto count_wire = reader.u32();
                if (!inner || !outer || !inner_radius || !outer_radius ||
                    !count_wire) {
                    return malformed("truncated RENDER radial gradient");
                }
                count = *count_wire;
                RenderRadialGradient gradient{
                    *inner, *outer, signed_dword(*inner_radius),
                    signed_dword(*outer_radius), {}};
                if (reader.remaining() != static_cast<std::size_t>(count) * 12U ||
                    !read_gradient_stops(reader, count, gradient.stops)) {
                    return error(bad_value, count);
                }
                picture.source = std::move(gradient);
            }
            else {
                const auto center = read_point(reader);
                const auto angle = reader.u32();
                const auto count_wire = reader.u32();
                if (!center || !angle || !count_wire)
                    return malformed("truncated RENDER conical gradient");
                count = *count_wire;
                RenderConicalGradient gradient{
                    *center, signed_dword(*angle), {}};
                if (reader.remaining() != static_cast<std::size_t>(count) * 12U ||
                    !read_gradient_stops(reader, count, gradient.stops)) {
                    return error(bad_value, count);
                }
                picture.source = std::move(gradient);
            }
            if (!server_.add_render_picture(
                    std::move(picture), config_.resource_base)) {
                return error(bad_alloc);
            }
            return Result<void>::success();
        }
        default:
            return error(bad_request);
        }
    }
    catch (const std::bad_alloc &) {
        return error(bad_alloc);
    }
}

} // namespace xmin::server
