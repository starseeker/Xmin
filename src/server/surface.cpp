#include "xmin/server/surface.hpp"

#include "xmin/server/checked.hpp"
#include "xmin/server/shared_memory.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace xmin::server {
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

enum : unsigned {
    clip_left = 1U << 0,
    clip_right = 1U << 1,
    clip_top = 1U << 2,
    clip_bottom = 1U << 3,
};

unsigned
line_outcode(std::int64_t x, std::int64_t y, std::int64_t width,
             std::int64_t height) noexcept
{
    unsigned code = 0;
    if (x < 0)
        code |= clip_left;
    else if (x >= width)
        code |= clip_right;
    if (y < 0)
        code |= clip_top;
    else if (y >= height)
        code |= clip_bottom;
    return code;
}

std::uint64_t
unsigned_magnitude(std::int64_t value) noexcept
{
    return value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);
}

std::int64_t
scaled_offset(std::int64_t delta, std::int64_t numerator,
              std::int64_t denominator) noexcept
{
    const std::uint64_t magnitude =
        unsigned_magnitude(delta) * unsigned_magnitude(numerator) /
        unsigned_magnitude(denominator);
    const auto result = static_cast<std::int64_t>(magnitude);
    const bool negative =
        (delta < 0) ^ (numerator < 0) ^ (denominator < 0);
    return negative ? -result : result;
}

bool
clip_line_to_surface(std::int64_t &start_x, std::int64_t &start_y,
                     std::int64_t &end_x, std::int64_t &end_y,
                     std::int64_t width, std::int64_t height) noexcept
{
    unsigned start_code = line_outcode(start_x, start_y, width, height);
    unsigned end_code = line_outcode(end_x, end_y, width, height);
    while (true) {
        if ((start_code | end_code) == 0)
            return true;
        if ((start_code & end_code) != 0)
            return false;

        const unsigned code = start_code != 0 ? start_code : end_code;
        std::int64_t x = 0;
        std::int64_t y = 0;
        if ((code & clip_top) != 0) {
            y = 0;
            x = start_x + scaled_offset(end_x - start_x, y - start_y,
                                        end_y - start_y);
        }
        else if ((code & clip_bottom) != 0) {
            y = height - 1;
            x = start_x + scaled_offset(end_x - start_x, y - start_y,
                                        end_y - start_y);
        }
        else if ((code & clip_right) != 0) {
            x = width - 1;
            y = start_y + scaled_offset(end_y - start_y, x - start_x,
                                        end_x - start_x);
        }
        else {
            x = 0;
            y = start_y + scaled_offset(end_y - start_y, x - start_x,
                                        end_x - start_x);
        }

        if (code == start_code) {
            start_x = x;
            start_y = y;
            start_code = line_outcode(start_x, start_y, width, height);
        }
        else {
            end_x = x;
            end_y = y;
            end_code = line_outcode(end_x, end_y, width, height);
        }
    }
}

} // namespace

Surface::Surface(std::uint16_t width, std::uint16_t height,
                 std::uint8_t depth, std::vector<std::uint32_t> pixels)
    : width_(width), height_(height), depth_(depth), pixels_(std::move(pixels))
{}

Surface::Surface(std::uint16_t width, std::uint16_t height,
                 std::uint8_t depth, std::shared_ptr<SharedMemory> memory,
                 std::uint32_t *shared_pixels) noexcept
    : width_(width), height_(height), depth_(depth),
      shared_memory_(std::move(memory)), shared_pixels_(shared_pixels)
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

std::optional<Surface>
Surface::create_shared(std::uint16_t width, std::uint16_t height,
                       std::uint8_t depth,
                       std::shared_ptr<SharedMemory> memory,
                       std::size_t offset)
{
    if ((depth != 24 && depth != 32) || !memory || memory->read_only() ||
        (offset % alignof(std::uint32_t)) != 0)
        return std::nullopt;
    const auto count = pixel_count(width, height);
    const auto bytes = count
        ? checked_multiply(*count, sizeof(std::uint32_t))
        : std::optional<std::size_t>{};
    const auto end = bytes ? checked_add(offset, *bytes)
                           : std::optional<std::size_t>{};
    if (!end || *end > memory->size())
        return std::nullopt;
    auto *address = memory->writable_data() + offset;
    return Surface(width, height, depth, std::move(memory),
                   reinterpret_cast<std::uint32_t *>(address));
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
    auto *pixels = data();
    const std::uint32_t destination = pixels[index];
    const std::uint32_t result = raster(function, source, destination);
    pixels[index] = ((result & mask) | (destination & ~mask)) & depth_mask();
}

