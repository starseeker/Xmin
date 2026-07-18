/* Focused Xlib core and drawing facade for FLTK/Tk. */
#include "xlib_internal.hpp"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <xcb/xcb_image.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

struct _XGC {
    GContext gid = 0;
    XGCValues values{};
};

namespace {

xcb_connection_t *connection(Display *display)
{
    return xmin::client::x11::xlib_connection(display);
}

xcb_gcontext_t gc_id(GC gc)
{
    return gc == nullptr ? XCB_NONE : static_cast<xcb_gcontext_t>(gc->gid);
}

template <typename Value>
void append_gc_value(
    std::vector<std::uint32_t> &result, unsigned long mask,
    unsigned long selected, Value value)
{
    if ((mask & selected) != 0)
        result.push_back(static_cast<std::uint32_t>(value));
}

std::vector<std::uint32_t> gc_value_list(
    unsigned long mask, const XGCValues &values)
{
    std::vector<std::uint32_t> result;
    result.reserve(23);
    append_gc_value(result, mask, GCFunction, values.function);
    append_gc_value(result, mask, GCPlaneMask, values.plane_mask);
    append_gc_value(result, mask, GCForeground, values.foreground);
    append_gc_value(result, mask, GCBackground, values.background);
    append_gc_value(result, mask, GCLineWidth, values.line_width);
    append_gc_value(result, mask, GCLineStyle, values.line_style);
    append_gc_value(result, mask, GCCapStyle, values.cap_style);
    append_gc_value(result, mask, GCJoinStyle, values.join_style);
    append_gc_value(result, mask, GCFillStyle, values.fill_style);
    append_gc_value(result, mask, GCFillRule, values.fill_rule);
    append_gc_value(result, mask, GCTile, values.tile);
    append_gc_value(result, mask, GCStipple, values.stipple);
    append_gc_value(result, mask, GCTileStipXOrigin, values.ts_x_origin);
    append_gc_value(result, mask, GCTileStipYOrigin, values.ts_y_origin);
    append_gc_value(result, mask, GCFont, values.font);
    append_gc_value(result, mask, GCSubwindowMode, values.subwindow_mode);
    append_gc_value(
        result, mask, GCGraphicsExposures, values.graphics_exposures);
    append_gc_value(result, mask, GCClipXOrigin, values.clip_x_origin);
    append_gc_value(result, mask, GCClipYOrigin, values.clip_y_origin);
    append_gc_value(result, mask, GCClipMask, values.clip_mask);
    append_gc_value(result, mask, GCDashOffset, values.dash_offset);
    append_gc_value(result, mask, GCDashList, values.dashes);
    append_gc_value(result, mask, GCArcMode, ArcPieSlice);
    return result;
}

void merge_gc_values(XGCValues &target, unsigned long mask,
                     const XGCValues &source)
{
#define XMIN_COPY_GC(bit, member) \
    do { if ((mask & (bit)) != 0) target.member = source.member; } while (false)
    XMIN_COPY_GC(GCFunction, function);
    XMIN_COPY_GC(GCPlaneMask, plane_mask);
    XMIN_COPY_GC(GCForeground, foreground);
    XMIN_COPY_GC(GCBackground, background);
    XMIN_COPY_GC(GCLineWidth, line_width);
    XMIN_COPY_GC(GCLineStyle, line_style);
    XMIN_COPY_GC(GCCapStyle, cap_style);
    XMIN_COPY_GC(GCJoinStyle, join_style);
    XMIN_COPY_GC(GCFillStyle, fill_style);
    XMIN_COPY_GC(GCFillRule, fill_rule);
    XMIN_COPY_GC(GCTile, tile);
    XMIN_COPY_GC(GCStipple, stipple);
    XMIN_COPY_GC(GCTileStipXOrigin, ts_x_origin);
    XMIN_COPY_GC(GCTileStipYOrigin, ts_y_origin);
    XMIN_COPY_GC(GCFont, font);
    XMIN_COPY_GC(GCSubwindowMode, subwindow_mode);
    XMIN_COPY_GC(GCGraphicsExposures, graphics_exposures);
    XMIN_COPY_GC(GCClipXOrigin, clip_x_origin);
    XMIN_COPY_GC(GCClipYOrigin, clip_y_origin);
    XMIN_COPY_GC(GCClipMask, clip_mask);
    XMIN_COPY_GC(GCDashOffset, dash_offset);
    XMIN_COPY_GC(GCDashList, dashes);
#undef XMIN_COPY_GC
}

unsigned long scale_component(
    unsigned short component, unsigned long mask)
{
    if (mask == 0)
        return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1UL) == 0)
        ++shift;
    const unsigned long maximum = mask >> shift;
    return ((static_cast<unsigned long>(component) * maximum + 32767UL) /
            65535UL) << shift;
}

unsigned short unscale_component(unsigned long pixel, unsigned long mask)
{
    if (mask == 0)
        return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1UL) == 0)
        ++shift;
    const unsigned long maximum = mask >> shift;
    return static_cast<unsigned short>(
        (((pixel & mask) >> shift) * 65535UL + maximum / 2UL) / maximum);
}

int configure_window(
    Display *display, Window window, std::uint16_t mask,
    const std::vector<std::uint32_t> &values)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return 0;
    xcb_configure_window(
        xcb, static_cast<xcb_window_t>(window), mask, values.data());
    return 1;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int default_error_handler(Display *, XErrorEvent *event) noexcept
{
    if (event != nullptr) {
        std::fprintf(
            stderr,
            "XminClient: X11 error %u on request %u.%u "
            "(resource 0x%lx, serial %lu)\n",
            event->error_code, event->request_code, event->minor_code,
            event->resourceid, event->serial);
    }
    return 0;
}

int default_io_error_handler(Display *) noexcept
{
    std::fprintf(stderr, "XminClient: X11 connection lost\n");
    return 0;
}

std::atomic<XErrorHandler> error_handler{default_error_handler};
std::atomic<XIOErrorHandler> io_error_handler{default_io_error_handler};

} // namespace

namespace xmin::client::x11 {

void xlib_dispatch_error(
    Display *display, const xcb_generic_error_t *error) noexcept
{
    if (error == nullptr)
        return;
    XErrorEvent event{};
    event.type = 0;
    event.display = display;
    event.resourceid = error->resource_id;
    event.serial = error->full_sequence != 0
        ? error->full_sequence
        : error->sequence;
    event.error_code = error->error_code;
    event.request_code = error->major_code;
    event.minor_code = error->minor_code;
    const XErrorHandler handler = error_handler.load();
    try {
        static_cast<void>((handler != nullptr ? handler : default_error_handler)(
            display, &event));
    }
    catch (...) {
    }
}

void xlib_dispatch_io_error(Display *display) noexcept
{
    const XIOErrorHandler handler = io_error_handler.load();
    try {
        static_cast<void>((handler != nullptr
            ? handler
            : default_io_error_handler)(display));
    }
    catch (...) {
    }
}

} // namespace xmin::client::x11

extern "C" {

XSizeHints *XAllocSizeHints(void)
{
    return static_cast<XSizeHints *>(std::calloc(1, sizeof(XSizeHints)));
}

XWMHints *XAllocWMHints(void)
{
    return static_cast<XWMHints *>(std::calloc(1, sizeof(XWMHints)));
}

int XBell(Display *display, int percent)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_bell(xcb, static_cast<std::int8_t>(std::clamp(percent, -100, 100)));
    return xcb != nullptr;
}

