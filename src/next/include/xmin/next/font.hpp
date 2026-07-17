#ifndef XMIN_NEXT_FONT_HPP
#define XMIN_NEXT_FONT_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace xmin::next {

struct EmbeddedGlyph {
    std::int16_t left = 0;
    std::int16_t right = 0;
    std::int16_t width = 0;
    std::int16_t ascent = 0;
    std::int16_t descent = 0;
    std::uint32_t row_offset = 0;
    std::uint16_t row_count = 0;
};

struct EmbeddedFont {
    std::string_view canonical_name;
    std::string_view alias;
    std::uint16_t minimum_character = 0;
    std::uint16_t maximum_character = 0;
    std::uint16_t default_character = 0;
    bool all_characters_exist = false;
    std::int16_t ascent = 0;
    std::int16_t descent = 0;
    const EmbeddedGlyph *glyphs = nullptr;
    std::size_t glyph_count = 0;
    const std::uint32_t *rows = nullptr;
    const std::uint16_t *encoding = nullptr;
    const std::uint8_t *defined = nullptr;
};

[[nodiscard]] const EmbeddedFont *find_font(std::string_view pattern) noexcept;
[[nodiscard]] std::vector<const EmbeddedFont *>
list_fonts(std::string_view pattern, std::size_t maximum);
[[nodiscard]] const EmbeddedGlyph &
font_glyph(const EmbeddedFont &font, std::uint16_t character) noexcept;
[[nodiscard]] bool
font_character_exists(const EmbeddedFont &font,
                      std::uint16_t character) noexcept;
[[nodiscard]] std::uint32_t
glyph_row(const EmbeddedFont &font, const EmbeddedGlyph &glyph,
          std::uint16_t row) noexcept;

} // namespace xmin::next

#endif
