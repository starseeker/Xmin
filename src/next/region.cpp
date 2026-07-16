#include "xmin/next/region.hpp"

#include <limits>
#include <new>
#include <pixman.h>
#include <utility>

namespace xmin::next {

bool
Region::canonicalize(const std::vector<Rectangle> &rectangles,
                     Region &result)
{
    std::vector<pixman_box32_t> boxes;
    try {
        boxes.reserve(rectangles.size());
        for (const auto &rectangle : rectangles) {
            if (rectangle.width == 0 || rectangle.height == 0)
                continue;
            const std::int64_t right =
                static_cast<std::int64_t>(rectangle.x) + rectangle.width;
            const std::int64_t bottom =
                static_cast<std::int64_t>(rectangle.y) + rectangle.height;
            if (right > std::numeric_limits<std::int32_t>::max() ||
                bottom > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            boxes.push_back(pixman_box32_t{
                rectangle.x, rectangle.y, static_cast<std::int32_t>(right),
                static_cast<std::int32_t>(bottom)});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }

    if (boxes.size() > static_cast<std::size_t>(
                           std::numeric_limits<int>::max())) {
        return false;
    }
    pixman_region32_t region;
    if (!pixman_region32_init_rects(
            &region, boxes.data(), static_cast<int>(boxes.size()))) {
        pixman_region32_fini(&region);
        return false;
    }

    int count = 0;
    const auto *canonical = pixman_region32_rectangles(&region, &count);
    std::vector<Rectangle> normalized;
    try {
        normalized.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            normalized.push_back(Rectangle{
                canonical[index].x1, canonical[index].y1,
                static_cast<std::uint32_t>(canonical[index].x2 -
                                           canonical[index].x1),
                static_cast<std::uint32_t>(canonical[index].y2 -
                                           canonical[index].y1)});
        }
    }
    catch (const std::bad_alloc &) {
        pixman_region32_fini(&region);
        return false;
    }
    pixman_region32_fini(&region);
    result.rectangles_ = std::move(normalized);
    return true;
}

bool
Region::contains(std::int64_t x, std::int64_t y) const noexcept
{
    for (const auto &rectangle : rectangles_) {
        if (x >= rectangle.x && y >= rectangle.y &&
            x < static_cast<std::int64_t>(rectangle.x) + rectangle.width &&
            y < static_cast<std::int64_t>(rectangle.y) + rectangle.height) {
            return true;
        }
    }
    return false;
}

} // namespace xmin::next
