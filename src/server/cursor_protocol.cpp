#include "xmin/server/connection.hpp"

#include "xmin/server/font.hpp"
#include "xmin/server/generated/core_protocol.hpp"

#include <algorithm>
#include <new>

namespace xmin::server {
namespace {

enum : std::uint8_t {
    bad_value = 2,
    bad_pixmap = 4,
    bad_cursor = 6,
    bad_font = 7,
    bad_match = 8,
    bad_alloc = 11,
    bad_id_choice = 14,
    bad_length = 16,
};

Result<void>
malformed_cursor(std::string message)
{
    return Result<void>::failure(ErrorCode::malformed, std::move(message));
}

} // namespace

Result<void>
Connection::handle_create_cursor(const RequestContext &context)
{
    if (context.request.size() != 32)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 28, context.order);
    const auto id = reader.u32();
    const auto source_id = reader.u32();
    const auto mask_id = reader.u32();
    const auto foreground_red = reader.u16();
    const auto foreground_green = reader.u16();
    const auto foreground_blue = reader.u16();
    const auto background_red = reader.u16();
    const auto background_green = reader.u16();
    const auto background_blue = reader.u16();
    const auto x_hot = reader.u16();
    const auto y_hot = reader.u16();
    if (!id || !source_id || !mask_id || !foreground_red ||
        !foreground_green || !foreground_blue || !background_red ||
        !background_green || !background_blue || !x_hot || !y_hot) {
        return malformed_cursor("truncated CreateCursor request");
    }
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    const auto *source = server_.pixmap(*source_id);
    if (source == nullptr)
        return send_error(context.order, bad_pixmap, context.opcode,
                          context.sequence, *source_id);
    const auto *mask = *mask_id == 0 ? nullptr : server_.pixmap(*mask_id);
    if (*mask_id != 0 && mask == nullptr)
        return send_error(context.order, bad_pixmap, context.opcode,
                          context.sequence, *mask_id);
    if (source->surface->depth() != 1 ||
        (mask != nullptr &&
         (mask->surface->depth() != 1 ||
          mask->surface->width() != source->surface->width() ||
          mask->surface->height() != source->surface->height())) ||
        *x_hot > source->surface->width() ||
        *y_hot > source->surface->height()) {
        return send_error(context.order, bad_match, context.opcode,
                          context.sequence);
    }
    try {
        auto image = std::make_shared<CursorImage>();
        image->width = source->surface->width();
        image->height = source->surface->height();
        image->x_hot = *x_hot;
        image->y_hot = *y_hot;
        image->foreground = {
            *foreground_red, *foreground_green, *foreground_blue, 0xffff};
        image->background = {
            *background_red, *background_green, *background_blue, 0xffff};
        image->pixels.resize(
            static_cast<std::size_t>(image->width) * image->height);
        image->pixel_roles.resize(image->pixels.size());
        for (std::uint16_t y = 0; y < image->height; ++y) {
            for (std::uint16_t x = 0; x < image->width; ++x) {
                const bool source_bit = source->surface->pixel(x, y) != 0;
                const bool mask_bit = mask == nullptr
                    ? true
                    : mask->surface->pixel(x, y) != 0;
                image->pixel_roles[
                    static_cast<std::size_t>(y) * image->width + x] =
                    mask_bit ? static_cast<std::uint8_t>(source_bit ? 2 : 1)
                             : 0;
            }
        }
        image->recolor(image->foreground, image->background);
        if (!server_.add_cursor(
                {*id, std::move(image)}, config_.resource_base)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_create_glyph_cursor(const RequestContext &context)
{
    if (context.request.size() != 32)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 28, context.order);
    const auto id = reader.u32();
    const auto source_font = reader.u32();
    const auto mask_font = reader.u32();
    const auto source_character = reader.u16();
    const auto mask_character = reader.u16();
    const auto foreground_red = reader.u16();
    const auto foreground_green = reader.u16();
    const auto foreground_blue = reader.u16();
    const auto background_red = reader.u16();
    const auto background_green = reader.u16();
    const auto background_blue = reader.u16();
    if (!id || !source_font || !mask_font || !source_character ||
        !mask_character || !foreground_red || !foreground_green ||
        !foreground_blue || !background_red || !background_green ||
        !background_blue) {
        return malformed_cursor("truncated CreateGlyphCursor request");
    }
    if (!server_.valid_client_resource(*id, config_.resource_base))
        return send_error(context.order, bad_id_choice, context.opcode,
                          context.sequence, *id);
    if (server_.resource_limit_reached(config_.resource_base))
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    const auto *source_record = server_.font(*source_font);
    if (source_record == nullptr || source_record->font == nullptr)
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, *source_font);
    const auto *mask_record = *mask_font == 0
        ? nullptr
        : server_.font(*mask_font);
    if (*mask_font != 0 &&
        (mask_record == nullptr || mask_record->font == nullptr)) {
        return send_error(context.order, bad_font, context.opcode,
                          context.sequence, *mask_font);
    }
    if (!font_character_exists(*source_record->font, *source_character))
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *source_character);
    if (mask_record != nullptr &&
        !font_character_exists(*mask_record->font, *mask_character)) {
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *mask_character);
    }

    const auto &source = font_glyph(
        *source_record->font, *source_character);
    const auto *mask = mask_record == nullptr
        ? nullptr
        : &font_glyph(*mask_record->font, *mask_character);
    const std::int16_t left = mask == nullptr
        ? source.left : std::min(source.left, mask->left);
    const std::int16_t right = mask == nullptr
        ? source.right : std::max(source.right, mask->right);
    const std::int16_t ascent = mask == nullptr
        ? source.ascent : std::max(source.ascent, mask->ascent);
    const std::int16_t descent = mask == nullptr
        ? source.descent : std::max(source.descent, mask->descent);
    const std::int32_t width = right - left;
    const std::int32_t height = ascent + descent;
    if (width <= 0 || height <= 0 || -static_cast<std::int32_t>(left) < 0)
        return send_error(context.order, bad_value, context.opcode,
                          context.sequence, *source_character);
    try {
        auto image = std::make_shared<CursorImage>();
        image->width = static_cast<std::uint16_t>(width);
        image->height = static_cast<std::uint16_t>(height);
        image->x_hot = static_cast<std::uint16_t>(-left);
        image->y_hot = static_cast<std::uint16_t>(ascent);
        image->foreground = {
            *foreground_red, *foreground_green, *foreground_blue, 0xffff};
        image->background = {
            *background_red, *background_green, *background_blue, 0xffff};
        image->pixels.resize(
            static_cast<std::size_t>(image->width) * image->height);
        image->pixel_roles.resize(image->pixels.size());
        const auto glyph_bit = [](
                const EmbeddedFont &font, const EmbeddedGlyph &glyph,
                std::int32_t x, std::int32_t y) noexcept {
            const std::int32_t column = x - glyph.left;
            const std::int32_t row = y + glyph.ascent;
            if (column < 0 || column >= glyph.right - glyph.left ||
                row < 0 || row >= glyph.row_count) {
                return false;
            }
            return (glyph_row(font, glyph, static_cast<std::uint16_t>(row)) &
                    (std::uint32_t{1} << column)) != 0;
        };
        for (std::int32_t y = -ascent; y < descent; ++y) {
            for (std::int32_t x = left; x < right; ++x) {
                const bool source_bit = glyph_bit(
                    *source_record->font, source, x, y);
                const bool mask_bit = mask == nullptr
                    ? true
                    : glyph_bit(*mask_record->font, *mask, x, y);
                image->pixel_roles[
                    static_cast<std::size_t>(y + ascent) * image->width +
                    static_cast<std::size_t>(x - left)] =
                    mask_bit ? static_cast<std::uint8_t>(source_bit ? 2 : 1)
                             : 0;
            }
        }
        image->recolor(image->foreground, image->background);
        if (!server_.add_cursor(
                {*id, std::move(image)}, config_.resource_base)) {
            return send_error(context.order, bad_alloc, context.opcode,
                              context.sequence);
        }
    }
    catch (const std::bad_alloc &) {
        return send_error(context.order, bad_alloc, context.opcode,
                          context.sequence);
    }
    return Result<void>::success();
}