GC XCreateGC(
    Display *display, Drawable drawable, unsigned long value_mask,
    XGCValues *values)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || (value_mask != 0 && values == nullptr))
        return nullptr;
    auto *gc = new (std::nothrow) _XGC;
    if (gc == nullptr)
        return nullptr;
    gc->gid = xcb_generate_id(xcb);
    gc->values.function = GXcopy;
    gc->values.plane_mask = AllPlanes;
    gc->values.foreground = 0;
    gc->values.background = 1;
    gc->values.graphics_exposures = True;
    if (values != nullptr)
        merge_gc_values(gc->values, value_mask, *values);
    const auto encoded = gc_value_list(value_mask, gc->values);
    xcb_create_gc(
        xcb, static_cast<xcb_gcontext_t>(gc->gid),
        static_cast<xcb_drawable_t>(drawable),
        static_cast<std::uint32_t>(value_mask),
        encoded.empty() ? nullptr : encoded.data());
    return gc;
}

int XChangeGC(
    Display *display, GC gc, unsigned long value_mask, XGCValues *values)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || values == nullptr)
        return 0;
    merge_gc_values(gc->values, value_mask, *values);
    const auto encoded = gc_value_list(value_mask, *values);
    xcb_change_gc(xcb, gc_id(gc), static_cast<std::uint32_t>(value_mask),
                  encoded.empty() ? nullptr : encoded.data());
    return 1;
}

int XFreeGC(Display *display, GC gc)
{
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr)
        xcb_free_gc(xcb, gc_id(gc));
    delete gc;
    return xcb != nullptr;
}

Status XGetGCValues(
    Display *, GC gc, unsigned long value_mask, XGCValues *values)
{
    if (gc == nullptr || values == nullptr)
        return 0;
    *values = XGCValues{};
    merge_gc_values(*values, value_mask, gc->values);
    return 1;
}

GContext XGContextFromGC(GC gc)
{
    return gc == nullptr ? 0 : gc->gid;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground)
{
    XGCValues values{};
    values.foreground = foreground;
    return XChangeGC(display, gc, GCForeground, &values);
}

int XSetFillStyle(Display *display, GC gc, int fill_style)
{
    XGCValues values{};
    values.fill_style = fill_style;
    return XChangeGC(display, gc, GCFillStyle, &values);
}

int XSetStipple(Display *display, GC gc, Pixmap stipple)
{
    XGCValues values{};
    values.stipple = stipple;
    return XChangeGC(display, gc, GCStipple, &values);
}

int XSetTSOrigin(Display *display, GC gc, int x, int y)
{
    XGCValues values{};
    values.ts_x_origin = x;
    values.ts_y_origin = y;
    return XChangeGC(
        display, gc, GCTileStipXOrigin | GCTileStipYOrigin, &values);
}

int XSetClipOrigin(Display *display, GC gc, int x, int y)
{
    XGCValues values{};
    values.clip_x_origin = x;
    values.clip_y_origin = y;
    return XChangeGC(display, gc, GCClipXOrigin | GCClipYOrigin, &values);
}

int XSetClipMask(Display *display, GC gc, Pixmap pixmap)
{
    XGCValues values{};
    values.clip_mask = pixmap;
    return XChangeGC(display, gc, GCClipMask, &values);
}

int XSetLineAttributes(
    Display *display, GC gc, unsigned int width, int line_style,
    int cap_style, int join_style)
{
    XGCValues values{};
    values.line_width = static_cast<int>(width);
    values.line_style = line_style;
    values.cap_style = cap_style;
    values.join_style = join_style;
    return XChangeGC(
        display, gc, GCLineWidth | GCLineStyle | GCCapStyle | GCJoinStyle,
        &values);
}

int XSetDashes(
    Display *display, GC gc, int dash_offset, const char *dash_list,
    int count)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || dash_list == nullptr || count < 0)
        return 0;
    xcb_set_dashes(
        xcb, gc_id(gc), static_cast<std::uint16_t>(dash_offset),
        static_cast<std::uint16_t>(count),
        reinterpret_cast<const std::uint8_t *>(dash_list));
    return 1;
}

int XDrawPoint(Display *display, Drawable drawable, GC gc, int x, int y)
{
    XPoint point{static_cast<short>(x), static_cast<short>(y)};
    return XDrawPoints(display, drawable, gc, &point, 1, CoordModeOrigin);
}

int XDrawPoints(
    Display *display, Drawable drawable, GC gc, XPoint *points,
    int count, int mode)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || points == nullptr || count < 0)
        return 0;
    xcb_poly_point(
        xcb, static_cast<std::uint8_t>(mode),
        static_cast<xcb_drawable_t>(drawable), gc_id(gc),
        static_cast<std::uint32_t>(count),
        reinterpret_cast<const xcb_point_t *>(points));
    return 1;
}

int XDrawLine(
    Display *display, Drawable drawable, GC gc, int x1, int y1,
    int x2, int y2)
{
    XPoint points[2]{{static_cast<short>(x1), static_cast<short>(y1)},
                     {static_cast<short>(x2), static_cast<short>(y2)}};
    return XDrawLines(display, drawable, gc, points, 2, CoordModeOrigin);
}

int XDrawLines(
    Display *display, Drawable drawable, GC gc, XPoint *points,
    int count, int mode)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || points == nullptr || count < 0)
        return 0;
    xcb_poly_line(
        xcb, static_cast<std::uint8_t>(mode),
        static_cast<xcb_drawable_t>(drawable), gc_id(gc),
        static_cast<std::uint32_t>(count),
        reinterpret_cast<const xcb_point_t *>(points));
    return 1;
}

int XDrawRectangle(
    Display *display, Drawable drawable, GC gc, int x, int y,
    unsigned int width, unsigned int height)
{
    const xcb_rectangle_t rectangle{
        static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr)
        xcb_poly_rectangle(xcb, drawable, gc_id(gc), 1, &rectangle);
    return xcb != nullptr && gc != nullptr;
}

int XFillRectangle(
    Display *display, Drawable drawable, GC gc, int x, int y,
    unsigned int width, unsigned int height)
{
    const xcb_rectangle_t rectangle{
        static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr)
        xcb_poly_fill_rectangle(xcb, drawable, gc_id(gc), 1, &rectangle);
    return xcb != nullptr && gc != nullptr;
}

int XFillRectangles(
    Display *display, Drawable drawable, GC gc, XRectangle *rectangles,
    int count)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || rectangles == nullptr || count < 0)
        return 0;
    xcb_poly_fill_rectangle(
        xcb, drawable, gc_id(gc), static_cast<std::uint32_t>(count),
        reinterpret_cast<const xcb_rectangle_t *>(rectangles));
    return 1;
}

int XDrawArc(
    Display *display, Drawable drawable, GC gc, int x, int y,
    unsigned int width, unsigned int height, int angle1, int angle2)
{
    const xcb_arc_t arc{
        static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
        static_cast<std::int16_t>(angle1), static_cast<std::int16_t>(angle2)};
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr)
        xcb_poly_arc(xcb, drawable, gc_id(gc), 1, &arc);
    return xcb != nullptr && gc != nullptr;
}

