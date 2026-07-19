#include "render_font.hpp"

#include <xcb/xcb_renderutil.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

namespace xmin::client::font {
namespace {

struct FreeDeleter {
    void operator()(void *pointer) const noexcept
    {
        std::free(pointer);
    }
};

bool checked(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> error(
        xcb_request_check(connection, cookie));
    return !error;
}

template <typename T>
bool fits(std::int64_t value) noexcept
{
    return value >= static_cast<std::int64_t>(std::numeric_limits<T>::min()) &&
           value <= static_cast<std::int64_t>(std::numeric_limits<T>::max());
}

template <typename T>
void append_native(std::vector<std::uint8_t> &output, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const std::size_t offset = output.size();
    output.resize(offset + sizeof(value));
    std::memcpy(output.data() + offset, &value, sizeof(value));
}

} // namespace

RenderFont::RenderFont(
    xcb_connection_t *connection, EmbeddedFace face, float pixel_height)
    : connection_(connection), font_(face, pixel_height)
{
    if (connection_ == nullptr || !font_.valid() ||
        xcb_connection_has_error(connection_) != 0) {
        return;
    }

    xcb_generic_error_t *raw_error = nullptr;
    std::unique_ptr<xcb_render_query_pict_formats_reply_t, FreeDeleter> formats(
        xcb_render_query_pict_formats_reply(
            connection_, xcb_render_query_pict_formats(connection_),
            &raw_error));
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> error(raw_error);
    if (!formats || error) {
        return;
    }
    const auto *a8 = xcb_render_util_find_standard_format(
        formats.get(), XCB_PICT_STANDARD_A_8);
    if (a8 == nullptr) {
        return;
    }
    format_ = a8->id;
    glyphset_ = xcb_generate_id(connection_);
    if (glyphset_ == XCB_NONE ||
        !checked(
            connection_, xcb_render_create_glyph_set_checked(
                             connection_, glyphset_, format_))) {
        glyphset_ = XCB_NONE;
        format_ = XCB_NONE;
    }
}

RenderFont::~RenderFont()
{
    if (connection_ != nullptr && glyphset_ != XCB_NONE &&
        xcb_connection_has_error(connection_) == 0) {
        xcb_render_free_glyph_set(connection_, glyphset_);
    }
}

bool RenderFont::valid() const noexcept
{
    return connection_ != nullptr && font_.valid() && format_ != XCB_NONE &&
           glyphset_ != XCB_NONE;
}

FontEngine &RenderFont::font() noexcept
{
    return font_;
}

const FontEngine &RenderFont::font() const noexcept
{
    return font_;
}

xcb_render_pictformat_t RenderFont::format() const noexcept
{
    return format_;
}

xcb_render_glyphset_t RenderFont::glyphset() const noexcept
{
    return glyphset_;
}

bool RenderFont::ensure_glyph(std::uint32_t glyph)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_glyph_locked(glyph);
}

bool RenderFont::ensure_glyph_locked(std::uint32_t glyph)
{
    if (!valid()) {
        return false;
    }
    if (uploaded_.count(glyph) != 0) {
        return true;
    }

    const GlyphBitmap bitmap = font_.rasterize(glyph);
    const auto &metrics = bitmap.metrics;
    if (!fits<std::uint16_t>(metrics.width) ||
        !fits<std::uint16_t>(metrics.height) ||
        !fits<std::int16_t>(-static_cast<std::int64_t>(metrics.x)) ||
        !fits<std::int16_t>(-static_cast<std::int64_t>(metrics.y)) ||
        !fits<std::int16_t>(metrics.advance)) {
        return false;
    }

    const xcb_render_glyphinfo_t info{
        static_cast<std::uint16_t>(metrics.width),
        static_cast<std::uint16_t>(metrics.height),
        static_cast<std::int16_t>(-metrics.x),
        static_cast<std::int16_t>(-metrics.y),
        static_cast<std::int16_t>(metrics.advance), 0};
    const std::size_t stride =
        (static_cast<std::size_t>(metrics.width) + 3U) & ~std::size_t{3};
    if (metrics.height > 0 &&
        stride > std::numeric_limits<std::size_t>::max() /
                     static_cast<std::size_t>(metrics.height)) {
        return false;
    }
    std::vector<std::uint8_t> padded(
        stride * static_cast<std::size_t>(metrics.height), 0);
    for (int row = 0; row < metrics.height; ++row) {
        std::copy_n(
            bitmap.coverage.data() +
                static_cast<std::size_t>(row) * metrics.width,
            metrics.width,
            padded.data() + static_cast<std::size_t>(row) * stride);
    }
    if (padded.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    if (!checked(
            connection_,
            xcb_render_add_glyphs_checked(
                connection_, glyphset_, 1, &glyph, &info,
                static_cast<std::uint32_t>(padded.size()), padded.data()))) {
        return false;
    }
    uploaded_.insert(glyph);
    return true;
}

bool RenderFont::draw(
    xcb_render_picture_t source, xcb_render_picture_t destination,
    int baseline_x, int baseline_y, const std::uint32_t *glyphs,
    std::size_t glyph_count)
{
    if ((glyphs == nullptr && glyph_count != 0) || glyph_count > 16384 ||
        !fits<std::int16_t>(baseline_x) || !fits<std::int16_t>(baseline_y)) {
        return false;
    }
    if (glyph_count == 0) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t index = 0; index < glyph_count; ++index) {
        if (!ensure_glyph_locked(glyphs[index])) {
            return false;
        }
    }

    std::vector<std::uint8_t> commands;
    commands.reserve(glyph_count * sizeof(std::uint32_t) +
                     ((glyph_count + 253U) / 254U) * 8U);
    std::size_t consumed = 0;
    while (consumed < glyph_count) {
        const std::size_t run_length =
            std::min<std::size_t>(254, glyph_count - consumed);
        append_native(commands, static_cast<std::uint8_t>(run_length));
        append_native(commands, std::uint8_t{0});
        append_native(commands, std::uint8_t{0});
        append_native(commands, std::uint8_t{0});
        append_native(
            commands, static_cast<std::int16_t>(
                          consumed == 0 ? baseline_x : 0));
        append_native(
            commands, static_cast<std::int16_t>(
                          consumed == 0 ? baseline_y : 0));
        for (std::size_t index = 0; index < run_length; ++index) {
            append_native(commands, glyphs[consumed + index]);
        }
        consumed += run_length;
    }
    if (commands.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    return checked(
        connection_, xcb_render_composite_glyphs_32_checked(
                         connection_, XCB_RENDER_PICT_OP_OVER, source,
                         destination, format_, glyphset_, 0, 0,
                         static_cast<std::uint32_t>(commands.size()),
                         commands.data()));
}

} // namespace xmin::client::font
