#ifndef XMIN_CLIENT_X11_FONT_ENGINE_HPP
#define XMIN_CLIENT_X11_FONT_ENGINE_HPP

#include "embedded_fonts.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace xmin::client::font {

struct FontMetrics {
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    int height = 0;
};

struct GlyphMetrics {
    std::uint32_t glyph = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int advance = 0;
};

struct GlyphBitmap {
    GlyphMetrics metrics;
    std::vector<std::uint8_t> coverage;
};

class FontEngine {
public:
    FontEngine(EmbeddedFace face, float pixel_height);
    ~FontEngine();

    FontEngine(FontEngine &&) noexcept;
    FontEngine &operator=(FontEngine &&) noexcept;
    FontEngine(const FontEngine &) = delete;
    FontEngine &operator=(const FontEngine &) = delete;

    bool valid() const noexcept;
    EmbeddedFace face() const noexcept;
    float pixel_height() const noexcept;
    const FontMetrics &metrics() const noexcept;

    std::uint32_t glyph_index(char32_t codepoint) const noexcept;
    GlyphMetrics glyph_metrics(std::uint32_t glyph) const noexcept;
    GlyphBitmap rasterize(std::uint32_t glyph) const;
    int kerning(std::uint32_t left, std::uint32_t right) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

EmbeddedFace select_embedded_face(
    bool monospace, bool bold, bool italic) noexcept;

} // namespace xmin::client::font

#endif