int XFillArc(
    Display *display, Drawable drawable, GC gc, int x, int y,
    unsigned int width, unsigned int height, int angle1, int angle2)
{
    const xcb_arc_t arc{
        static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
        static_cast<std::int16_t>(angle1), static_cast<std::int16_t>(angle2)};
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr)
        xcb_poly_fill_arc(xcb, drawable, gc_id(gc), 1, &arc);
    return xcb != nullptr && gc != nullptr;
}

int XFillPolygon(
    Display *display, Drawable drawable, GC gc, XPoint *points,
    int count, int shape, int mode)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || points == nullptr || count < 0)
        return 0;
    xcb_fill_poly(
        xcb, drawable, gc_id(gc), static_cast<std::uint8_t>(shape),
        static_cast<std::uint8_t>(mode), static_cast<std::uint32_t>(count),
        reinterpret_cast<const xcb_point_t *>(points));
    return 1;
}

int XCopyArea(
    Display *display, Drawable source, Drawable destination, GC gc,
    int source_x, int source_y, unsigned int width, unsigned int height,
    int destination_x, int destination_y)
{
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr) {
        xcb_copy_area(
            xcb, source, destination, gc_id(gc),
            static_cast<std::int16_t>(source_x),
            static_cast<std::int16_t>(source_y),
            static_cast<std::int16_t>(destination_x),
            static_cast<std::int16_t>(destination_y),
            static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height));
    }
    return xcb != nullptr && gc != nullptr;
}

int XCopyPlane(
    Display *display, Drawable source, Drawable destination, GC gc,
    int source_x, int source_y, unsigned int width, unsigned int height,
    int destination_x, int destination_y, unsigned long plane)
{
    auto *xcb = connection(display);
    if (xcb != nullptr && gc != nullptr) {
        xcb_copy_plane(
            xcb, source, destination, gc_id(gc),
            static_cast<std::int16_t>(source_x),
            static_cast<std::int16_t>(source_y),
            static_cast<std::int16_t>(destination_x),
            static_cast<std::int16_t>(destination_y),
            static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height),
            static_cast<std::uint32_t>(plane));
    }
    return xcb != nullptr && gc != nullptr;
}

Pixmap XCreatePixmap(
    Display *display, Drawable drawable, unsigned int width,
    unsigned int height, unsigned int depth)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return None;
    const Pixmap pixmap = xcb_generate_id(xcb);
    xcb_create_pixmap(
        xcb, static_cast<std::uint8_t>(depth), pixmap, drawable,
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    return pixmap;
}

Pixmap XCreateBitmapFromData(
    Display *display, Drawable drawable, const char *data,
    unsigned int width, unsigned int height)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || data == nullptr)
        return None;
    return xcb_create_pixmap_from_bitmap_data(
        xcb, drawable,
        reinterpret_cast<std::uint8_t *>(const_cast<char *>(data)),
        width, height, 1, 1, 0, nullptr);
}

int XFreePixmap(Display *display, Pixmap pixmap)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_free_pixmap(xcb, pixmap);
    return xcb != nullptr;
}

int XClearWindow(Display *display, Window window)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_clear_area(xcb, False, window, 0, 0, 0, 0);
    return xcb != nullptr;
}

int XPutImage(
    Display *display, Drawable drawable, GC gc, XImage *image,
    int source_x, int source_y, int destination_x, int destination_y,
    unsigned int width, unsigned int height)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || gc == nullptr || image == nullptr ||
        image->data == nullptr)
        return 0;
    XImage *upload = image;
    if (source_x != 0 || source_y != 0 ||
        width != static_cast<unsigned int>(image->width) ||
        height != static_cast<unsigned int>(image->height)) {
        upload = XSubImage(image, source_x, source_y, width, height);
        if (upload == nullptr)
            return 0;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(
        static_cast<std::size_t>(upload->bytes_per_line) * height);
    xcb_put_image(
        xcb, static_cast<std::uint8_t>(upload->format), drawable, gc_id(gc),
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
        static_cast<std::int16_t>(destination_x),
        static_cast<std::int16_t>(destination_y),
        static_cast<std::uint8_t>(upload->xoffset),
        static_cast<std::uint8_t>(upload->depth), length,
        reinterpret_cast<const std::uint8_t *>(upload->data));
    if (upload != image)
        XDestroyImage(upload);
    return 1;
}

XImage *XGetSubImage(
    Display *display, Drawable drawable, int x, int y,
    unsigned int width, unsigned int height, unsigned long plane_mask,
    int format, XImage *destination, int destination_x, int destination_y)
{
    XImage *source = XGetImage(
        display, drawable, x, y, width, height, plane_mask, format);
    if (source == nullptr)
        return nullptr;
    if (destination == nullptr)
        return source;
    for (unsigned row = 0; row < height; ++row) {
        for (unsigned column = 0; column < width; ++column) {
            XPutPixel(
                destination, destination_x + static_cast<int>(column),
                destination_y + static_cast<int>(row),
                XGetPixel(source, static_cast<int>(column),
                          static_cast<int>(row)));
        }
    }
    XDestroyImage(source);
    return destination;
}

int XChangeProperty(
    Display *display, Window window, Atom property, Atom type, int format,
    int mode, const unsigned char *data, int element_count)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || element_count < 0)
        return 0;
    if (format == 32 && data != nullptr) {
        const auto *values = reinterpret_cast<const unsigned long *>(data);
        std::vector<std::uint32_t> packed(static_cast<std::size_t>(element_count));
        std::transform(values, values + element_count, packed.begin(),
                       [](unsigned long value) {
                           return static_cast<std::uint32_t>(value);
                       });
        xcb_change_property(
            xcb, static_cast<std::uint8_t>(mode), window, property, type, 32,
            static_cast<std::uint32_t>(element_count), packed.data());
    }
    else {
        xcb_change_property(
            xcb, static_cast<std::uint8_t>(mode), window, property, type,
            static_cast<std::uint8_t>(format),
            static_cast<std::uint32_t>(element_count), data);
    }
    return 1;
}

int XDeleteProperty(Display *display, Window window, Atom property)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_delete_property(xcb, window, property);
    return xcb != nullptr;
}

Atom XInternAtom(Display *display, const char *name, Bool only_if_exists)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || name == nullptr)
        return None;
    std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply(
        xcb_intern_atom_reply(
            xcb,
            xcb_intern_atom(
                xcb, only_if_exists, static_cast<std::uint16_t>(std::strlen(name)),
                name),
            nullptr),
        std::free);
    return reply == nullptr ? None : reply->atom;
}

