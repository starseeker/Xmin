#include "xmin/next/surface.hpp"

#include "xmin/next/checked.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace xmin::next {
namespace {

std::optional<std::size_t>
pixel_count(std::uint16_t width, std::uint16_t height)
{
    if (width == 0 || height == 0)
        return std::nullopt;
    const auto count = checked_multiply(
        static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    const auto bytes = count
        ? checked_multiply(*count, sizeof(std::uint32_t))
        : std::optional<std::size_t>{};
    if (!count || !bytes || *bytes > maximum_surface_bytes)
        return std::nullopt;
    return count;
}

std::uint32_t
raster(std::uint8_t function, std::uint32_t source,
       std::uint32_t destination) noexcept
{
    switch (function) {
    case 0:
        return 0;
    case 1:
        return source & destination;
    case 2:
        return source & ~destination;
    case 3:
        return source;
    case 4:
        return ~source & destination;
    case 5:
        return destination;
    case 6:
        return source ^ destination;
    case 7:
        return source | destination;
    case 8:
        return ~(source | destination);
    case 9:
        return ~(source ^ destination);
    case 10:
        return ~destination;
    case 11:
        return source | ~destination;
    case 12:
        return ~source;
    case 13:
        return ~source | destination;
    case 14:
        return ~(source & destination);
    case 15:
        return 0xffffffffU;
    default:
        return destination;
    }
}

} // namespace

Surface::Surface(std::uint16_t width, std::uint16_t height,
                 std::uint8_t depth, std::vector<std::uint32_t> pixels)
    : width_(width), height_(height), depth_(depth), pixels_(std::move(pixels))
{}

std::optional<Surface>
Surface::create(std::uint16_t width, std::uint16_t height, std::uint8_t depth)
{
    if (depth != 1 && depth != 8 && depth != 24 && depth != 32)
        return std::nullopt;
    const auto count = pixel_count(width, height);
    if (!count)
        return std::nullopt;
    try {
        return Surface(width, height, depth,
                       std::vector<std::uint32_t>(*count, 0));
    }
    catch (const std::bad_alloc &) {
        return std::nullopt;
    }
}

std::uint32_t
Surface::depth_mask() const noexcept
{
    if (depth_ == 32)
        return 0xffffffffU;
    return (std::uint32_t{1} << depth_) - 1U;
}

void
Surface::store(std::size_t index, std::uint32_t source,
               std::uint8_t function, std::uint32_t plane_mask) noexcept
{
    const std::uint32_t mask = plane_mask & depth_mask();
    const std::uint32_t destination = pixels_[index];
    const std::uint32_t result = raster(function, source, destination);
    pixels_[index] = ((result & mask) | (destination & ~mask)) & depth_mask();
}

bool
Surface::resize(std::uint16_t width, std::uint16_t height)
{
    const auto count = pixel_count(width, height);
    if (!count)
        return false;
    std::vector<std::uint32_t> replacement;
    try {
        replacement.assign(*count, 0);
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    const std::uint16_t copied_width = std::min(width_, width);
    const std::uint16_t copied_height = std::min(height_, height);
    for (std::uint16_t y = 0; y < copied_height; ++y) {
        std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(y) * width_,
                    copied_width,
                    replacement.begin() +
                        static_cast<std::ptrdiff_t>(y) * width);
    }
    width_ = width;
    height_ = height;
    pixels_ = std::move(replacement);
    return true;
}

void
Surface::fill(const Rectangle &rectangle, std::uint32_t source,
              std::uint8_t function, std::uint32_t plane_mask)
{
    const std::int64_t left = std::max<std::int64_t>(0, rectangle.x);
    const std::int64_t top = std::max<std::int64_t>(0, rectangle.y);
    const std::int64_t right = std::min<std::int64_t>(
        width_, static_cast<std::int64_t>(rectangle.x) + rectangle.width);
    const std::int64_t bottom = std::min<std::int64_t>(
        height_, static_cast<std::int64_t>(rectangle.y) + rectangle.height);
    for (std::int64_t y = top; y < bottom; ++y) {
        for (std::int64_t x = left; x < right; ++x) {
            store(static_cast<std::size_t>(y) * width_ +
                      static_cast<std::size_t>(x),
                  source, function, plane_mask);
        }
    }
}

void
Surface::copy_from(const Surface &source, std::int16_t source_x,
                   std::int16_t source_y, std::int16_t destination_x,
                   std::int16_t destination_y, std::uint16_t width,
                   std::uint16_t height, std::uint8_t function,
                   std::uint32_t plane_mask)
{
    const std::int64_t first_x = std::max<std::int64_t>(
        {0, -static_cast<std::int64_t>(source_x),
         -static_cast<std::int64_t>(destination_x)});
    const std::int64_t first_y = std::max<std::int64_t>(
        {0, -static_cast<std::int64_t>(source_y),
         -static_cast<std::int64_t>(destination_y)});
    const std::int64_t last_x = std::min<std::int64_t>(
        {width, static_cast<std::int64_t>(source.width_) - source_x,
         static_cast<std::int64_t>(width_) - destination_x});
    const std::int64_t last_y = std::min<std::int64_t>(
        {height, static_cast<std::int64_t>(source.height_) - source_y,
         static_cast<std::int64_t>(height_) - destination_y});
    if (first_x >= last_x || first_y >= last_y)
        return;

    std::int64_t first_row = first_y;
    std::int64_t after_last_row = last_y;
    std::int64_t row_step = 1;
    if (&source == this && destination_y > source_y) {
        first_row = last_y - 1;
        after_last_row = first_y - 1;
        row_step = -1;
    }
    std::int64_t first_column = first_x;
    std::int64_t after_last_column = last_x;
    std::int64_t column_step = 1;
    if (&source == this && destination_y == source_y &&
        destination_x > source_x) {
        first_column = last_x - 1;
        after_last_column = first_x - 1;
        column_step = -1;
    }
    for (std::int64_t row = first_row; row != after_last_row;
         row += row_step) {
        for (std::int64_t column = first_column;
             column != after_last_column; column += column_step) {
            const auto sx = static_cast<std::size_t>(source_x + column);
            const auto sy = static_cast<std::size_t>(source_y + row);
            const auto dx = static_cast<std::size_t>(destination_x + column);
            const auto dy = static_cast<std::size_t>(destination_y + row);
            const std::uint32_t source_pixel =
                source.pixels_[sy * source.width_ + sx];
            store(dy * width_ + dx, source_pixel, function, plane_mask);
        }
    }
}

std::uint32_t
Surface::pixel(std::uint16_t x, std::uint16_t y) const noexcept
{
    if (x >= width_ || y >= height_)
        return 0;
    return pixels_[static_cast<std::size_t>(y) * width_ + x];
}

} // namespace xmin::next
