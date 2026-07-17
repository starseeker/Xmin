#include "xmin/server/core_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace xmin::server {
namespace {

constexpr double pi = 3.14159265358979323846;

std::int32_t
positive_modulo(std::int32_t value, std::uint16_t divisor) noexcept
{
    const std::int32_t result = value % divisor;
    return result < 0 ? result + divisor : result;
}

void
fill_pixel(Surface &surface, const GraphicsContextRecord &graphics,
           std::int32_t x, std::int32_t y)
{
    std::uint32_t source = graphics.foreground;
    if (graphics.fill_style == 1 && graphics.tile) {
        source = graphics.tile->pixel(
            static_cast<std::uint16_t>(positive_modulo(
                x - graphics.tile_x_origin, graphics.tile->width())),
            static_cast<std::uint16_t>(positive_modulo(
                y - graphics.tile_y_origin, graphics.tile->height())));
    }
    else if ((graphics.fill_style == 2 || graphics.fill_style == 3) &&
             graphics.stipple) {
        const bool set = graphics.stipple->pixel(
            static_cast<std::uint16_t>(positive_modulo(
                x - graphics.tile_x_origin, graphics.stipple->width())),
            static_cast<std::uint16_t>(positive_modulo(
                y - graphics.tile_y_origin, graphics.stipple->height()))) != 0;
        if (!set && graphics.fill_style == 2)
            return;
        source = set ? graphics.foreground : graphics.background;
    }
    surface.draw_pixel(x, y, source, graphics.function, graphics.plane_mask,
                       graphics.clip());
}

std::pair<bool, std::uint32_t>
line_source(const GraphicsContextRecord &graphics,
            std::size_t phase) noexcept
{
    if (graphics.line_style == 0 || graphics.dash_count == 0)
        return {true, graphics.foreground};
    std::size_t offset = phase + graphics.dash_offset;
    std::size_t index = 0;
    while (true) {
        const std::size_t length = graphics.dashes[index];
        if (offset < length)
            break;
        offset -= length;
        index = (index + 1) % graphics.dash_count;
    }
    const bool foreground = (index & 1U) == 0;
    if (foreground)
        return {true, graphics.foreground};
    return graphics.line_style == 2
        ? std::pair<bool, std::uint32_t>{true, graphics.background}
        : std::pair<bool, std::uint32_t>{false, 0};
}

void
draw_brush(Surface &surface, const GraphicsContextRecord &graphics,
           std::int32_t x, std::int32_t y, std::uint32_t source)
{
    const std::int32_t width = std::max<std::int32_t>(1, graphics.line_width);
    const std::int32_t before = (width - 1) / 2;
    const std::int32_t after = width / 2;
    const std::int32_t left = std::max<std::int32_t>(0, x - before);
    const std::int32_t top = std::max<std::int32_t>(0, y - before);
    const std::int32_t right = std::min<std::int32_t>(
        surface.width() - 1, x + after);
    const std::int32_t bottom = std::min<std::int32_t>(
        surface.height() - 1, y + after);
    for (std::int32_t row = top; row <= bottom; ++row) {
        for (std::int32_t column = left; column <= right; ++column) {
            surface.draw_pixel(column, row, source, graphics.function,
                               graphics.plane_mask, graphics.clip());
        }
    }
}

bool
angle_contains(const RasterArc &arc, double angle) noexcept
{
    if (arc.angle >= 360 * 64 || arc.angle <= -360 * 64)
        return true;
    const auto normalize = [](double value) {
        value = std::fmod(value, 2.0 * pi);
        return value < 0 ? value + 2.0 * pi : value;
    };
    const double start = normalize(
        static_cast<double>(arc.start_angle) / 64.0 * pi / 180.0);
    const double candidate = normalize(angle);
    const double extent = static_cast<double>(arc.angle) /
        64.0 * pi / 180.0;
    if (extent >= 0)
        return normalize(candidate - start) <= extent;
    return normalize(start - candidate) <= -extent;
}

} // namespace

void
fill_gc_rectangle(Surface &surface, const GraphicsContextRecord &graphics,
                  const Rectangle &rectangle)
{
    if (graphics.fill_style == 0) {
        surface.fill(rectangle, graphics.foreground, graphics.function,
                     graphics.plane_mask, graphics.clip());
        return;
    }
    const std::int64_t right = static_cast<std::int64_t>(rectangle.x) +
        rectangle.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rectangle.y) +
        rectangle.height;
    for (std::int64_t y = std::max<std::int64_t>(0, rectangle.y);
         y < std::min<std::int64_t>(surface.height(), bottom); ++y) {
        for (std::int64_t x = std::max<std::int64_t>(0, rectangle.x);
             x < std::min<std::int64_t>(surface.width(), right); ++x) {
            fill_pixel(surface, graphics, static_cast<std::int32_t>(x),
                       static_cast<std::int32_t>(y));
        }
    }
}

