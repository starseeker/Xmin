#include "xmin/next/color.hpp"

#include <array>
#include <cctype>
#include <string>

namespace xmin::next {
namespace {

struct NamedColor {
    std::string_view name;
    RgbColor color;
};

constexpr std::array<NamedColor, 16> named_colors{{
    {"black", {0x0000, 0x0000, 0x0000}},
    {"silver", {0xc0c0, 0xc0c0, 0xc0c0}},
    {"gray", {0x8080, 0x8080, 0x8080}},
    {"white", {0xffff, 0xffff, 0xffff}},
    {"maroon", {0x8080, 0x0000, 0x0000}},
    {"red", {0xffff, 0x0000, 0x0000}},
    {"purple", {0x8080, 0x0000, 0x8080}},
    {"fuchsia", {0xffff, 0x0000, 0xffff}},
    {"green", {0x0000, 0x8080, 0x0000}},
    {"lime", {0x0000, 0xffff, 0x0000}},
    {"olive", {0x8080, 0x8080, 0x0000}},
    {"yellow", {0xffff, 0xffff, 0x0000}},
    {"navy", {0x0000, 0x0000, 0x8080}},
    {"blue", {0x0000, 0x0000, 0xffff}},
    {"teal", {0x0000, 0x8080, 0x8080}},
    {"aqua", {0x0000, 0xffff, 0xffff}},
}};

std::optional<std::uint16_t>
hex_component(std::string_view digits)
{
    std::uint16_t value = 0;
    for (const char digit : digits) {
        value = static_cast<std::uint16_t>(value << 4);
        if (digit >= '0' && digit <= '9')
            value = static_cast<std::uint16_t>(value + digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            value = static_cast<std::uint16_t>(value + digit - 'a' + 10);
        else if (digit >= 'A' && digit <= 'F')
            value = static_cast<std::uint16_t>(value + digit - 'A' + 10);
        else
            return std::nullopt;
    }
    if (digits.size() == 1)
        return static_cast<std::uint16_t>(value * 0x1111U);
    if (digits.size() == 2)
        return static_cast<std::uint16_t>(value * 0x0101U);
    return value;
}

} // namespace

std::optional<RgbColor>
parse_color(std::string_view name)
{
    if (!name.empty() && name.front() == '#' &&
        (name.size() == 4 || name.size() == 7 || name.size() == 13)) {
        const std::size_t digits = (name.size() - 1) / 3;
        const auto red = hex_component(name.substr(1, digits));
        const auto green = hex_component(name.substr(1 + digits, digits));
        const auto blue = hex_component(name.substr(1 + digits * 2, digits));
        if (red && green && blue)
            return RgbColor{*red, *green, *blue};
        return std::nullopt;
    }

    std::string normalized;
    normalized.reserve(name.size());
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isspace(byte))
            normalized.push_back(static_cast<char>(std::tolower(byte)));
    }
    if (normalized == "grey")
        normalized = "gray";
    for (const auto &candidate : named_colors) {
        if (candidate.name == normalized)
            return candidate.color;
    }
    return std::nullopt;
}

std::uint32_t
true_color_pixel(RgbColor color) noexcept
{
    return (static_cast<std::uint32_t>(color.red >> 8) << 16) |
        (static_cast<std::uint32_t>(color.green >> 8) << 8) |
        static_cast<std::uint32_t>(color.blue >> 8);
}

RgbColor
true_color_rgb(std::uint32_t pixel) noexcept
{
    const auto red = static_cast<std::uint16_t>((pixel >> 16) & 0xffU);
    const auto green = static_cast<std::uint16_t>((pixel >> 8) & 0xffU);
    const auto blue = static_cast<std::uint16_t>(pixel & 0xffU);
    return RgbColor{
        static_cast<std::uint16_t>(red * 0x0101U),
        static_cast<std::uint16_t>(green * 0x0101U),
        static_cast<std::uint16_t>(blue * 0x0101U),
    };
}

} // namespace xmin::next
