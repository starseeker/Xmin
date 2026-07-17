#include "xmin/next/font.hpp"

#include "xmin/next/generated/core_fonts.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace xmin::next {
namespace {

char
fold(char value) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool
matches(std::string_view pattern, std::string_view value) noexcept
{
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' ||
             fold(pattern[pattern_index]) == fold(value[value_index]))) {
            ++pattern_index;
            ++value_index;
        }
        else if (pattern_index < pattern.size() &&
                 pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        }
        else if (star != std::string_view::npos) {
            pattern_index = star + 1;
            value_index = ++retry;
        }
        else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*')
        ++pattern_index;
    return pattern_index == pattern.size();
}

bool
matches_font(std::string_view pattern, const EmbeddedFont &font) noexcept
{
    if (matches(pattern, font.alias) || matches(pattern, font.canonical_name))
        return true;
    if (&font == &generated::fixed_font) {
        return matches(pattern, "6x13") ||
            matches(pattern,
                "-misc-fixed-medium-r-semicondensed--13-100-100-100-c-60-iso8859-1");
    }
    return false;
}

constexpr std::array<const EmbeddedFont *, 2> fonts{{
    &generated::fixed_font,
    &generated::cursor_font,
}};

} // namespace

const EmbeddedFont *
find_font(std::string_view pattern) noexcept
{
    for (const auto *font : fonts) {
        if (matches_font(pattern, *font))
            return font;
    }
    return nullptr;
}

std::vector<const EmbeddedFont *>
list_fonts(std::string_view pattern, std::size_t maximum)
{
    std::vector<const EmbeddedFont *> result;
    result.reserve(std::min(maximum, fonts.size()));
    for (const auto *font : fonts) {
        if (result.size() == maximum)
            break;
        if (matches_font(pattern, *font))
            result.push_back(font);
    }
    return result;
}

const EmbeddedGlyph &
font_glyph(const EmbeddedFont &font, std::uint16_t character) noexcept
{
    const std::uint16_t code = character <= 0xffU
        ? character
        : font.default_character;
    const std::uint16_t index = font.encoding[code];
    return font.glyphs[index < font.glyph_count ? index : 0];
}

bool
font_character_exists(const EmbeddedFont &font,
                      std::uint16_t character) noexcept
{
    return character <= 0xffU && font.defined[character] != 0;
}

std::uint32_t
glyph_row(const EmbeddedFont &font, const EmbeddedGlyph &glyph,
          std::uint16_t row) noexcept
{
    return row < glyph.row_count ? font.rows[glyph.row_offset + row] : 0;
}

} // namespace xmin::next