void
draw_gc_line(Surface &surface, const GraphicsContextRecord &graphics,
             RasterPoint start, RasterPoint end, std::size_t &dash_phase)
{
    std::int64_t x = start.x;
    std::int64_t y = start.y;
    const std::int64_t end_x = end.x;
    const std::int64_t end_y = end.y;
    const std::int64_t delta_x = std::abs(end_x - x);
    const std::int64_t step_x = x < end_x ? 1 : -1;
    const std::int64_t delta_y = -std::abs(end_y - y);
    const std::int64_t step_y = y < end_y ? 1 : -1;
    std::int64_t error = delta_x + delta_y;
    for (;;) {
        const auto selected = line_source(graphics, dash_phase++);
        if (selected.first &&
            x >= std::numeric_limits<std::int32_t>::min() &&
            x <= std::numeric_limits<std::int32_t>::max() &&
            y >= std::numeric_limits<std::int32_t>::min() &&
            y <= std::numeric_limits<std::int32_t>::max()) {
            draw_brush(surface, graphics, static_cast<std::int32_t>(x),
                       static_cast<std::int32_t>(y), selected.second);
        }
        if (x == end_x && y == end_y)
            break;
        const std::int64_t twice_error = error * 2;
        if (twice_error >= delta_y) {
            error += delta_y;
            x += step_x;
        }
        if (twice_error <= delta_x) {
            error += delta_x;
            y += step_y;
        }
    }
}

void
draw_gc_arc(Surface &surface, const GraphicsContextRecord &graphics,
            const RasterArc &arc)
{
    if (arc.angle == 0)
        return;
    const double start = static_cast<double>(arc.start_angle) /
        64.0 * pi / 180.0;
    const double extent = std::clamp(
        static_cast<double>(arc.angle) / 64.0 * pi / 180.0,
        -2.0 * pi, 2.0 * pi);
    const std::size_t steps = std::max<std::size_t>(
        1, static_cast<std::size_t>(
            std::ceil((arc.width + arc.height) * std::abs(extent) / 4.0)));
    const auto point = [&arc](double angle) {
        return RasterPoint{
            static_cast<std::int32_t>(std::lround(
                arc.x + arc.width / 2.0 + arc.width / 2.0 * std::cos(angle))),
            static_cast<std::int32_t>(std::lround(
                arc.y + arc.height / 2.0 -
                arc.height / 2.0 * std::sin(angle)))};
    };
    RasterPoint previous = point(start);
    std::size_t dash_phase = 0;
    for (std::size_t index = 1; index <= steps; ++index) {
        const RasterPoint next = point(
            start + extent * static_cast<double>(index) / steps);
        draw_gc_line(surface, graphics, previous, next, dash_phase);
        previous = next;
    }
}

void
fill_gc_polygon(Surface &surface, const GraphicsContextRecord &graphics,
                const std::vector<RasterPoint> &points)
{
    if (points.size() < 3)
        return;
    const auto bounds = std::minmax_element(
        points.begin(), points.end(), [](const auto &left, const auto &right) {
            return left.y < right.y;
        });
    const std::int32_t first_y = std::max<std::int32_t>(0, bounds.first->y);
    const std::int32_t last_y = std::min<std::int32_t>(
        surface.height() - 1, bounds.second->y);
    std::vector<std::pair<double, int>> crossings;
    crossings.reserve(points.size());
    for (std::int32_t y = first_y; y <= last_y; ++y) {
        crossings.clear();
        const double scan = y + 0.5;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto &a = points[index];
            const auto &b = points[(index + 1) % points.size()];
            if ((a.y <= scan && b.y > scan) ||
                (b.y <= scan && a.y > scan)) {
                const double x = a.x +
                    (scan - a.y) * (b.x - a.x) / (b.y - a.y);
                crossings.emplace_back(x, b.y > a.y ? 1 : -1);
            }
        }
        std::sort(crossings.begin(), crossings.end(),
                  [](const auto &left, const auto &right) {
                      return left.first < right.first;
                  });
        int winding = 0;
        bool inside = false;
        double start = 0;
        for (const auto &crossing : crossings) {
            const bool was_inside = graphics.fill_rule == 0
                ? inside
                : winding != 0;
            if (graphics.fill_rule == 0)
                inside = !inside;
            else
                winding += crossing.second;
            const bool now_inside = graphics.fill_rule == 0
                ? inside
                : winding != 0;
            if (!was_inside && now_inside)
                start = crossing.first;
            else if (was_inside && !now_inside) {
                const std::int32_t first_x = std::max<std::int32_t>(
                    0, static_cast<std::int32_t>(std::ceil(start - 0.5)));
                const std::int32_t last_x = std::min<std::int32_t>(
                    surface.width() - 1,
                    static_cast<std::int32_t>(
                        std::floor(crossing.first - 0.5)));
                for (std::int32_t x = first_x; x <= last_x; ++x)
                    fill_pixel(surface, graphics, x, y);
            }
        }
    }
}

void
fill_gc_arc(Surface &surface, const GraphicsContextRecord &graphics,
            const RasterArc &arc)
{
    if (arc.width == 0 || arc.height == 0 || arc.angle == 0)
        return;
    const std::int32_t left = std::max<std::int32_t>(0, arc.x);
    const std::int32_t top = std::max<std::int32_t>(0, arc.y);
    const std::int32_t right = std::min<std::int32_t>(
        surface.width() - 1, arc.x + arc.width);
    const std::int32_t bottom = std::min<std::int32_t>(
        surface.height() - 1, arc.y + arc.height);
    const double center_x = arc.x + arc.width / 2.0;
    const double center_y = arc.y + arc.height / 2.0;
    for (std::int32_t y = top; y <= bottom; ++y) {
        for (std::int32_t x = left; x <= right; ++x) {
            const double normalized_x = (x + 0.5 - center_x) /
                (arc.width / 2.0);
            const double normalized_y = (y + 0.5 - center_y) /
                (arc.height / 2.0);
            if (normalized_x * normalized_x +
                    normalized_y * normalized_y > 1.0) {
                continue;
            }
            const double angle = std::atan2(-normalized_y, normalized_x);
            if (angle_contains(arc, angle))
                fill_pixel(surface, graphics, x, y);
        }
    }
}

} // namespace xmin::server
