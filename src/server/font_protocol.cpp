#include "xmin/server/connection.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/font.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_value = 2,
    bad_font = 7,
    bad_match = 8,
    bad_drawable = 9,
    bad_alloc = 11,
    bad_graphics_context = 13,
    bad_id_choice = 14,
    bad_name = 15,
    bad_length = 16,
};

Result<void>
malformed_font(std::string_view message)
{
    return Result<void>::failure(ErrorCode::malformed, std::string(message));
}

void
write_character_info(WireWriter &writer, const EmbeddedGlyph &glyph)
{
    writer.i16(glyph.left);
    writer.i16(glyph.right);
    writer.i16(glyph.width);
    writer.i16(glyph.ascent);
    writer.i16(glyph.descent);
    writer.u16(0);
}

struct FontBounds {
    EmbeddedGlyph minimum;
    EmbeddedGlyph maximum;
};

FontBounds
font_bounds(const EmbeddedFont &font) noexcept
{
    FontBounds bounds;
    if (font.glyph_count == 0)
        return bounds;
    bounds.minimum = font.glyphs[0];
    bounds.maximum = font.glyphs[0];
    for (std::size_t index = 1; index < font.glyph_count; ++index) {
        const auto &glyph = font.glyphs[index];
        bounds.minimum.left = std::min(bounds.minimum.left, glyph.left);
        bounds.minimum.right = std::min(bounds.minimum.right, glyph.right);
        bounds.minimum.width = std::min(bounds.minimum.width, glyph.width);
        bounds.minimum.ascent = std::min(bounds.minimum.ascent, glyph.ascent);
        bounds.minimum.descent = std::min(bounds.minimum.descent, glyph.descent);
        bounds.maximum.left = std::max(bounds.maximum.left, glyph.left);
        bounds.maximum.right = std::max(bounds.maximum.right, glyph.right);
        bounds.maximum.width = std::max(bounds.maximum.width, glyph.width);
        bounds.maximum.ascent = std::max(bounds.maximum.ascent, glyph.ascent);
        bounds.maximum.descent = std::max(bounds.maximum.descent, glyph.descent);
    }
    return bounds;
}

void
write_font_metrics(WireWriter &writer, const EmbeddedFont &font,
                   std::uint32_t final_value)
{
    const auto bounds = font_bounds(font);
    write_character_info(writer, bounds.minimum);
    writer.pad(4);
    write_character_info(writer, bounds.maximum);
    writer.pad(4);
    writer.u16(font.minimum_character);
    writer.u16(font.maximum_character);
    writer.u16(font.default_character);
    writer.u16(0); // properties
    writer.u8(0);  // left-to-right
    writer.u8(0);  // linear eight-bit font
    writer.u8(0);
    writer.u8(font.all_characters_exist ? 1 : 0);
    writer.i16(font.ascent);
    writer.i16(font.descent);
    writer.u32(final_value);
}

std::string_view
listed_name(const EmbeddedFont &font, std::string_view pattern) noexcept
{
    if (pattern == font.alias)
        return font.alias;
    return font.canonical_name;
}

std::int32_t
text_width(const EmbeddedFont &font,
           const std::vector<std::uint16_t> &characters) noexcept
{
    std::int64_t width = 0;
    for (const auto character : characters)
        width += font_glyph(font, character).width;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        width, std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

std::int16_t
signed_coordinate(std::uint16_t value) noexcept
{
    const std::int32_t widened = value;
    return static_cast<std::int16_t>(
        widened <= std::numeric_limits<std::int16_t>::max()
            ? widened
            : widened - 65536);
}

} // namespace

