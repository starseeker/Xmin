#ifndef XMIN_SERVER_REGION_HPP
#define XMIN_SERVER_REGION_HPP

#include <cstdint>
#include <vector>

namespace xmin::server {

struct Rectangle {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class RegionOperation : std::uint8_t {
    set,
    unite,
    intersect,
    subtract,
    invert,
};

class Region {
public:
    [[nodiscard]] static bool canonicalize(
        const std::vector<Rectangle> &rectangles, Region &result);
    [[nodiscard]] static bool combine(
        RegionOperation operation, const Region &destination,
        const Region &source, Region &result);

    [[nodiscard]] bool contains(std::int64_t x,
                                std::int64_t y) const noexcept;
    [[nodiscard]] bool translate(std::int32_t x, std::int32_t y);
    [[nodiscard]] Rectangle extents() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return rectangles_.empty(); }
    [[nodiscard]] const std::vector<Rectangle> &rectangles() const noexcept
    {
        return rectangles_;
    }

private:
    std::vector<Rectangle> rectangles_;
};

struct ClipView {
    const Region *region = nullptr;
    std::int32_t x_origin = 0;
    std::int32_t y_origin = 0;

    [[nodiscard]] bool unrestricted() const noexcept
    {
        return region == nullptr;
    }

    [[nodiscard]] bool contains(std::int64_t x,
                                std::int64_t y) const noexcept
    {
        return region == nullptr ||
            region->contains(x - x_origin, y - y_origin);
    }
};

} // namespace xmin::server

#endif