int XGetWindowProperty(
    Display *display, Window window, Atom property, long offset, long length,
    Bool delete_property, Atom requested_type, Atom *actual_type,
    int *actual_format, unsigned long *item_count,
    unsigned long *bytes_after, unsigned char **returned_data)
{
    auto *xcb = connection(display);
    if (returned_data != nullptr)
        *returned_data = nullptr;
    if (xcb == nullptr)
        return BadRequest;
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply(
        xcb_get_property_reply(
            xcb,
            xcb_get_property(
                xcb, delete_property, window, property, requested_type,
                static_cast<std::uint32_t>(std::max(offset, 0L)),
                static_cast<std::uint32_t>(std::max(length, 0L))),
            nullptr),
        std::free);
    if (reply == nullptr)
        return BadRequest;
    if (actual_type != nullptr)
        *actual_type = reply->type;
    if (actual_format != nullptr)
        *actual_format = reply->format;
    if (item_count != nullptr)
        *item_count = reply->value_len;
    if (bytes_after != nullptr)
        *bytes_after = reply->bytes_after;
    if (returned_data == nullptr || reply->type == XCB_ATOM_NONE)
        return Success;
    const void *value = xcb_get_property_value(reply.get());
    if (reply->format == 32) {
        auto *result = static_cast<unsigned long *>(
            std::calloc(static_cast<std::size_t>(reply->value_len) + 1,
                        sizeof(unsigned long)));
        if (result == nullptr)
            return BadAlloc;
        const auto *packed = static_cast<const std::uint32_t *>(value);
        std::copy(packed, packed + reply->value_len, result);
        *returned_data = reinterpret_cast<unsigned char *>(result);
    }
    else {
        const int bytes = xcb_get_property_value_length(reply.get());
        auto *result = static_cast<unsigned char *>(
            std::calloc(static_cast<std::size_t>(std::max(bytes, 0)) + 1, 1));
        if (result == nullptr)
            return BadAlloc;
        std::memcpy(result, value, static_cast<std::size_t>(std::max(bytes, 0)));
        *returned_data = result;
    }
    return Success;
}

Status XGetGeometry(
    Display *display, Drawable drawable, Window *root, int *x, int *y,
    unsigned int *width, unsigned int *height, unsigned int *border_width,
    unsigned int *depth)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return 0;
    std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> reply(
        xcb_get_geometry_reply(xcb, xcb_get_geometry(xcb, drawable), nullptr),
        std::free);
    if (reply == nullptr)
        return 0;
    if (root != nullptr) *root = reply->root;
    if (x != nullptr) *x = reply->x;
    if (y != nullptr) *y = reply->y;
    if (width != nullptr) *width = reply->width;
    if (height != nullptr) *height = reply->height;
    if (border_width != nullptr) *border_width = reply->border_width;
    if (depth != nullptr) *depth = reply->depth;
    return 1;
}

Status XGetWindowAttributes(
    Display *display, Window window, XWindowAttributes *attributes)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || attributes == nullptr)
        return 0;
    std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)> reply(
        xcb_get_window_attributes_reply(
            xcb, xcb_get_window_attributes(xcb, window), nullptr),
        std::free);
    Window root = None;
    unsigned depth = 0;
    if (reply == nullptr ||
        !XGetGeometry(display, window, &root, &attributes->x, &attributes->y,
                      reinterpret_cast<unsigned *>(&attributes->width),
                      reinterpret_cast<unsigned *>(&attributes->height),
                      reinterpret_cast<unsigned *>(&attributes->border_width),
                      &depth))
        return 0;
    attributes->depth = static_cast<int>(depth);
    attributes->visual = DefaultVisual(display, DefaultScreen(display));
    attributes->root = root;
    attributes->c_class = reply->_class;
    attributes->bit_gravity = reply->bit_gravity;
    attributes->win_gravity = reply->win_gravity;
    attributes->backing_store = reply->backing_store;
    attributes->backing_planes = reply->backing_planes;
    attributes->backing_pixel = reply->backing_pixel;
    attributes->save_under = reply->save_under;
    attributes->colormap = reply->colormap;
    attributes->map_installed = reply->map_is_installed;
    attributes->map_state = reply->map_state;
    attributes->all_event_masks = reply->all_event_masks;
    attributes->your_event_mask = reply->your_event_mask;
    attributes->do_not_propagate_mask = reply->do_not_propagate_mask;
    attributes->override_redirect = reply->override_redirect;
    attributes->screen = DefaultScreenOfDisplay(display);
    return 1;
}

XVisualInfo *XGetVisualInfo(
    Display *display, long mask, XVisualInfo *wanted, int *count)
{
    if (display == nullptr)
        return nullptr;
    XVisualInfo candidate{};
    candidate.visual = DefaultVisual(display, DefaultScreen(display));
    candidate.visualid = XVisualIDFromVisual(candidate.visual);
    candidate.screen = DefaultScreen(display);
    candidate.depth = DefaultDepth(display, candidate.screen);
    candidate.c_class = candidate.visual->c_class;
    candidate.red_mask = candidate.visual->red_mask;
    candidate.green_mask = candidate.visual->green_mask;
    candidate.blue_mask = candidate.visual->blue_mask;
    candidate.colormap_size = candidate.visual->map_entries;
    candidate.bits_per_rgb = candidate.visual->bits_per_rgb;
    const bool matches = wanted == nullptr ||
        ((!(mask & VisualIDMask) || wanted->visualid == candidate.visualid) &&
         (!(mask & VisualScreenMask) || wanted->screen == candidate.screen) &&
         (!(mask & VisualDepthMask) || wanted->depth == candidate.depth) &&
         (!(mask & VisualClassMask) || wanted->c_class == candidate.c_class));
    if (count != nullptr)
        *count = matches ? 1 : 0;
    if (!matches)
        return nullptr;
    auto *result = static_cast<XVisualInfo *>(std::malloc(sizeof(candidate)));
    if (result != nullptr)
        *result = candidate;
    return result;
}

XPixmapFormatValues *XListPixmapFormats(Display *display, int *count)
{
    if (display == nullptr)
        return nullptr;
    const xcb_setup_t *setup = xcb_get_setup(connection(display));
    auto iterator = xcb_setup_pixmap_formats_iterator(setup);
    if (count != nullptr)
        *count = iterator.rem;
    auto *result = static_cast<XPixmapFormatValues *>(
        std::calloc(static_cast<std::size_t>(iterator.rem),
                    sizeof(XPixmapFormatValues)));
    for (int index = 0; result != nullptr && iterator.rem != 0;
         ++index, xcb_format_next(&iterator)) {
        result[index].depth = iterator.data->depth;
        result[index].bits_per_pixel = iterator.data->bits_per_pixel;
        result[index].scanline_pad = iterator.data->scanline_pad;
    }
    return result;
}

int XMoveWindow(Display *display, Window window, int x, int y)
{
    return configure_window(display, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                            {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)});
}

int XResizeWindow(
    Display *display, Window window, unsigned int width, unsigned int height)
{
    return configure_window(
        display, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        {width, height});
}

int XMoveResizeWindow(
    Display *display, Window window, int x, int y,
    unsigned int width, unsigned int height)
{
    return configure_window(
        display, window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
         width, height});
}

int XConfigureWindow(
    Display *display, Window window, unsigned int value_mask,
    XWindowChanges *changes)
{
    if (changes == nullptr)
        return 0;
    std::vector<std::uint32_t> values;
    if (value_mask & CWX) values.push_back(static_cast<std::uint32_t>(changes->x));
    if (value_mask & CWY) values.push_back(static_cast<std::uint32_t>(changes->y));
    if (value_mask & CWWidth) values.push_back(static_cast<std::uint32_t>(changes->width));
    if (value_mask & CWHeight) values.push_back(static_cast<std::uint32_t>(changes->height));
    if (value_mask & CWBorderWidth) values.push_back(static_cast<std::uint32_t>(changes->border_width));
    if (value_mask & CWSibling) values.push_back(static_cast<std::uint32_t>(changes->sibling));
    if (value_mask & CWStackMode) values.push_back(static_cast<std::uint32_t>(changes->stack_mode));
    return configure_window(
        display, window, static_cast<std::uint16_t>(value_mask), values);
}

