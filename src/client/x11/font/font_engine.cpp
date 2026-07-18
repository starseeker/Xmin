#include "font_engine.hpp"

#include <struetype.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace xmin::client::font {
namespace {

int rounded(float value) noexcept
{
    if (value <= static_cast<float>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    if (value >= static_cast<float>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(std::lround(value));
}

} // namespace

struct FontEngine::Impl {
    EmbeddedFace face = EmbeddedFace::sans_regular;
    float pixel_height = 0.0F;
    float scale = 0.0F;
    stt_fontinfo font{};
    FontMetrics metrics{};
    bool valid = false;
};

FontEngine::FontEngine(EmbeddedFace face, float pixel_height)
    : impl_(std::make_unique<Impl>())
{
    impl_->face = face;
    impl_->pixel_height = pixel_height;
    if (!std::isfinite(pixel_height) || pixel_height <= 0.0F) {
        return;
    }

    const EmbeddedFontBlob blob = embedded_font(face);
    if (blob.size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    const int data_size = static_cast<int>(blob.size);
    const int offset = stt_GetFontOffsetForIndex(blob.data, 0);
    if (offset < 0 || !stt_InitFont(&impl_->font, blob.data, data_size, offset)) {
        return;
    }

    impl_->scale = stt_ScaleForPixelHeight(&impl_->font, pixel_height);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stt_GetFontVMetrics(&impl_->font, &ascent, &descent, &line_gap);
    impl_->metrics.ascent = rounded(ascent * impl_->scale);
    impl_->metrics.descent = rounded(-descent * impl_->scale);
    impl_->metrics.line_gap = rounded(line_gap * impl_->scale);
    impl_->metrics.height = std::max(
        1, impl_->metrics.ascent + impl_->metrics.descent +
               impl_->metrics.line_gap);
    impl_->valid = true;
}

FontEngine::~FontEngine() = default;
FontEngine::FontEngine(FontEngine &&) noexcept = default;
FontEngine &FontEngine::operator=(FontEngine &&) noexcept = default;

bool FontEngine::valid() const noexcept
{
    return impl_ && impl_->valid;
}

EmbeddedFace FontEngine::face() const noexcept
{
    return impl_->face;
}

float FontEngine::pixel_height() const noexcept
{
    return impl_->pixel_height;
}

const FontMetrics &FontEngine::metrics() const noexcept
{
    return impl_->metrics;
}

std::uint32_t FontEngine::glyph_index(char32_t codepoint) const noexcept
{
    if (!valid() || codepoint > 0x10ffffU) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        stt_FindGlyphIndex(&impl_->font, static_cast<int>(codepoint)));
}

GlyphMetrics FontEngine::glyph_metrics(std::uint32_t glyph) const noexcept
{
    GlyphMetrics result{};
    result.glyph = glyph;
    if (!valid() || glyph >= static_cast<std::uint32_t>(impl_->font.numGlyphs)) {
        return result;
    }

    int advance = 0;
    int left_side_bearing = 0;
    stt_GetGlyphHMetrics(
        &impl_->font, static_cast<int>(glyph), &advance, &left_side_bearing);
    (void)left_side_bearing;
    stt_GetGlyphBitmapBox(
        &impl_->font, static_cast<int>(glyph), impl_->scale, impl_->scale,
        &result.x, &result.y, &result.width, &result.height);
    result.width -= result.x;
    result.height -= result.y;
    result.advance = rounded(advance * impl_->scale);
    return result;
}

GlyphBitmap FontEngine::rasterize(std::uint32_t glyph) const
{
    GlyphBitmap result{};
    result.metrics = glyph_metrics(glyph);
    if (!valid() || result.metrics.width <= 0 || result.metrics.height <= 0) {
        return result;
    }

    const std::size_t width = static_cast<std::size_t>(result.metrics.width);
    const std::size_t height = static_cast<std::size_t>(result.metrics.height);
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        return result;
    }
    result.coverage.resize(width * height);
    stt_MakeGlyphBitmap(
        &impl_->font, result.coverage.data(), result.metrics.width,
        result.metrics.height, result.metrics.width, impl_->scale,
        impl_->scale, static_cast<int>(glyph));
    return result;
}

int FontEngine::kerning(
    std::uint32_t left, std::uint32_t right) const noexcept
{
    if (!valid() || left >= static_cast<std::uint32_t>(impl_->font.numGlyphs) ||
        right >= static_cast<std::uint32_t>(impl_->font.numGlyphs)) {
        return 0;
    }
    return rounded(stt_GetGlyphKernAdvance(
                       &impl_->font, static_cast<int>(left),
                       static_cast<int>(right)) *
                   impl_->scale);
}

EmbeddedFace select_embedded_face(
    bool monospace, bool bold, bool italic) noexcept
{
    if (monospace) {
        if (bold && italic) {
            return EmbeddedFace::mono_bold_italic;
        }
        if (bold) {
            return EmbeddedFace::mono_bold;
        }
        if (italic) {
            return EmbeddedFace::mono_italic;
        }
        return EmbeddedFace::mono_regular;
    }
    if (bold && italic) {
        return EmbeddedFace::sans_bold_italic;
    }
    if (bold) {
        return EmbeddedFace::sans_bold;
    }
    if (italic) {
        return EmbeddedFace::sans_italic;
    }
    return EmbeddedFace::sans_regular;
}

} // namespace xmin::client::font