bool
Surface::resize(std::uint16_t width, std::uint16_t height)
{
    if (shared_pixels_ != nullptr)
        return width == width_ && height == height_;
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
        std::copy_n(data() + static_cast<std::ptrdiff_t>(y) * width_,
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
              std::uint8_t function, std::uint32_t plane_mask, ClipView clip)
{
    const std::int64_t target_left = rectangle.x;
    const std::int64_t target_top = rectangle.y;
    const std::int64_t target_right = target_left + rectangle.width;
    const std::int64_t target_bottom = target_top + rectangle.height;
    const auto fill_intersection = [&](std::int64_t clip_left,
                                       std::int64_t clip_top,
                                       std::int64_t clip_right,
                                       std::int64_t clip_bottom) {
        const std::int64_t left = std::max<std::int64_t>(
            {0, target_left, clip_left});
        const std::int64_t top = std::max<std::int64_t>(
            {0, target_top, clip_top});
        const std::int64_t right = std::min<std::int64_t>(
            {width_, target_right, clip_right});
        const std::int64_t bottom = std::min<std::int64_t>(
            {height_, target_bottom, clip_bottom});
        for (std::int64_t y = top; y < bottom; ++y) {
            for (std::int64_t x = left; x < right; ++x) {
                store(static_cast<std::size_t>(y) * width_ +
                          static_cast<std::size_t>(x),
                      source, function, plane_mask);
            }
        }
    };

    if (clip.unrestricted()) {
        fill_intersection(0, 0, width_, height_);
        return;
    }
    for (const auto &clip_rectangle : clip.region->rectangles()) {
        const std::int64_t left =
            static_cast<std::int64_t>(clip_rectangle.x) + clip.x_origin;
        const std::int64_t top =
            static_cast<std::int64_t>(clip_rectangle.y) + clip.y_origin;
        fill_intersection(left, top, left + clip_rectangle.width,
                          top + clip_rectangle.height);
    }
}

void
Surface::draw_pixel(std::int32_t x, std::int32_t y, std::uint32_t source,
                    std::uint8_t function,
                    std::uint32_t plane_mask, ClipView clip) noexcept
{
    if (x < 0 || y < 0 || x >= width_ || y >= height_ ||
        !clip.contains(x, y)) {
        return;
    }
    store(static_cast<std::size_t>(y) * width_ +
              static_cast<std::size_t>(x),
          source, function, plane_mask);
}

void
Surface::draw_line(std::int32_t start_x, std::int32_t start_y,
                   std::int32_t end_x, std::int32_t end_y,
                   std::uint32_t source, std::uint8_t function,
                   std::uint32_t plane_mask, ClipView clip) noexcept
{
    std::int64_t x0 = start_x;
    std::int64_t y0 = start_y;
    std::int64_t x1 = end_x;
    std::int64_t y1 = end_y;
    if (!clip_line_to_surface(x0, y0, x1, y1, width_, height_))
        return;

    const std::int64_t delta_x = x1 >= x0 ? x1 - x0 : x0 - x1;
    const std::int64_t step_x = x0 < x1 ? 1 : -1;
    const std::int64_t delta_y = -(y1 >= y0 ? y1 - y0 : y0 - y1);
    const std::int64_t step_y = y0 < y1 ? 1 : -1;
    std::int64_t error = delta_x + delta_y;
    while (true) {
        draw_pixel(static_cast<std::int32_t>(x0),
                   static_cast<std::int32_t>(y0), source, function,
                   plane_mask, clip);
        if (x0 == x1 && y0 == y1)
            break;
        const std::int64_t twice_error = error * 2;
        if (twice_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (twice_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

void
Surface::copy_from(const Surface &source, std::int32_t source_x,
                   std::int32_t source_y, std::int32_t destination_x,
                   std::int32_t destination_y, std::uint32_t width,
                   std::uint32_t height, std::uint8_t function,
                   std::uint32_t plane_mask, ClipView clip)
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
            if (!clip.contains(static_cast<std::int64_t>(dx),
                               static_cast<std::int64_t>(dy))) {
                continue;
            }
            const std::uint32_t source_pixel =
                source.data()[sy * source.width_ + sx];
            store(dy * width_ + dx, source_pixel, function, plane_mask);
        }
    }
}

void
Surface::copy_plane_from(const Surface &source, std::int32_t source_x,
                         std::int32_t source_y,
                         std::int32_t destination_x,
                         std::int32_t destination_y, std::uint32_t width,
                         std::uint32_t height, std::uint32_t bit_plane,
                         std::uint32_t foreground, std::uint32_t background,
                         std::uint8_t function, std::uint32_t plane_mask,
                         ClipView clip)
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
            if (!clip.contains(static_cast<std::int64_t>(dx),
                               static_cast<std::int64_t>(dy))) {
                continue;
            }
            const std::uint32_t source_pixel =
                source.data()[sy * source.width_ + sx];
            store(dy * width_ + dx,
                  (source_pixel & bit_plane) != 0 ? foreground : background,
                  function, plane_mask);
        }
    }
}

std::uint32_t
Surface::pixel(std::uint16_t x, std::uint16_t y) const noexcept
{
    if (x >= width_ || y >= height_)
        return 0;
    return data()[static_cast<std::size_t>(y) * width_ + x];
}

} // namespace xmin::server
