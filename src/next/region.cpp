#include "xmin/next/region.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <pixman.h>
#include <utility>

namespace xmin::next {
namespace {

class PixmanRegion {
public:
    explicit PixmanRegion(const std::vector<pixman_box32_t> &boxes)
        : valid_(pixman_region32_init_rects(
              &region_, boxes.data(), static_cast<int>(boxes.size())) != 0)
    {}

    ~PixmanRegion() { pixman_region32_fini(&region_); }

    PixmanRegion(const PixmanRegion &) = delete;
    PixmanRegion &operator=(const PixmanRegion &) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] pixman_region32_t *get() noexcept { return &region_; }
    [[nodiscard]] const pixman_region32_t *get() const noexcept
    {
        return &region_;
    }

private:
    pixman_region32_t region_{};
    bool valid_ = false;
};

bool
make_boxes(const std::vector<Rectangle> &rectangles,
           std::vector<pixman_box32_t> &boxes)
{
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
    return boxes.size() <=
        static_cast<std::size_t>(std::numeric_limits<int>::max());
}

bool
make_rectangles(const pixman_region32_t &region,
                std::vector<Rectangle> &rectangles)
{
    int count = 0;
    const auto *canonical = pixman_region32_rectangles(
        const_cast<pixman_region32_t *>(&region), &count);
    try {
        rectangles.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            rectangles.push_back(Rectangle{
                canonical[index].x1, canonical[index].y1,
                static_cast<std::uint32_t>(canonical[index].x2 -
                                           canonical[index].x1),
                static_cast<std::uint32_t>(canonical[index].y2 -
                                           canonical[index].y1)});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

} // namespace

bool
Region::canonicalize(const std::vector<Rectangle> &rectangles,
                     Region &result)
{
    std::vector<pixman_box32_t> boxes;
    if (!make_boxes(rectangles, boxes))
        return false;
    PixmanRegion region(boxes);
    if (!region.valid())
        return false;
    std::vector<Rectangle> normalized;
    if (!make_rectangles(*region.get(), normalized))
        return false;
    result.rectangles_ = std::move(normalized);
    return true;
}

bool
Region::combine(RegionOperation operation, const Region &destination,
                const Region &source, Region &result)
{
    if (operation == RegionOperation::set) {
        try {
            result = source;
        }
        catch (const std::bad_alloc &) {
            return false;
        }
        return true;
    }
    std::vector<pixman_box32_t> destination_boxes;
    std::vector<pixman_box32_t> source_boxes;
    if (!make_boxes(destination.rectangles_, destination_boxes) ||
        !make_boxes(source.rectangles_, source_boxes)) {
        return false;
    }
    PixmanRegion destination_region(destination_boxes);
    PixmanRegion source_region(source_boxes);
    const std::vector<pixman_box32_t> empty;
    PixmanRegion combined(empty);
    if (!destination_region.valid() || !source_region.valid() ||
        !combined.valid()) {
        return false;
    }

    pixman_bool_t succeeded = 0;
    switch (operation) {
    case RegionOperation::set:
        break;
    case RegionOperation::unite:
        succeeded = pixman_region32_union(
            combined.get(), destination_region.get(), source_region.get());
        break;
    case RegionOperation::intersect:
        succeeded = pixman_region32_intersect(
            combined.get(), destination_region.get(), source_region.get());
        break;
    case RegionOperation::subtract:
        succeeded = pixman_region32_subtract(
            combined.get(), destination_region.get(), source_region.get());
        break;
    case RegionOperation::invert:
        succeeded = pixman_region32_subtract(
            combined.get(), source_region.get(), destination_region.get());
        break;
    }
    if (succeeded == 0)
        return false;
    std::vector<Rectangle> normalized;
    if (!make_rectangles(*combined.get(), normalized))
        return false;
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

bool
Region::translate(std::int32_t x, std::int32_t y)
{
    std::vector<Rectangle> translated;
    try {
        translated.reserve(rectangles_.size());
        for (const auto &rectangle : rectangles_) {
            const std::int64_t translated_x =
                static_cast<std::int64_t>(rectangle.x) + x;
            const std::int64_t translated_y =
                static_cast<std::int64_t>(rectangle.y) + y;
            const std::int64_t right = translated_x + rectangle.width;
            const std::int64_t bottom = translated_y + rectangle.height;
            if (translated_x < std::numeric_limits<std::int32_t>::min() ||
                translated_y < std::numeric_limits<std::int32_t>::min() ||
                right > std::numeric_limits<std::int32_t>::max() ||
                bottom > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            translated.push_back(Rectangle{
                static_cast<std::int32_t>(translated_x),
                static_cast<std::int32_t>(translated_y), rectangle.width,
                rectangle.height});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    Region normalized;
    if (!canonicalize(translated, normalized))
        return false;
    *this = std::move(normalized);
    return true;
}

Rectangle
Region::extents() const noexcept
{
    if (rectangles_.empty())
        return {};
    std::int32_t left = rectangles_.front().x;
    std::int32_t top = rectangles_.front().y;
    std::int64_t right =
        static_cast<std::int64_t>(left) + rectangles_.front().width;
    std::int64_t bottom =
        static_cast<std::int64_t>(top) + rectangles_.front().height;
    for (const auto &rectangle : rectangles_) {
        left = std::min(left, rectangle.x);
        top = std::min(top, rectangle.y);
        right = std::max(
            right, static_cast<std::int64_t>(rectangle.x) + rectangle.width);
        bottom = std::max(
            bottom, static_cast<std::int64_t>(rectangle.y) + rectangle.height);
    }
    return Rectangle{
        left, top, static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top)};
}

} // namespace xmin::next
