#ifndef XMIN_CLIENT_X11_FONT_RENDER_FONT_HPP
#define XMIN_CLIENT_X11_FONT_RENDER_FONT_HPP

#include "font_engine.hpp"

#include <xcb/render.h>
#include <xcb/xcb.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace xmin::client::font {

class RenderFont {
public:
    RenderFont(
        xcb_connection_t *connection, EmbeddedFace face, float pixel_height);
    ~RenderFont();

    RenderFont(const RenderFont &) = delete;
    RenderFont &operator=(const RenderFont &) = delete;

    bool valid() const noexcept;
    FontEngine &font() noexcept;
    const FontEngine &font() const noexcept;
    xcb_render_pictformat_t format() const noexcept;
    xcb_render_glyphset_t glyphset() const noexcept;

    bool ensure_glyph(std::uint32_t glyph);
    bool draw(
        xcb_render_picture_t source, xcb_render_picture_t destination,
        int baseline_x, int baseline_y, const std::uint32_t *glyphs,
        std::size_t glyph_count);

private:
    bool ensure_glyph_locked(std::uint32_t glyph);

    xcb_connection_t *connection_ = nullptr;
    FontEngine font_;
    xcb_render_pictformat_t format_ = XCB_NONE;
    xcb_render_glyphset_t glyphset_ = XCB_NONE;
    std::unordered_set<std::uint32_t> uploaded_;
    mutable std::mutex mutex_;
};

} // namespace xmin::client::font

#endif