Result<void>
Connection::handle_open_font(const RequestContext &context)
{
    if (context.request.size() < 12)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto id = reader.u32();
    const auto name_length = reader.u16();
    if (!id || !name_length || !reader.skip(2))
        return malformed_font("truncated OpenFont request");
    const auto padded = padded_to_four(*name_length);
    if (!padded || context.request.size() != 12 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::string_view name{
        reinterpret_cast<const char *>(context.request.data() + 12),
        *name_length};
    const auto *embedded = find_font(name);
    if (embedded == nullptr)
        return send_error(context.order, bad_name, context.opcode,
                          context.sequence);
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    if (!server_.add_font(FontRecord{*id, embedded}, config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    return Result<void>::success();
}

Result<void>
Connection::handle_close_font(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed_font("truncated CloseFont request");
    if (!server_.erase_font(*id))
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_query_font(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed_font("truncated QueryFont request");
    const EmbeddedFont *embedded = nullptr;
    if (const auto *record = server_.font(*id))
        embedded = record->font;
    else if (const auto *graphics = server_.graphics_context(*id))
        embedded = graphics->font;
    if (embedded == nullptr)
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, *id);

    const std::size_t character_count =
        static_cast<std::size_t>(embedded->maximum_character) -
        embedded->minimum_character + 1;
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(7 + character_count * 3));
    write_font_metrics(reply, *embedded,
                       static_cast<std::uint32_t>(character_count));
    for (std::uint32_t character = embedded->minimum_character;
         character <= embedded->maximum_character; ++character) {
        write_character_info(reply, font_glyph(*embedded, character));
    }
    return queue(reply.data());
}

Result<void>
Connection::handle_query_text_extents(const RequestContext &context)
{
    if (context.data > 1 || context.request.size() < 8 ||
        ((context.request.size() - 8) & 3U) != 0) {
        return send_error(context.order,
                          context.data > 1 ? bad_value : bad_length,
                          context.opcode, context.sequence, context.data);
    }
    WireReader header(context.request.data() + 4, 4, context.order);
    const auto id = header.u32();
    if (!id)
        return malformed_font("truncated QueryTextExtents request");
    const EmbeddedFont *embedded = nullptr;
    if (const auto *record = server_.font(*id))
        embedded = record->font;
    else if (const auto *graphics = server_.graphics_context(*id))
        embedded = graphics->font;
    if (embedded == nullptr)
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, *id);
    const std::size_t padded_characters = (context.request.size() - 8) / 2;
    if (padded_characters < context.data)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::size_t character_count = padded_characters - context.data;
    WireReader characters(context.request.data() + 8,
                          character_count * 2, context.order);
    std::int64_t width = 0;
    std::int64_t left = 0;
    std::int64_t right = 0;
    std::int16_t ascent = 0;
    std::int16_t descent = 0;
    bool first = true;
    for (std::size_t index = 0; index < character_count; ++index) {
        const auto byte1 = characters.u8();
        const auto byte2 = characters.u8();
        if (!byte1 || !byte2)
            return malformed_font("truncated QueryTextExtents string");
        const auto &glyph = font_glyph(
            *embedded, static_cast<std::uint16_t>(*byte1 << 8U | *byte2));
        const std::int64_t glyph_left = width + glyph.left;
        const std::int64_t glyph_right = width + glyph.right;
        if (first) {
            left = glyph_left;
            right = glyph_right;
            first = false;
        }
        else {
            left = std::min(left, glyph_left);
            right = std::max(right, glyph_right);
        }
        ascent = std::max(ascent, glyph.ascent);
        descent = std::max(descent, glyph.descent);
        width += glyph.width;
    }
    const auto bounded = [](std::int64_t value) {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(
            value, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    };
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(0);
    reply.i16(embedded->ascent);
    reply.i16(embedded->descent);
    reply.i16(ascent);
    reply.i16(descent);
    reply.u32(static_cast<std::uint32_t>(bounded(width)));
    reply.u32(static_cast<std::uint32_t>(bounded(left)));
    reply.u32(static_cast<std::uint32_t>(bounded(right)));
    reply.pad(4);
    return queue(reply.data());
}

Result<void>
Connection::handle_list_fonts(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto maximum = reader.u16();
    const auto pattern_length = reader.u16();
    if (!maximum || !pattern_length)
        return malformed_font("truncated ListFonts request");
    const auto padded = padded_to_four(*pattern_length);
    if (!padded || context.request.size() != 8 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::string_view pattern{
        reinterpret_cast<const char *>(context.request.data() + 8),
        *pattern_length};
    std::vector<const EmbeddedFont *> matches;
    try {
        matches = list_fonts(pattern, *maximum);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    WireWriter body(context.order);
    for (const auto *font : matches) {
        const auto name = listed_name(*font, pattern);
        body.u8(static_cast<std::uint8_t>(name.size()));
        body.bytes(name);
    }
    body.pad_to_four();
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(body.size() / 4));
    reply.u16(static_cast<std::uint16_t>(matches.size()));
    reply.pad(22);
    reply.bytes(body.data());
    return queue(reply.data());
}

Result<void>
Connection::handle_list_fonts_with_info(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto maximum = reader.u16();
    const auto pattern_length = reader.u16();
    if (!maximum || !pattern_length)
        return malformed_font("truncated ListFontsWithInfo request");
    const auto padded = padded_to_four(*pattern_length);
    if (!padded || context.request.size() != 8 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const std::string_view pattern{
        reinterpret_cast<const char *>(context.request.data() + 8),
        *pattern_length};
    std::vector<const EmbeddedFont *> matches;
    try {
        matches = list_fonts(pattern, *maximum);
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    for (std::size_t index = 0; index < matches.size(); ++index) {
        const auto &font = *matches[index];
        const auto name = listed_name(font, pattern);
        const auto name_size = padded_to_four(name.size());
        if (!name_size)
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        WireWriter reply(context.order);
        reply.u8(1);
        reply.u8(static_cast<std::uint8_t>(name.size()));
        reply.u16(context.sequence);
        reply.u32(static_cast<std::uint32_t>(7 + *name_size / 4));
        write_font_metrics(
            reply, font,
            static_cast<std::uint32_t>(matches.size() - index - 1));
        reply.bytes(name);
        reply.pad(*name_size - name.size());
        auto queued = queue(reply.data());
        if (!queued)
            return queued;
    }
    WireWriter terminator(context.order);
    terminator.u8(1);
    terminator.u8(0);
    terminator.u16(context.sequence);
    terminator.u32(7);
    terminator.pad(52);
    return queue(terminator.data());
}

Result<void>
Connection::handle_set_font_path(const RequestContext &context)
{
    if (context.request.size() < 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto count = reader.u16();
    if (!count || !reader.skip(2))
        return malformed_font("truncated SetFontPath request");
    std::vector<std::string_view> paths;
    try {
        paths.reserve(*count);
        for (std::size_t index = 0; index < *count; ++index) {
            const auto length = reader.u8();
            if (!length || reader.remaining() < *length)
                return send_error(context.order, bad_length, context.opcode,
                                  context.sequence);
            const std::size_t offset = context.request.size() - reader.remaining();
            paths.emplace_back(
                reinterpret_cast<const char *>(context.request.data() + offset),
                *length);
            if (!reader.skip(*length))
                return malformed_font("truncated SetFontPath element");
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    if (reader.remaining() > 3)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    if (!paths.empty() &&
        (paths.size() != 1 || paths.front() != "built-ins")) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_get_font_path(const RequestContext &context)
{
    if (context.request.size() != 4)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    constexpr std::string_view path = "built-ins";
    WireWriter body(context.order);
    body.u8(static_cast<std::uint8_t>(path.size()));
    body.bytes(path);
    body.pad_to_four();
    WireWriter reply(context.order);
    reply.u8(1);
    reply.u8(0);
    reply.u16(context.sequence);
    reply.u32(static_cast<std::uint32_t>(body.size() / 4));
    reply.u16(1);
    reply.pad(22);
    reply.bytes(body.data());
    return queue(reply.data());
}

Result<void>
Connection::paint_text(
    const RequestContext &context, std::uint32_t drawable,
    std::uint32_t graphics_id, const EmbeddedFont &font,
    const std::vector<std::uint16_t> &characters,
    std::int32_t x, std::int32_t baseline, bool image_text)
{
    auto *surface = server_.drawable_surface(drawable);
    if (surface == nullptr)
        return send_error(context.order, bad_drawable, context.opcode,
                          context.sequence, drawable);
    const auto *graphics = server_.graphics_context(graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, graphics_id);
    if (graphics->depth != surface->depth())
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    if (image_text) {
        const std::int32_t width = text_width(font, characters);
        if (width > 0) {
            surface->fill(
                Rectangle{x, baseline - font.ascent,
                          static_cast<std::uint32_t>(width),
                          static_cast<std::uint32_t>(font.ascent + font.descent)},
                graphics->background, 3, graphics->plane_mask,
                graphics->clip());
        }
    }
    std::int32_t current_x = x;
    for (const auto character : characters) {
        const auto &glyph = font_glyph(font, character);
        const std::uint16_t bitmap_width = static_cast<std::uint16_t>(
            std::max<std::int16_t>(0, glyph.right - glyph.left));
        for (std::uint16_t row = 0; row < glyph.row_count; ++row) {
            const std::uint32_t bits = glyph_row(font, glyph, row);
            for (std::uint16_t column = 0; column < bitmap_width; ++column) {
                if ((bits & (std::uint32_t{1} << column)) == 0)
                    continue;
                surface->draw_pixel(
                    current_x + glyph.left + column,
                    baseline - glyph.ascent + row,
                    graphics->foreground,
                    image_text ? 3 : graphics->function,
                    graphics->plane_mask, graphics->clip());
            }
        }
        current_x += glyph.width;
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_image_text(const RequestContext &context)
{
    const bool wide = context.opcode == 77;
    if (context.request.size() < 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    const auto byte_count = checked_multiply(
        static_cast<std::size_t>(context.data),
        static_cast<std::size_t>(wide ? 2 : 1));
    const auto padded = byte_count ? padded_to_four(*byte_count)
                                   : std::optional<std::size_t>{};
    if (!padded || context.request.size() != 16 + *padded)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4,
                      context.request.size() - 4, context.order);
    const auto drawable = reader.u32();
    const auto graphics_id = reader.u32();
    const auto x = reader.u16();
    const auto y = reader.u16();
    if (!drawable || !graphics_id || !x || !y)
        return malformed_font("truncated ImageText request");
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    if (graphics->font == nullptr)
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, 0);
    std::vector<std::uint16_t> characters;
    try {
        characters.reserve(context.data);
        for (std::size_t index = 0; index < context.data; ++index) {
            const auto byte1 = reader.u8();
            const auto byte2 = wide ? reader.u8()
                                    : std::optional<std::uint8_t>{};
            if (!byte1 || (wide && !byte2))
                return malformed_font("truncated ImageText string");
            characters.push_back(wide
                ? static_cast<std::uint16_t>(*byte1 << 8U | *byte2)
                : *byte1);
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    auto painted = paint_text(
        context, *drawable, *graphics_id, *graphics->font, characters,
        signed_coordinate(*x), signed_coordinate(*y), true);
    if (!painted)
        return painted;
    return finish_draw(context, *drawable);
}

Result<void>
Connection::handle_poly_text(const RequestContext &context)
{
    const bool wide = context.opcode == 75;
    if (context.request.size() < 16)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 12, context.order);
    const auto drawable = reader.u32();
    const auto graphics_id = reader.u32();
    const auto encoded_x = reader.u16();
    const auto encoded_y = reader.u16();
    if (!drawable || !graphics_id || !encoded_x || !encoded_y)
        return malformed_font("truncated PolyText request");
    const auto *graphics = server_.graphics_context(*graphics_id);
    if (graphics == nullptr)
        return send_error(context.order, bad_graphics_context,
                          context.opcode, context.sequence, *graphics_id);
    const EmbeddedFont *font = graphics->font;
    if (font == nullptr)
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, 0);
    std::int32_t x = signed_coordinate(*encoded_x);
    const std::int32_t y = signed_coordinate(*encoded_y);
    std::size_t offset = 16;
    while (offset < context.request.size()) {
        const std::size_t remaining = context.request.size() - offset;
        if (remaining <= 3 && std::all_of(
                context.request.begin() + static_cast<std::ptrdiff_t>(offset),
                context.request.end(), [](std::uint8_t value) {
                    return value == 0;
                })) {
            break;
        }
        const std::uint8_t count = context.request[offset++];
        if (count == 255) {
            if (context.request.size() - offset < 4)
                return send_error(context.order, bad_length, context.opcode,
                                  context.sequence);
            WireReader font_reader(
                context.request.data() + offset, 4, context.order);
            const auto font_id = font_reader.u32();
            if (!font_id)
                return malformed_font("truncated PolyText font shift");
            const auto *record = server_.font(*font_id);
            if (record == nullptr || record->font == nullptr)
                return send_error(context.order, bad_font, context.opcode,
                                  context.sequence, *font_id);
            font = record->font;
            offset += 4;
            continue;
        }
        const std::size_t character_bytes =
            static_cast<std::size_t>(count) * (wide ? 2U : 1U);
        if (context.request.size() - offset < 1 + character_bytes)
            return send_error(context.order, bad_length, context.opcode,
                              context.sequence);
        const std::uint8_t delta = context.request[offset++];
        x += delta <= 0x7fU ? delta : static_cast<std::int16_t>(delta) - 256;
        std::vector<std::uint16_t> characters;
        try {
            characters.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const std::uint8_t byte1 = context.request[offset++];
                const std::uint8_t byte2 = wide
                    ? context.request[offset++]
                    : 0;
                characters.push_back(wide
                    ? static_cast<std::uint16_t>(byte1 << 8U | byte2)
                    : byte1);
            }
        }
        catch (const std::bad_alloc &) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
        auto painted = paint_text(
            context, *drawable, *graphics_id, *font, characters, x, y, false);
        if (!painted)
            return painted;
        x += text_width(*font, characters);
    }
    return finish_draw(context, *drawable);
}

} // namespace xmin::server
