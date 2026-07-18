#include "font_engine.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using xmin::client::font::EmbeddedFace;
using xmin::client::font::FontEngine;

bool check_face(EmbeddedFace face)
{
    FontEngine font(face, 18.0F);
    if (!font.valid() || font.metrics().ascent <= 0 ||
        font.metrics().descent < 0 || font.metrics().height < 16) {
        std::cerr << "invalid embedded font metrics\n";
        return false;
    }

    const std::uint32_t glyph = font.glyph_index(U'G');
    const auto bitmap = font.rasterize(glyph);
    if (glyph == 0 || bitmap.metrics.width <= 0 ||
        bitmap.metrics.height <= 0 || bitmap.metrics.advance <= 0 ||
        bitmap.coverage.size() !=
            static_cast<std::size_t>(bitmap.metrics.width) *
                static_cast<std::size_t>(bitmap.metrics.height)) {
        std::cerr << "failed to rasterize embedded glyph\n";
        return false;
    }

    bool has_coverage = false;
    for (const std::uint8_t value : bitmap.coverage) {
        has_coverage = has_coverage || value != 0;
    }
    if (!has_coverage || font.glyph_index(U'\u03a9') == 0) {
        std::cerr << "embedded font lacks expected WGL4 coverage\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    constexpr std::array faces{
        EmbeddedFace::sans_regular,
        EmbeddedFace::sans_bold,
        EmbeddedFace::sans_italic,
        EmbeddedFace::sans_bold_italic,
        EmbeddedFace::mono_regular,
        EmbeddedFace::mono_bold,
        EmbeddedFace::mono_italic,
        EmbeddedFace::mono_bold_italic,
    };
    for (const auto face : faces) {
        if (!check_face(face)) {
            return 1;
        }
    }

    FontEngine proportional(EmbeddedFace::sans_regular, 18.0F);
    FontEngine monospace(EmbeddedFace::mono_regular, 18.0F);
    const auto mono_i = monospace.glyph_metrics(monospace.glyph_index(U'i'));
    const auto mono_w = monospace.glyph_metrics(monospace.glyph_index(U'W'));
    const auto sans_i = proportional.glyph_metrics(proportional.glyph_index(U'i'));
    const auto sans_w = proportional.glyph_metrics(proportional.glyph_index(U'W'));
    if (mono_i.advance != mono_w.advance || sans_i.advance >= sans_w.advance) {
        std::cerr << "embedded family selection is not behaving as expected\n";
        return 1;
    }
    return 0;
}
