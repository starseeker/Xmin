#ifndef XMIN_CLIENT_X11_FONT_EMBEDDED_FONTS_HPP
#define XMIN_CLIENT_X11_FONT_EMBEDDED_FONTS_HPP

#include <cstddef>
#include <cstdint>

namespace xmin::client::font {

enum class EmbeddedFace : std::uint8_t {
    sans_regular,
    sans_bold,
    sans_italic,
    sans_bold_italic,
    mono_regular,
    mono_bold,
    mono_italic,
    mono_bold_italic,
};

struct EmbeddedFontBlob {
    const unsigned char *data = nullptr;
    std::size_t size = 0;
};

EmbeddedFontBlob embedded_font(EmbeddedFace face) noexcept;

} // namespace xmin::client::font

#endif
