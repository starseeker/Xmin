#ifndef XMIN_SERVER_COLOR_HPP
#define XMIN_SERVER_COLOR_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace xmin::server {

struct RgbColor {
    std::uint16_t red = 0;
    std::uint16_t green = 0;
    std::uint16_t blue = 0;
};

[[nodiscard]] std::optional<RgbColor> parse_color(std::string_view name);
[[nodiscard]] std::uint32_t true_color_pixel(RgbColor color) noexcept;
[[nodiscard]] RgbColor true_color_rgb(std::uint32_t pixel) noexcept;

} // namespace xmin::server

#endif
