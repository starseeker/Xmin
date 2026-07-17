#ifndef XMIN_NEXT_CORE_RASTER_HPP
#define XMIN_NEXT_CORE_RASTER_HPP

#include "xmin/next/server_state.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xmin::next {

struct RasterPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct RasterArc {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t start_angle = 0;
    std::int16_t angle = 0;
};

void fill_gc_rectangle(Surface &surface, const GraphicsContextRecord &graphics,
                       const Rectangle &rectangle);
void draw_gc_line(Surface &surface, const GraphicsContextRecord &graphics,
                  RasterPoint start, RasterPoint end,
                  std::size_t &dash_phase);
void draw_gc_arc(Surface &surface, const GraphicsContextRecord &graphics,
                 const RasterArc &arc);
void fill_gc_polygon(Surface &surface, const GraphicsContextRecord &graphics,
                     const std::vector<RasterPoint> &points);
void fill_gc_arc(Surface &surface, const GraphicsContextRecord &graphics,
                 const RasterArc &arc);

} // namespace xmin::next

#endif
