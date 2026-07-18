/* Small client-side rectangle regions used by FLTK and Xft clipping. */
#include "xlib_internal.hpp"

#include <X11/Xregion.h>
#include <X11/Xutil.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

std::vector<BOX> boxes(Region region)
{
    if (region == nullptr || region->rects == nullptr || region->numRects <= 0)
        return {};
    return {region->rects, region->rects + region->numRects};
}

bool replace(Region region, const std::vector<BOX> &values)
{
    if (region == nullptr)
        return false;
    auto *storage = values.empty() ? nullptr : static_cast<BOX *>(
        std::malloc(values.size() * sizeof(BOX)));
    if (!values.empty() && storage == nullptr)
        return false;
    std::copy(values.begin(), values.end(), storage);
    std::free(region->rects);
    region->rects = storage;
    region->numRects = static_cast<long>(values.size());
    region->size = std::max<long>(region->numRects, 1);
    if (values.empty()) {
        region->extents = BOX{};
    }
    else {
        region->extents = values.front();
        for (const BOX &box : values) {
            region->extents.x1 = std::min(region->extents.x1, box.x1);
            region->extents.y1 = std::min(region->extents.y1, box.y1);
            region->extents.x2 = std::max(region->extents.x2, box.x2);
            region->extents.y2 = std::max(region->extents.y2, box.y2);
        }
    }
    return true;
}

bool valid(const BOX &box)
{
    return box.x1 < box.x2 && box.y1 < box.y2;
}

std::vector<BOX> subtract_box(
    const std::vector<BOX> &sources, const BOX &cut)
{
    std::vector<BOX> result;
    result.reserve(sources.size() * 4U);
    for (const BOX &source : sources) {
        const BOX overlap{
            std::max(source.x1, cut.x1), std::min(source.x2, cut.x2),
            std::max(source.y1, cut.y1), std::min(source.y2, cut.y2)};
        if (!valid(overlap)) {
            result.push_back(source);
            continue;
        }
        const BOX fragments[]{{source.x1, source.x2,
                               source.y1, overlap.y1},
                              {source.x1, source.x2,
                               overlap.y2, source.y2},
                              {source.x1, overlap.x1,
                               overlap.y1, overlap.y2},
                              {overlap.x2, source.x2,
                               overlap.y1, overlap.y2}};
        for (const BOX &fragment : fragments) {
            if (valid(fragment))
                result.push_back(fragment);
        }
    }
    return result;
}

} // namespace

extern "C" {

Region XCreateRegion(void)
{
    auto *region = static_cast<Region>(std::calloc(1, sizeof(REGION)));
    if (region != nullptr)
        region->size = 1;
    return region;
}

int XDestroyRegion(Region region)
{
    if (region != nullptr) {
        std::free(region->rects);
        std::free(region);
    }
    return 1;
}

Bool XEmptyRegion(Region region)
{
    return region == nullptr || region->numRects == 0;
}

int XClipBox(Region region, XRectangle *rectangle)
{
    if (region == nullptr || rectangle == nullptr)
        return 0;
    rectangle->x = region->extents.x1;
    rectangle->y = region->extents.y1;
    rectangle->width = static_cast<unsigned short>(
        std::max(0, region->extents.x2 - region->extents.x1));
    rectangle->height = static_cast<unsigned short>(
        std::max(0, region->extents.y2 - region->extents.y1));
    return 1;
}

int XUnionRectWithRegion(
    XRectangle *rectangle, Region source, Region destination)
{
    if (rectangle == nullptr || destination == nullptr)
        return 0;
    auto result = boxes(source);
    const BOX addition{
        rectangle->x,
        static_cast<short>(rectangle->x + rectangle->width),
        rectangle->y,
        static_cast<short>(rectangle->y + rectangle->height)};
    if (valid(addition))
        result.push_back(addition);
    return replace(destination, result);
}

int XUnionRegion(Region first, Region second, Region destination)
{
    if (destination == nullptr)
        return 0;
    auto result = boxes(first);
    const auto tail = boxes(second);
    result.insert(result.end(), tail.begin(), tail.end());
    return replace(destination, result);
}

int XIntersectRegion(Region first, Region second, Region destination)
{
    if (destination == nullptr)
        return 0;
    const auto left = boxes(first);
    const auto right = boxes(second);
    std::vector<BOX> result;
    result.reserve(left.size() * right.size());
    for (const BOX &a : left) {
        for (const BOX &b : right) {
            const BOX intersection{
                std::max(a.x1, b.x1), std::min(a.x2, b.x2),
                std::max(a.y1, b.y1), std::min(a.y2, b.y2)};
            if (valid(intersection))
                result.push_back(intersection);
        }
    }
    return replace(destination, result);
}

int XSubtractRegion(Region first, Region second, Region destination)
{
    if (destination == nullptr)
        return 0;
    std::vector<BOX> result = boxes(first);
    for (const BOX &cut : boxes(second))
        result = subtract_box(result, cut);
    return replace(destination, result);
}

int XRectInRegion(Region region, int x, int y,
                  unsigned int width, unsigned int height)
{
    if (region == nullptr || width == 0 || height == 0)
        return RectangleOut;
    const BOX wanted{
        static_cast<short>(x), static_cast<short>(x + width),
        static_cast<short>(y), static_cast<short>(y + height)};
    if (!valid(wanted))
        return RectangleOut;
    bool intersects = false;
    std::vector<BOX> uncovered{wanted};
    for (const BOX &box : boxes(region)) {
        intersects = intersects || valid({
            std::max(box.x1, wanted.x1), std::min(box.x2, wanted.x2),
            std::max(box.y1, wanted.y1), std::min(box.y2, wanted.y2)});
        uncovered = subtract_box(uncovered, box);
        if (uncovered.empty())
            return RectangleIn;
    }
    return intersects ? RectanglePart : RectangleOut;
}

int XSetRegion(Display *display, GC gc, Region region)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection == nullptr || gc == nullptr || region == nullptr)
        return 0;
    std::vector<xcb_rectangle_t> rectangles;
    rectangles.reserve(static_cast<std::size_t>(region->numRects));
    for (const BOX &box : boxes(region)) {
        rectangles.push_back({
            box.x1, box.y1,
            static_cast<std::uint16_t>(box.x2 - box.x1),
            static_cast<std::uint16_t>(box.y2 - box.y1)});
    }
    xcb_set_clip_rectangles(
        connection, XCB_CLIP_ORDERING_UNSORTED,
        static_cast<xcb_gcontext_t>(XGContextFromGC(gc)), 0, 0,
        static_cast<std::uint32_t>(rectangles.size()), rectangles.data());
    return 1;
}

int XSetClipRectangles(
    Display *display, GC gc, int origin_x, int origin_y,
    XRectangle *values, int count, int ordering)
{
    auto *xcb = xmin::client::x11::xlib_connection(display);
    if (xcb == nullptr || gc == nullptr || count < 0 ||
        (count != 0 && values == nullptr))
        return 0;
    std::vector<xcb_rectangle_t> rectangles;
    rectangles.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        rectangles.push_back({values[index].x, values[index].y,
                              values[index].width, values[index].height});
    }
    xcb_set_clip_rectangles(
        xcb, static_cast<std::uint8_t>(ordering),
        static_cast<xcb_gcontext_t>(XGContextFromGC(gc)),
        static_cast<std::int16_t>(origin_x),
        static_cast<std::int16_t>(origin_y),
        static_cast<std::uint32_t>(rectangles.size()), rectangles.data());
    return 1;
}

} // extern "C"