Status XReconfigureWMWindow(
    Display *display, Window window, int, unsigned int value_mask,
    XWindowChanges *changes)
{
    return XConfigureWindow(display, window, value_mask, changes);
}

int XRaiseWindow(Display *display, Window window)
{
    return configure_window(
        display, window, XCB_CONFIG_WINDOW_STACK_MODE, {XCB_STACK_MODE_ABOVE});
}

int XReparentWindow(
    Display *display, Window window, Window parent, int x, int y)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_reparent_window(
            xcb, window, parent, static_cast<std::int16_t>(x),
            static_cast<std::int16_t>(y));
    return xcb != nullptr;
}

int XWithdrawWindow(Display *display, Window window, int)
{
    return XUnmapWindow(display, window);
}

int XChangeWindowAttributes(
    Display *display, Window window, unsigned long value_mask,
    XSetWindowAttributes *attributes)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || attributes == nullptr)
        return 0;
    std::vector<std::uint32_t> values;
    if (value_mask & CWBackPixmap) values.push_back(attributes->background_pixmap);
    if (value_mask & CWBackPixel) values.push_back(attributes->background_pixel);
    if (value_mask & CWBorderPixmap) values.push_back(attributes->border_pixmap);
    if (value_mask & CWBorderPixel) values.push_back(attributes->border_pixel);
    if (value_mask & CWBitGravity) values.push_back(attributes->bit_gravity);
    if (value_mask & CWWinGravity) values.push_back(attributes->win_gravity);
    if (value_mask & CWBackingStore) values.push_back(attributes->backing_store);
    if (value_mask & CWBackingPlanes) values.push_back(attributes->backing_planes);
    if (value_mask & CWBackingPixel) values.push_back(attributes->backing_pixel);
    if (value_mask & CWOverrideRedirect) values.push_back(attributes->override_redirect);
    if (value_mask & CWSaveUnder) values.push_back(attributes->save_under);
    if (value_mask & CWEventMask) values.push_back(attributes->event_mask);
    if (value_mask & CWDontPropagate) values.push_back(attributes->do_not_propagate_mask);
    if (value_mask & CWColormap) values.push_back(attributes->colormap);
    if (value_mask & CWCursor) values.push_back(attributes->cursor);
    xcb_change_window_attributes(
        xcb, window, static_cast<std::uint32_t>(value_mask), values.data());
    return 1;
}

int XSetWindowBackground(Display *display, Window window, unsigned long pixel)
{
    XSetWindowAttributes attributes{};
    attributes.background_pixel = pixel;
    return XChangeWindowAttributes(display, window, CWBackPixel, &attributes);
}

int XSetWindowBackgroundPixmap(Display *display, Window window, Pixmap pixmap)
{
    XSetWindowAttributes attributes{};
    attributes.background_pixmap = pixmap;
    return XChangeWindowAttributes(display, window, CWBackPixmap, &attributes);
}

int XSetWindowBorder(Display *display, Window window, unsigned long pixel)
{
    XSetWindowAttributes attributes{};
    attributes.border_pixel = pixel;
    return XChangeWindowAttributes(display, window, CWBorderPixel, &attributes);
}

int XSetWindowBorderPixmap(Display *display, Window window, Pixmap pixmap)
{
    XSetWindowAttributes attributes{};
    attributes.border_pixmap = pixmap;
    return XChangeWindowAttributes(display, window, CWBorderPixmap, &attributes);
}

int XSetWindowBorderWidth(
    Display *display, Window window, unsigned int width)
{
    return configure_window(
        display, window, XCB_CONFIG_WINDOW_BORDER_WIDTH, {width});
}

int XSetWindowColormap(Display *display, Window window, Colormap colormap)
{
    XSetWindowAttributes attributes{};
    attributes.colormap = colormap;
    return XChangeWindowAttributes(display, window, CWColormap, &attributes);
}

Colormap XCreateColormap(
    Display *display, Window window, Visual *visual, int alloc)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || visual == nullptr)
        return None;
    const Colormap colormap = xcb_generate_id(xcb);
    xcb_create_colormap(
        xcb, static_cast<std::uint8_t>(alloc), colormap, window,
        visual->visualid);
    return colormap;
}

int XFreeColormap(Display *display, Colormap colormap)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_free_colormap(xcb, colormap);
    return xcb != nullptr;
}

Status XAllocColor(Display *display, Colormap, XColor *color)
{
    if (display == nullptr || color == nullptr)
        return 0;
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    color->pixel = scale_component(color->red, visual->red_mask) |
                   scale_component(color->green, visual->green_mask) |
                   scale_component(color->blue, visual->blue_mask);
    color->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

int XFreeColors(Display *, Colormap, unsigned long *, int, unsigned long)
{
    return 1;
}

Status XQueryColors(Display *display, Colormap, XColor *colors, int count)
{
    if (display == nullptr || colors == nullptr || count < 0)
        return 0;
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    for (int index = 0; index < count; ++index) {
        colors[index].red = unscale_component(colors[index].pixel, visual->red_mask);
        colors[index].green = unscale_component(colors[index].pixel, visual->green_mask);
        colors[index].blue = unscale_component(colors[index].pixel, visual->blue_mask);
        colors[index].flags = DoRed | DoGreen | DoBlue;
    }
    return 1;
}

Status XParseColor(Display *, Colormap, const char *specification, XColor *color)
{
    if (specification == nullptr || color == nullptr)
        return 0;
    const std::string text = lower(specification);
    if (!text.empty() && text.front() == '#') {
        const std::string digits = text.substr(1);
        if (digits.size() % 3 != 0 || digits.empty() || digits.size() > 12)
            return 0;
        const std::size_t component_digits = digits.size() / 3;
        const unsigned long maximum =
            (1UL << static_cast<unsigned int>(component_digits * 4)) - 1UL;
        unsigned short components[3]{};
        for (std::size_t component = 0; component < 3; ++component) {
            const std::string part = digits.substr(
                component * component_digits, component_digits);
            char *end = nullptr;
            const unsigned long value = std::strtoul(part.c_str(), &end, 16);
            if (end != part.c_str() + part.size())
                return 0;
            components[component] = static_cast<unsigned short>(
                (value * 65535UL + maximum / 2UL) / maximum);
        }
        color->red = components[0];
        color->green = components[1];
        color->blue = components[2];
        color->flags = DoRed | DoGreen | DoBlue;
        return 1;
    }
    struct Named { const char *name; unsigned short r, g, b; };
    static constexpr Named named[]{{"black", 0, 0, 0},
        {"white", 65535, 65535, 65535}, {"red", 65535, 0, 0},
        {"green", 0, 65535, 0}, {"blue", 0, 0, 65535},
        {"yellow", 65535, 65535, 0}, {"gray", 32896, 32896, 32896},
        {"grey", 32896, 32896, 32896}};
    for (const auto &entry : named) {
        if (text == entry.name) {
            color->red = entry.r; color->green = entry.g; color->blue = entry.b;
            color->flags = DoRed | DoGreen | DoBlue;
            return 1;
        }
    }
    return 0;
}

Status XLookupColor(
    Display *display, Colormap colormap, const char *name,
    XColor *exact, XColor *screen)
{
    XColor parsed{};
    if (!XParseColor(display, colormap, name, &parsed))
        return 0;
    if (exact != nullptr)
        *exact = parsed;
    if (screen != nullptr) {
        *screen = parsed;
        XAllocColor(display, colormap, screen);
    }
    return 1;
}

Status XAllocNamedColor(
    Display *display, Colormap colormap, const char *name,
    XColor *screen, XColor *exact)
{
    return XLookupColor(display, colormap, name, exact, screen);
}

Status XQueryColor(Display *display, Colormap colormap, XColor *color)
{
    return XQueryColors(display, colormap, color, 1);
}

Cursor XCreatePixmapCursor(
    Display *display, Pixmap source, Pixmap mask, XColor *foreground,
    XColor *background, unsigned int x, unsigned int y)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || foreground == nullptr || background == nullptr)
        return None;
    const Cursor cursor = xcb_generate_id(xcb);
    xcb_create_cursor(
        xcb, cursor, source, mask, foreground->red, foreground->green,
        foreground->blue, background->red, background->green,
        background->blue, static_cast<std::uint16_t>(x),
        static_cast<std::uint16_t>(y));
    return cursor;
}