Result<void>
Connection::handle_free_cursor(const RequestContext &context)
{
    if (context.request.size() != 8)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 4, context.order);
    const auto id = reader.u32();
    if (!id)
        return malformed_cursor("truncated FreeCursor request");
    if (!server_.erase_cursor(*id))
        return send_error(context.order, bad_cursor, context.opcode,
                          context.sequence, *id);
    return Result<void>::success();
}

Result<void>
Connection::handle_recolor_cursor(const RequestContext &context)
{
    if (context.request.size() != 20)
        return send_error(context.order, bad_length, context.opcode,
                          context.sequence);
    WireReader reader(context.request.data() + 4, 16, context.order);
    const auto id = reader.u32();
    const auto foreground_red = reader.u16();
    const auto foreground_green = reader.u16();
    const auto foreground_blue = reader.u16();
    const auto background_red = reader.u16();
    const auto background_green = reader.u16();
    const auto background_blue = reader.u16();
    if (!id || !foreground_red || !foreground_green || !foreground_blue ||
        !background_red || !background_green || !background_blue) {
        return malformed_cursor("truncated RecolorCursor request");
    }
    auto *cursor = server_.cursor(*id);
    if (cursor == nullptr || !cursor->image)
        return send_error(context.order, bad_cursor, context.opcode,
                          context.sequence, *id);
    cursor->image->recolor(
        {*foreground_red, *foreground_green, *foreground_blue, 0xffff},
        {*background_red, *background_green, *background_blue, 0xffff});
    return Result<void>::success();
}

} // namespace xmin::server