Cursor XCreateFontCursor(Display *display, unsigned int shape)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return None;
    const Font font = xcb_generate_id(xcb);
    static constexpr char name[] = "cursor";
    xcb_open_font(xcb, font, sizeof(name) - 1, name);
    const Cursor cursor = xcb_generate_id(xcb);
    xcb_create_glyph_cursor(
        xcb, cursor, font, font, static_cast<std::uint16_t>(shape),
        static_cast<std::uint16_t>(shape + 1), 0, 0, 0, 65535, 65535, 65535);
    xcb_close_font(xcb, font);
    return cursor;
}

Cursor XCreateGlyphCursor(
    Display *display, Font source_font, Font mask_font,
    unsigned int source_character, unsigned int mask_character,
    const XColor *foreground, const XColor *background)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || foreground == nullptr || background == nullptr)
        return None;
    const Cursor cursor = xcb_generate_id(xcb);
    xcb_create_glyph_cursor(
        xcb, cursor, source_font, mask_font,
        static_cast<std::uint16_t>(source_character),
        static_cast<std::uint16_t>(mask_character), foreground->red,
        foreground->green, foreground->blue, background->red,
        background->green, background->blue);
    return cursor;
}

int XFreeCursor(Display *display, Cursor cursor)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_free_cursor(xcb, cursor);
    return xcb != nullptr;
}

int XDefineCursor(Display *display, Window window, Cursor cursor)
{
    auto *xcb = connection(display);
    const std::uint32_t value = cursor;
    if (xcb != nullptr)
        xcb_change_window_attributes(xcb, window, XCB_CW_CURSOR, &value);
    return xcb != nullptr;
}

int XRecolorCursor(Display *, Cursor, XColor *, XColor *)
{
    return 1;
}

Font XLoadFont(Display *display, const char *name)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || name == nullptr)
        return None;
    const Font font = xcb_generate_id(xcb);
    xcb_open_font(
        xcb, font, static_cast<std::uint16_t>(std::strlen(name)), name);
    return font;
}

XFontStruct *XLoadQueryFont(Display *display, const char *name)
{
    const Font font = XLoadFont(display, name == nullptr ? "fixed" : name);
    if (font == None)
        return nullptr;
    auto *result = static_cast<XFontStruct *>(std::calloc(1, sizeof(XFontStruct)));
    if (result == nullptr)
        return nullptr;
    result->fid = font;
    result->direction = FontLeftToRight;
    result->min_char_or_byte2 = 0;
    result->max_char_or_byte2 = 255;
    result->ascent = 11;
    result->descent = 3;
    result->min_bounds.width = 8;
    result->max_bounds.width = 8;
    return result;
}

int XFreeFont(Display *display, XFontStruct *font)
{
    auto *xcb = connection(display);
    if (xcb != nullptr && font != nullptr)
        xcb_close_font(xcb, font->fid);
    std::free(font);
    return xcb != nullptr;
}

int XUnloadFont(Display *display, Font font)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_close_font(xcb, font);
    return xcb != nullptr;
}

Window XRootWindow(Display *display, int screen)
{
    return display == nullptr ? None : RootWindow(display, screen);
}

Status XQueryPointer(
    Display *display, Window window, Window *root, Window *child,
    int *root_x, int *root_y, int *window_x, int *window_y,
    unsigned int *mask)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return 0;
    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&std::free)> reply(
        xcb_query_pointer_reply(xcb, xcb_query_pointer(xcb, window), nullptr),
        std::free);
    if (reply == nullptr)
        return 0;
    if (root) *root = reply->root;
    if (child) *child = reply->child;
    if (root_x) *root_x = reply->root_x;
    if (root_y) *root_y = reply->root_y;
    if (window_x) *window_x = reply->win_x;
    if (window_y) *window_y = reply->win_y;
    if (mask) *mask = reply->mask;
    return reply->same_screen;
}

Status XQueryTree(
    Display *display, Window window, Window *root, Window *parent,
    Window **children, unsigned int *count)
{
    auto *xcb = connection(display);
    if (children) *children = nullptr;
    if (xcb == nullptr)
        return 0;
    std::unique_ptr<xcb_query_tree_reply_t, decltype(&std::free)> reply(
        xcb_query_tree_reply(xcb, xcb_query_tree(xcb, window), nullptr),
        std::free);
    if (reply == nullptr)
        return 0;
    if (root) *root = reply->root;
    if (parent) *parent = reply->parent;
    const int length = xcb_query_tree_children_length(reply.get());
    if (count) *count = static_cast<unsigned int>(length);
    if (children && length > 0) {
        *children = static_cast<Window *>(
            std::malloc(static_cast<std::size_t>(length) * sizeof(Window)));
        const auto *source = xcb_query_tree_children(reply.get());
        if (*children != nullptr)
            std::copy(source, source + length, *children);
    }
    return 1;
}

Bool XTranslateCoordinates(
    Display *display, Window source, Window destination, int source_x,
    int source_y, int *destination_x, int *destination_y, Window *child)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return False;
    std::unique_ptr<xcb_translate_coordinates_reply_t, decltype(&std::free)> reply(
        xcb_translate_coordinates_reply(
            xcb,
            xcb_translate_coordinates(
                xcb, source, destination, static_cast<std::int16_t>(source_x),
                static_cast<std::int16_t>(source_y)),
            nullptr),
        std::free);
    if (reply == nullptr)
        return False;
    if (destination_x) *destination_x = reply->dst_x;
    if (destination_y) *destination_y = reply->dst_y;
    if (child) *child = reply->child;
    return reply->same_screen;
}

int XWarpPointer(
    Display *display, Window source, Window destination,
    int source_x, int source_y, unsigned int source_width,
    unsigned int source_height, int destination_x, int destination_y)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) {
        xcb_warp_pointer(
            xcb, source, destination, static_cast<std::int16_t>(source_x),
            static_cast<std::int16_t>(source_y),
            static_cast<std::uint16_t>(source_width),
            static_cast<std::uint16_t>(source_height),
            static_cast<std::int16_t>(destination_x),
            static_cast<std::int16_t>(destination_y));
    }
    return xcb != nullptr;
}

int XConvertSelection(
    Display *display, Atom selection, Atom target, Atom property,
    Window requestor, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_convert_selection(xcb, requestor, selection, target, property, time);
    return xcb != nullptr;
}

int XSetSelectionOwner(
    Display *display, Atom selection, Window owner, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_set_selection_owner(xcb, owner, selection, time);
    return xcb != nullptr;
}

Window XGetSelectionOwner(Display *display, Atom selection)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return None;
    std::unique_ptr<xcb_get_selection_owner_reply_t, decltype(&std::free)> reply(
        xcb_get_selection_owner_reply(
            xcb, xcb_get_selection_owner(xcb, selection), nullptr),
        std::free);
    return reply == nullptr ? None : reply->owner;
}

char *XGetAtomName(Display *display, Atom atom)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return nullptr;
    std::unique_ptr<xcb_get_atom_name_reply_t, decltype(&std::free)> reply(
        xcb_get_atom_name_reply(xcb, xcb_get_atom_name(xcb, atom), nullptr),
        std::free);
    if (reply == nullptr)
        return nullptr;
    const int length = xcb_get_atom_name_name_length(reply.get());
    auto *result = static_cast<char *>(
        std::calloc(static_cast<std::size_t>(length) + 1, 1));
    if (result != nullptr)
        std::memcpy(result, xcb_get_atom_name_name(reply.get()), length);
    return result;
}

int XSetInputFocus(
    Display *display, Window focus, int revert_to, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_set_input_focus(
            xcb, static_cast<std::uint8_t>(revert_to), focus, time);
    return xcb != nullptr;
}

int XGetInputFocus(Display *display, Window *focus, int *revert_to)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return 0;
    std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> reply(
        xcb_get_input_focus_reply(xcb, xcb_get_input_focus(xcb), nullptr),
        std::free);
    if (reply == nullptr)
        return 0;
    if (focus != nullptr) *focus = reply->focus;
    if (revert_to != nullptr) *revert_to = reply->revert_to;
    return 1;
}

int XGrabServer(Display *display)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) xcb_grab_server(xcb);
    return xcb != nullptr;
}

int XUngrabServer(Display *display)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) xcb_ungrab_server(xcb);
    return xcb != nullptr;
}

int XNoOp(Display *display)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) xcb_no_operation(xcb);
    return xcb != nullptr;
}

int XForceScreenSaver(Display *, int) { return 1; }
int XResetScreenSaver(Display *) { return 1; }

int XIconifyWindow(Display *display, Window window, int)
{
    return XUnmapWindow(display, window);
}

int XSetTransientForHint(Display *display, Window window, Window parent)
{
    const unsigned long value = parent;
    return XChangeProperty(
        display, window, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32,
        PropModeReplace, reinterpret_cast<const unsigned char *>(&value), 1);
}

int XStoreName(Display *display, Window window, const char *name)
{
    const auto *value = reinterpret_cast<const unsigned char *>(
        name == nullptr ? "" : name);
    return XChangeProperty(
        display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, value,
        static_cast<int>(std::strlen(reinterpret_cast<const char *>(value))));
}

int XSetIconName(Display *display, Window window, const char *name)
{
    const auto *value = reinterpret_cast<const unsigned char *>(
        name == nullptr ? "" : name);
    return XChangeProperty(
        display, window, XA_WM_ICON_NAME, XA_STRING, 8, PropModeReplace,
        value, static_cast<int>(std::strlen(reinterpret_cast<const char *>(value))));
}

int XSetClassHint(Display *display, Window window, XClassHint *hint)
{
    if (hint == nullptr)
        return 0;
    const std::string name = hint->res_name == nullptr ? "" : hint->res_name;
    const std::string class_name = hint->res_class == nullptr ? "" : hint->res_class;
    std::vector<unsigned char> value(name.begin(), name.end());
    value.push_back(0);
    value.insert(value.end(), class_name.begin(), class_name.end());
    value.push_back(0);
    return XChangeProperty(
        display, window, XA_WM_CLASS, XA_STRING, 8, PropModeReplace,
        value.data(), static_cast<int>(value.size()));
}

Status XStringListToTextProperty(
    char **list, int count, XTextProperty *property)
{
    if (property == nullptr || count < 0)
        return 0;
    std::vector<unsigned char> value;
    for (int index = 0; index < count; ++index) {
        const char *text = list == nullptr || list[index] == nullptr ? "" : list[index];
        value.insert(value.end(), text, text + std::strlen(text));
        if (index + 1 < count)
            value.push_back(0);
    }
    property->value = static_cast<unsigned char *>(
        std::malloc(std::max<std::size_t>(value.size(), 1)));
    if (property->value == nullptr)
        return 0;
    if (!value.empty())
        std::copy(value.begin(), value.end(), property->value);
    property->encoding = XA_STRING;
    property->format = 8;
    property->nitems = value.size();
    return 1;
}

void XSetWMClientMachine(
    Display *display, Window window, XTextProperty *property)
{
    if (property != nullptr)
        XChangeProperty(
            display, window, XA_WM_CLIENT_MACHINE, property->encoding,
            property->format, PropModeReplace, property->value,
            static_cast<int>(property->nitems));
}

int XSetCommand(Display *display, Window window, char **arguments, int count)
{
    XTextProperty property{};
    if (!XStringListToTextProperty(arguments, count, &property))
        return 0;
    const int result = XChangeProperty(
        display, window, XA_WM_COMMAND, XA_STRING, 8, PropModeReplace,
        property.value, static_cast<int>(property.nitems));
    XFree(property.value);
    return result;
}

Status XSetWMColormapWindows(
    Display *display, Window window, Window *windows, int count)
{
    const Atom property = XInternAtom(
        display, "WM_COLORMAP_WINDOWS", False);
    return XChangeProperty(
        display, window, property, XA_WINDOW, 32,
        PropModeReplace, reinterpret_cast<const unsigned char *>(windows), count);
}

Status XGetWMColormapWindows(
    Display *display, Window window, Window **windows, int *count)
{
    const Atom property = XInternAtom(
        display, "WM_COLORMAP_WINDOWS", False);
    Atom type = None;
    int format = 0;
    unsigned long items = 0;
    unsigned long after = 0;
    unsigned char *data = nullptr;
    const int result = XGetWindowProperty(
        display, window, property, 0,
        std::numeric_limits<long>::max(), False, XA_WINDOW, &type, &format,
        &items, &after, &data);
    if (windows != nullptr) *windows = reinterpret_cast<Window *>(data);
    else XFree(data);
    if (count != nullptr) *count = static_cast<int>(items);
    return result == Success && type == XA_WINDOW && format == 32;
}

XClassHint *XAllocClassHint(void)
{
    return static_cast<XClassHint *>(std::calloc(1, sizeof(XClassHint)));
}

XHostAddress *XListHosts(
    Display *, int *count, Bool *enabled)
{
    if (count != nullptr) *count = 0;
    if (enabled != nullptr) *enabled = False;
    return nullptr;
}

int XDisplayKeycodes(Display *display, int *minimum, int *maximum)
{
    const auto storage = reinterpret_cast<_XPrivDisplay>(display);
    if (storage == nullptr)
        return 0;
    if (minimum != nullptr) *minimum = storage->min_keycode;
    if (maximum != nullptr) *maximum = storage->max_keycode;
    return 1;
}

XModifierKeymap *XGetModifierMapping(Display *display)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return nullptr;
    std::unique_ptr<xcb_get_modifier_mapping_reply_t, decltype(&std::free)> reply(
        xcb_get_modifier_mapping_reply(
            xcb, xcb_get_modifier_mapping(xcb), nullptr), std::free);
    if (reply == nullptr)
        return nullptr;
    auto *result = static_cast<XModifierKeymap *>(
        std::calloc(1, sizeof(XModifierKeymap)));
    if (result == nullptr)
        return nullptr;
    result->max_keypermod = reply->keycodes_per_modifier;
    const int length = xcb_get_modifier_mapping_keycodes_length(reply.get());
    result->modifiermap = static_cast<KeyCode *>(
        std::malloc(static_cast<std::size_t>(length) * sizeof(KeyCode)));
    if (result->modifiermap != nullptr)
        std::copy(xcb_get_modifier_mapping_keycodes(reply.get()),
                  xcb_get_modifier_mapping_keycodes(reply.get()) + length,
                  result->modifiermap);
    return result;
}

int XFreeModifiermap(XModifierKeymap *map)
{
    if (map != nullptr) {
        std::free(map->modifiermap);
        std::free(map);
    }
    return 1;
}

static int xmin_synchronize(Display *display)
{
    return XSync(display, False);
}

int (*XSynchronize(Display *display, Bool enabled))(Display *)
{
    const auto storage = reinterpret_cast<_XPrivDisplay>(display);
    if (storage == nullptr)
        return nullptr;
    const auto previous = storage->private15;
    storage->private15 = enabled ? xmin_synchronize : nullptr;
    return previous;
}

int XSetWMHints(Display *display, Window window, XWMHints *hints)
{
    if (hints == nullptr)
        return 0;
    const std::array<unsigned long, 9> values{{
        static_cast<unsigned long>(hints->flags),
        static_cast<unsigned long>(hints->input),
        static_cast<unsigned long>(hints->initial_state), hints->icon_pixmap,
        hints->icon_window, static_cast<unsigned long>(hints->icon_x),
        static_cast<unsigned long>(hints->icon_y), hints->icon_mask,
        hints->window_group}};
    return XChangeProperty(
        display, window, XA_WM_HINTS, XA_WM_HINTS, 32, PropModeReplace,
        reinterpret_cast<const unsigned char *>(values.data()), values.size());
}

void XSetWMNormalHints(Display *display, Window window, XSizeHints *hints)
{
    if (hints == nullptr)
        return;
    const std::array<unsigned long, 18> values{{
        static_cast<unsigned long>(hints->flags),
        static_cast<unsigned long>(hints->x), static_cast<unsigned long>(hints->y),
        static_cast<unsigned long>(hints->width), static_cast<unsigned long>(hints->height),
        static_cast<unsigned long>(hints->min_width), static_cast<unsigned long>(hints->min_height),
        static_cast<unsigned long>(hints->max_width), static_cast<unsigned long>(hints->max_height),
        static_cast<unsigned long>(hints->width_inc), static_cast<unsigned long>(hints->height_inc),
        static_cast<unsigned long>(hints->min_aspect.x), static_cast<unsigned long>(hints->min_aspect.y),
        static_cast<unsigned long>(hints->max_aspect.x), static_cast<unsigned long>(hints->max_aspect.y),
        static_cast<unsigned long>(hints->base_width), static_cast<unsigned long>(hints->base_height),
        static_cast<unsigned long>(hints->win_gravity)}};
    XChangeProperty(
        display, window, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32,
        PropModeReplace, reinterpret_cast<const unsigned char *>(values.data()),
        values.size());
}

void XSetWMProperties(
    Display *display, Window window, XTextProperty *window_name,
    XTextProperty *icon_name, char **, int, XSizeHints *normal_hints,
    XWMHints *wm_hints, XClassHint *class_hints)
{
    if (window_name != nullptr)
        XChangeProperty(display, window, XA_WM_NAME, window_name->encoding,
                        window_name->format, PropModeReplace,
                        window_name->value, window_name->nitems);
    if (icon_name != nullptr)
        XChangeProperty(display, window, XA_WM_ICON_NAME, icon_name->encoding,
                        icon_name->format, PropModeReplace,
                        icon_name->value, icon_name->nitems);
    if (normal_hints != nullptr)
        XSetWMNormalHints(display, window, normal_hints);
    if (wm_hints != nullptr)
        XSetWMHints(display, window, wm_hints);
    if (class_hints != nullptr) {
        const std::string resource_name = class_hints->res_name == nullptr
            ? std::string() : class_hints->res_name;
        const std::string resource_class = class_hints->res_class == nullptr
            ? std::string() : class_hints->res_class;
        std::vector<unsigned char> data(resource_name.begin(), resource_name.end());
        data.push_back(0);
        data.insert(data.end(), resource_class.begin(), resource_class.end());
        data.push_back(0);
        XChangeProperty(display, window, XA_WM_CLASS, XA_STRING, 8,
                        PropModeReplace, data.data(), data.size());
    }
}

char *XGetDefault(Display *, const char *, const char *)
{
    return nullptr;
}

int XGetErrorText(Display *, int code, char *buffer, int length)
{
    if (buffer == nullptr || length <= 0)
        return 0;
    const std::string message = "X protocol error " + std::to_string(code);
    std::strncpy(buffer, message.c_str(), static_cast<std::size_t>(length - 1));
    buffer[length - 1] = '\0';
    return 0;
}

int XGetErrorDatabaseText(
    Display *, const char *, const char *, const char *default_string,
    char *buffer, int length)
{
    if (buffer == nullptr || length <= 0)
        return 0;
    std::strncpy(buffer, default_string == nullptr ? "" : default_string,
                 static_cast<std::size_t>(length - 1));
    buffer[length - 1] = '\0';
    return 0;
}

XErrorHandler XSetErrorHandler(XErrorHandler handler)
{
    return error_handler.exchange(
        handler == nullptr ? default_error_handler : handler);
}

XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler handler)
{
    return io_error_handler.exchange(
        handler == nullptr ? default_io_error_handler : handler);
}

void XFreeStringList(char **list)
{
    if (list != nullptr) {
        for (char **entry = list; *entry != nullptr; ++entry)
            std::free(*entry);
        std::free(list);
    }
}

int XParseGeometry(
    const char *specification, int *x, int *y,
    unsigned int *width, unsigned int *height)
{
    if (specification == nullptr)
        return NoValue;
    const char *position = specification;
    if (*position == '=') ++position;
    int result = NoValue;
    char *end = nullptr;
    if (std::isdigit(static_cast<unsigned char>(*position))) {
        const unsigned long parsed = std::strtoul(position, &end, 10);
        if (*end == 'x' || *end == 'X') {
            if (width) *width = static_cast<unsigned int>(parsed);
            result |= WidthValue;
            position = end + 1;
            const unsigned long parsed_height = std::strtoul(position, &end, 10);
            if (end != position) {
                if (height) *height = static_cast<unsigned int>(parsed_height);
                result |= HeightValue;
                position = end;
            }
        }
    }
    if (*position == '+' || *position == '-') {
        const bool negative = *position++ == '-';
        const long parsed = std::strtol(position, &end, 10);
        if (end != position) {
            if (x) *x = static_cast<int>(negative ? -parsed : parsed);
            result |= XValue | (negative ? XNegative : 0);
            position = end;
        }
    }
    if (*position == '+' || *position == '-') {
        const bool negative = *position++ == '-';
        const long parsed = std::strtol(position, &end, 10);
        if (end != position) {
            if (y) *y = static_cast<int>(negative ? -parsed : parsed);
            result |= YValue | (negative ? YNegative : 0);
        }
    }
    return result;
}

} // extern "C"
