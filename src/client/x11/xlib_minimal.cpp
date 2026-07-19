/*
 * A focused Xlib-shaped facade over Xmin's C++17 XCB connection core.
 * This file owns Display/Screen/Visual client-side bookkeeping; it does not
 * incorporate libX11 implementation source.
 */
#include "xlib_internal.hpp"

#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlibint.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

struct DisplayState {
    xcb_connection_t *connection = nullptr;
    Visual visual{};
    Depth depth{};
    Screen screen{};
    std::recursive_mutex mutex;
    XEventQueueOwner queue_owner = XlibOwnsEventQueue;
    std::array<XminWireToEventProc, 256> wire_to_event{};
};

_XPrivDisplay private_display(Display *display) noexcept
{
    return reinterpret_cast<_XPrivDisplay>(display);
}

_XPrivDisplay private_display(const Display *display) noexcept
{
    return reinterpret_cast<_XPrivDisplay>(const_cast<Display *>(display));
}

DisplayState *state(Display *display) noexcept
{
    return display == nullptr
        ? nullptr
        : reinterpret_cast<DisplayState *>(
              private_display(display)->private11);
}

const DisplayState *state(const Display *display) noexcept
{
    return display == nullptr
        ? nullptr
        : reinterpret_cast<const DisplayState *>(
              private_display(display)->private11);
}

XID allocate_resource(Display *display)
{
    DisplayState *display_state = state(display);
    return display_state == nullptr
        ? 0
        : static_cast<XID>(xcb_generate_id(display_state->connection));
}

const xcb_visualtype_t *root_visual_type(
    const xcb_screen_t *screen) noexcept
{
    xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(screen);
    while (depths.rem != 0) {
        xcb_visualtype_iterator_t visuals =
            xcb_depth_visuals_iterator(depths.data);
        while (visuals.rem != 0) {
            if (visuals.data->visual_id == screen->root_visual) {
                return visuals.data;
            }
            xcb_visualtype_next(&visuals);
        }
        xcb_depth_next(&depths);
    }
    return nullptr;
}

void append_window_value(
    std::vector<std::uint32_t> &values, unsigned long mask,
    unsigned long selected, unsigned long value)
{
    if ((mask & selected) != 0) {
        values.push_back(static_cast<std::uint32_t>(value));
    }
}

unsigned long image_get_pixel(XImage *image, int x, int y)
{
    if (image == nullptr || image->data == nullptr || x < 0 || y < 0 ||
        x >= image->width || y >= image->height) {
        return 0;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(image->data);
    const auto *row = bytes + static_cast<std::size_t>(y) *
        image->bytes_per_line;
    if (image->bits_per_pixel == 32) {
        std::uint32_t value = 0;
        std::memcpy(&value, row + static_cast<std::size_t>(x) * 4U, 4);
        return value;
    }
    if (image->bits_per_pixel == 16) {
        std::uint16_t value = 0;
        std::memcpy(&value, row + static_cast<std::size_t>(x) * 2U, 2);
        return value;
    }
    if (image->bits_per_pixel == 8) {
        return row[x];
    }
    const auto bit_at = [image](const std::uint8_t *scanline, int column) {
        const unsigned int bit_index =
            static_cast<unsigned int>(column + image->xoffset);
        const std::uint8_t bit = static_cast<std::uint8_t>(
            image->bitmap_bit_order == LSBFirst
                ? 1U << (bit_index & 7U)
                : 0x80U >> (bit_index & 7U));
        return (scanline[bit_index / 8U] & bit) != 0;
    };
    if (image->format != XYPixmap || image->depth <= 1)
        return bit_at(row, x);
    const std::size_t plane_size =
        static_cast<std::size_t>(image->bytes_per_line) * image->height;
    unsigned long pixel = 0;
    for (int plane = 0; plane < image->depth; ++plane) {
        const auto *plane_row = bytes + static_cast<std::size_t>(plane) *
            plane_size + static_cast<std::size_t>(y) * image->bytes_per_line;
        if (bit_at(plane_row, x))
            pixel |= 1UL << (image->depth - plane - 1);
    }
    return pixel;
}

int image_put_pixel(XImage *image, int x, int y, unsigned long pixel)
{
    if (image == nullptr || image->data == nullptr || x < 0 || y < 0 ||
        x >= image->width || y >= image->height) {
        return 0;
    }
    auto *bytes = reinterpret_cast<std::uint8_t *>(image->data);
    auto *row = bytes +
        static_cast<std::size_t>(y) * image->bytes_per_line;
    if (image->bits_per_pixel == 32) {
        const std::uint32_t value = static_cast<std::uint32_t>(pixel);
        std::memcpy(row + static_cast<std::size_t>(x) * 4U, &value, 4);
    }
    else if (image->bits_per_pixel == 16) {
        const std::uint16_t value = static_cast<std::uint16_t>(pixel);
        std::memcpy(row + static_cast<std::size_t>(x) * 2U, &value, 2);
    }
    else if (image->bits_per_pixel == 8) {
        row[x] = static_cast<std::uint8_t>(pixel);
    }
    else if (image->format != XYPixmap || image->depth <= 1) {
        const unsigned int bit_index =
            static_cast<unsigned int>(x + image->xoffset);
        auto &byte = row[bit_index / 8U];
        const std::uint8_t bit = static_cast<std::uint8_t>(
            image->bitmap_bit_order == LSBFirst
                ? 1U << (bit_index & 7U)
                : 0x80U >> (bit_index & 7U));
        if (pixel != 0) {
            byte |= bit;
        }
        else {
            byte &= static_cast<std::uint8_t>(~bit);
        }
    }
    else {
        const std::size_t plane_size =
            static_cast<std::size_t>(image->bytes_per_line) * image->height;
        const unsigned int bit_index =
            static_cast<unsigned int>(x + image->xoffset);
        const std::uint8_t bit = static_cast<std::uint8_t>(
            image->bitmap_bit_order == LSBFirst
                ? 1U << (bit_index & 7U)
                : 0x80U >> (bit_index & 7U));
        for (int plane = 0; plane < image->depth; ++plane) {
            auto &byte = bytes[static_cast<std::size_t>(plane) * plane_size +
                static_cast<std::size_t>(y) * image->bytes_per_line +
                bit_index / 8U];
            if ((pixel & (1UL << (image->depth - plane - 1))) != 0)
                byte |= bit;
            else
                byte &= static_cast<std::uint8_t>(~bit);
        }
    }
    return 1;
}

int image_destroy(XImage *image)
{
    if (image != nullptr) {
        std::free(image->data);
        std::free(image);
    }
    return 1;
}

XImage *image_sub(
    XImage *source, int x, int y, unsigned int width, unsigned int height)
{
    if (source == nullptr) {
        return nullptr;
    }
    XImage *result = XCreateImage(
        nullptr, nullptr, source->depth, source->format, 0, nullptr, width,
        height, source->bitmap_pad, 0);
    if (result == nullptr) {
        return nullptr;
    }
    const std::size_t planes =
        result->format == XYPixmap ? result->depth : 1;
    result->data = static_cast<char *>(std::calloc(
        static_cast<std::size_t>(result->bytes_per_line) * height, planes));
    if (result->data == nullptr) {
        image_destroy(result);
        return nullptr;
    }
    for (unsigned int row = 0; row < height; ++row) {
        for (unsigned int column = 0; column < width; ++column) {
            image_put_pixel(
                result, static_cast<int>(column), static_cast<int>(row),
                image_get_pixel(
                    source, x + static_cast<int>(column),
                    y + static_cast<int>(row)));
        }
    }
    return result;
}

int image_add_pixel(XImage *image, long value)
{
    if (image == nullptr) {
        return 0;
    }
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            image_put_pixel(image, x, y, image_get_pixel(image, x, y) + value);
        }
    }
    return 1;
}

} // namespace

namespace xmin::client::x11 {

xcb_connection_t *xlib_connection(Display *display) noexcept
{
    DisplayState *display_state = state(display);
    return display_state == nullptr ? nullptr : display_state->connection;
}

const xcb_connection_t *xlib_connection(const Display *display) noexcept
{
    const DisplayState *display_state = state(display);
    return display_state == nullptr ? nullptr : display_state->connection;
}

void xlib_init_image(XImage *image) noexcept
{
    if (image == nullptr)
        return;
    image->f.create_image = XCreateImage;
    image->f.destroy_image = image_destroy;
    image->f.get_pixel = image_get_pixel;
    image->f.put_pixel = image_put_pixel;
    image->f.sub_image = image_sub;
    image->f.add_pixel = image_add_pixel;
}

} // namespace xmin::client::x11

extern "C" {

Status XInitThreads(void)
{
    return 1;
}

Status XFreeThreads(void)
{
    return 1;
}

void XLockDisplay(Display *display)
{
    if (auto *display_state = state(display))
        display_state->mutex.lock();
}

void XUnlockDisplay(Display *display)
{
    if (auto *display_state = state(display))
        display_state->mutex.unlock();
}

xcb_connection_t *XGetXCBConnection(Display *display)
{
    auto *display_state = state(display);
    return display_state == nullptr ? nullptr : display_state->connection;
}

void XSetEventQueueOwner(Display *display, XEventQueueOwner owner)
{
    if (auto *display_state = state(display))
        display_state->queue_owner = owner;
}

XminWireToEventProc XESetWireToEvent(
    Display *display, int event_number, XminWireToEventProc procedure)
{
    auto *display_state = state(display);
    if (display_state == nullptr || event_number < 0 || event_number >= 256)
        return nullptr;
    auto &slot = display_state->wire_to_event[
        static_cast<std::size_t>(event_number)];
    const auto previous = slot;
    slot = procedure;
    return previous;
}

int _XDefaultIOError(Display *display)
{
    xmin::client::x11::xlib_dispatch_io_error(display);
    return -1;
}

unsigned long XNextRequest(Display *display)
{
    return display == nullptr ? 0 : private_display(display)->request + 1;
}

unsigned long XLastKnownRequestProcessed(Display *display)
{
    return display == nullptr ? 0 : private_display(display)->last_request_read;
}

Display *XOpenDisplay(const char *display_name)
{
    int preferred_screen = 0;
    xcb_connection_t *connection =
        xcb_connect(display_name, &preferred_screen);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
        return nullptr;
    }
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (setup == nullptr) {
        xcb_disconnect(connection);
        return nullptr;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < preferred_screen && screens.rem != 0;
         ++index) {
        xcb_screen_next(&screens);
    }
    if (screens.rem == 0) {
        xcb_disconnect(connection);
        return nullptr;
    }
    const xcb_screen_t *xcb_screen = screens.data;
    const xcb_visualtype_t *xcb_visual = root_visual_type(xcb_screen);
    if (xcb_visual == nullptr) {
        xcb_disconnect(connection);
        return nullptr;
    }

    auto display_state = std::unique_ptr<DisplayState>(
        new (std::nothrow) DisplayState);
    auto *storage = static_cast<_XPrivDisplay>(
        std::calloc(1, sizeof(*static_cast<_XPrivDisplay>(nullptr))));
    if (!display_state || storage == nullptr) {
        std::free(storage);
        xcb_disconnect(connection);
        return nullptr;
    }
    Display *display = reinterpret_cast<Display *>(storage);
    display_state->connection = connection;

    Visual &visual = display_state->visual;
    visual.visualid = xcb_visual->visual_id;
    visual.c_class = xcb_visual->_class;
    visual.red_mask = xcb_visual->red_mask;
    visual.green_mask = xcb_visual->green_mask;
    visual.blue_mask = xcb_visual->blue_mask;
    visual.bits_per_rgb = xcb_visual->bits_per_rgb_value;
    visual.map_entries = xcb_visual->colormap_entries;

    Depth &depth = display_state->depth;
    depth.depth = xcb_screen->root_depth;
    depth.nvisuals = 1;
    depth.visuals = &visual;

    Screen &screen = display_state->screen;
    screen.display = display;
    screen.root = xcb_screen->root;
    screen.width = xcb_screen->width_in_pixels;
    screen.height = xcb_screen->height_in_pixels;
    screen.mwidth = xcb_screen->width_in_millimeters;
    screen.mheight = xcb_screen->height_in_millimeters;
    screen.ndepths = 1;
    screen.depths = &depth;
    screen.root_depth = xcb_screen->root_depth;
    screen.root_visual = &visual;
    screen.cmap = xcb_screen->default_colormap;
    screen.white_pixel = xcb_screen->white_pixel;
    screen.black_pixel = xcb_screen->black_pixel;
    screen.max_maps = xcb_screen->max_installed_maps;
    screen.min_maps = xcb_screen->min_installed_maps;
    screen.backing_store = xcb_screen->backing_stores;
    screen.save_unders = xcb_screen->save_unders;
    screen.root_input_mask = xcb_screen->current_input_masks;

    storage->fd = xcb_get_file_descriptor(connection);
    storage->proto_major_version = setup->protocol_major_version;
    storage->proto_minor_version = setup->protocol_minor_version;
    storage->release = static_cast<int>(setup->release_number);
    storage->resource_alloc = allocate_resource;
    storage->byte_order = setup->image_byte_order;
    storage->bitmap_unit = setup->bitmap_format_scanline_unit;
    storage->bitmap_pad = setup->bitmap_format_scanline_pad;
    storage->bitmap_bit_order = setup->bitmap_format_bit_order;
    storage->max_request_size = setup->maximum_request_length;
    storage->default_screen = 0;
    storage->nscreens = 1;
    storage->screens = &screen;
    storage->min_keycode = setup->min_keycode;
    storage->max_keycode = setup->max_keycode;
    storage->private11 = reinterpret_cast<XPointer>(display_state.release());

    screen.default_gc = XCreateGC(display, screen.root, 0, nullptr);
    if (screen.default_gc == nullptr) {
        XCloseDisplay(display);
        return nullptr;
    }

    const char *name = display_name;
    if (name == nullptr || *name == '\0') {
        name = std::getenv("DISPLAY");
    }
    storage->display_name = ::strdup(name == nullptr ? "" : name);
    const char *vendor = reinterpret_cast<const char *>(setup + 1);
    const int vendor_length = setup->vendor_len;
    storage->vendor = static_cast<char *>(
        std::calloc(static_cast<std::size_t>(vendor_length) + 1, 1));
    if (storage->vendor != nullptr && vendor != nullptr) {
        std::memcpy(storage->vendor, vendor, vendor_length);
    }
    if (storage->display_name == nullptr || storage->vendor == nullptr) {
        XCloseDisplay(display);
        return nullptr;
    }
    return display;
}

int XCloseDisplay(Display *display)
{
    if (display == nullptr) {
        return 0;
    }
    _XPrivDisplay storage = private_display(display);
    xmin::client::x11::xlib_forget_events(display);
    std::unique_ptr<DisplayState> display_state(state(display));
    if (display_state && display_state->connection != nullptr) {
        if (display_state->screen.default_gc != nullptr) {
            XFreeGC(display, display_state->screen.default_gc);
            display_state->screen.default_gc = nullptr;
        }
        xcb_disconnect(display_state->connection);
    }
    std::free(storage->vendor);
    std::free(storage->display_name);
    std::free(storage);
    return 0;
}

int XConnectionNumber(Display *display)
{
    const auto *display_state = state(display);
    return display_state == nullptr
        ? -1
        : xcb_get_file_descriptor(display_state->connection);
}

char *XDisplayString(Display *display)
{
    return display == nullptr ? nullptr : private_display(display)->display_name;
}

char *XDisplayName(const char *display_name)
{
    if (display_name != nullptr && *display_name != '\0') {
        return const_cast<char *>(display_name);
    }
    const char *environment = std::getenv("DISPLAY");
    return const_cast<char *>(environment == nullptr ? "" : environment);
}

int XFlush(Display *display)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    return connection == nullptr ? 0 : xcb_flush(connection);
}

int XSync(Display *display, Bool discard)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection == nullptr) {
        return 0;
    }
    xcb_generic_error_t *error = nullptr;
    xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != nullptr)
        xmin::client::x11::xlib_dispatch_error(display, error);
    std::free(error);
    std::free(reply);
    xmin::client::x11::xlib_pump_events(display);
    if (discard)
        xmin::client::x11::xlib_forget_events(display);
    return xcb_connection_has_error(connection) == 0;
}

int XFree(void *data)
{
    std::free(data);
    return 1;
}

XImage *XCreateImage(
    Display *display, Visual *visual, unsigned int depth, int format,
    int offset, char *data, unsigned int width, unsigned int height,
    int bitmap_pad, int bytes_per_line)
{
    auto *image = static_cast<XImage *>(std::calloc(1, sizeof(XImage)));
    if (image == nullptr) {
        return nullptr;
    }
    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->xoffset = offset;
    image->format = format;
    image->data = data;
    image->byte_order = display == nullptr ? LSBFirst : ImageByteOrder(display);
    image->bitmap_unit = display == nullptr ? 32 : BitmapUnit(display);
    image->bitmap_bit_order =
        display == nullptr ? LSBFirst : BitmapBitOrder(display);
    image->bitmap_pad = bitmap_pad == 0 ? 32 : bitmap_pad;
    image->depth = static_cast<int>(depth);
    image->bits_per_pixel = format == XYBitmap || format == XYPixmap
        ? 1
        : (depth <= 1 ? 1 : (depth <= 8 ? 8 :
            (depth <= 16 ? 16 : 32)));
    const unsigned long row_width =
        static_cast<unsigned long>(width) +
        (format == XYBitmap || format == XYPixmap
            ? static_cast<unsigned long>(std::max(offset, 0))
            : 0UL);
    const unsigned long bits =
        row_width * static_cast<unsigned long>(image->bits_per_pixel);
    image->bytes_per_line = bytes_per_line != 0
        ? bytes_per_line
        : static_cast<int>(
              ((bits + image->bitmap_pad - 1U) /
               static_cast<unsigned int>(image->bitmap_pad)) *
              static_cast<unsigned int>(image->bitmap_pad) / 8U);
    if (visual != nullptr) {
        image->red_mask = visual->red_mask;
        image->green_mask = visual->green_mask;
        image->blue_mask = visual->blue_mask;
    }
    xmin::client::x11::xlib_init_image(image);
    return image;
}

int _XInitImageFuncPtrs(XImage *image)
{
    xmin::client::x11::xlib_init_image(image);
    return image != nullptr;
}

XImage *XGetImage(
    Display *display, Drawable drawable, int x, int y, unsigned int width,
    unsigned int height, unsigned long plane_mask, int format)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection == nullptr) {
        return nullptr;
    }
    xcb_generic_error_t *error = nullptr;
    std::unique_ptr<xcb_get_image_reply_t, decltype(&std::free)> reply(
        xcb_get_image_reply(
            connection,
            xcb_get_image_unchecked(
                connection, static_cast<std::uint8_t>(format),
                static_cast<xcb_drawable_t>(drawable),
                static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                static_cast<std::uint16_t>(width),
                static_cast<std::uint16_t>(height),
                static_cast<std::uint32_t>(plane_mask)),
            &error),
        std::free);
    std::free(error);
    if (!reply) {
        return nullptr;
    }
    XImage *image = XCreateImage(
        display, DefaultVisual(display, DefaultScreen(display)), reply->depth,
        format, 0, nullptr, width, height, BitmapPad(display), 0);
    if (image == nullptr) {
        return nullptr;
    }
    const int length = xcb_get_image_data_length(reply.get());
    image->data = static_cast<char *>(
        std::malloc(static_cast<std::size_t>(std::max(length, 0))));
    if (image->data == nullptr && length != 0) {
        XDestroyImage(image);
        return nullptr;
    }
    if (length != 0) {
        std::memcpy(image->data, xcb_get_image_data(reply.get()), length);
    }
    return image;
}

VisualID XVisualIDFromVisual(Visual *visual)
{
    return visual == nullptr ? 0 : visual->visualid;
}

Window XCreateWindow(
    Display *display, Window parent, int x, int y, unsigned int width,
    unsigned int height, unsigned int border_width, int depth,
    unsigned int window_class, Visual *visual, unsigned long value_mask,
    XSetWindowAttributes *attributes)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection == nullptr || (value_mask != 0 && attributes == nullptr)) {
        return None;
    }
    std::vector<std::uint32_t> values;
    values.reserve(15);
    if (attributes != nullptr) {
        append_window_value(values, value_mask, CWBackPixmap,
                            attributes->background_pixmap);
        append_window_value(values, value_mask, CWBackPixel,
                            attributes->background_pixel);
        append_window_value(values, value_mask, CWBorderPixmap,
                            attributes->border_pixmap);
        append_window_value(values, value_mask, CWBorderPixel,
                            attributes->border_pixel);
        append_window_value(values, value_mask, CWBitGravity,
                            attributes->bit_gravity);
        append_window_value(values, value_mask, CWWinGravity,
                            attributes->win_gravity);
        append_window_value(values, value_mask, CWBackingStore,
                            attributes->backing_store);
        append_window_value(values, value_mask, CWBackingPlanes,
                            attributes->backing_planes);
        append_window_value(values, value_mask, CWBackingPixel,
                            attributes->backing_pixel);
        append_window_value(values, value_mask, CWOverrideRedirect,
                            attributes->override_redirect);
        append_window_value(values, value_mask, CWSaveUnder,
                            attributes->save_under);
        append_window_value(values, value_mask, CWEventMask,
                            attributes->event_mask);
        append_window_value(values, value_mask, CWDontPropagate,
                            attributes->do_not_propagate_mask);
        append_window_value(values, value_mask, CWColormap,
                            attributes->colormap);
        append_window_value(values, value_mask, CWCursor,
                            attributes->cursor);
    }
    const Window window = xcb_generate_id(connection);
    xcb_create_window(
        connection,
        static_cast<std::uint8_t>(depth == CopyFromParent ? 0 : depth),
        static_cast<xcb_window_t>(window), static_cast<xcb_window_t>(parent),
        static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
        static_cast<std::uint16_t>(width),
        static_cast<std::uint16_t>(height),
        static_cast<std::uint16_t>(border_width),
        static_cast<std::uint16_t>(window_class),
        visual == CopyFromParent
            ? XCB_COPY_FROM_PARENT
            : static_cast<xcb_visualid_t>(visual->visualid),
        static_cast<std::uint32_t>(value_mask),
        values.empty() ? nullptr : values.data());
    return window;
}

Window XCreateSimpleWindow(
    Display *display, Window parent, int x, int y, unsigned int width,
    unsigned int height, unsigned int border_width, unsigned long border,
    unsigned long background)
{
    XSetWindowAttributes attributes{};
    attributes.border_pixel = border;
    attributes.background_pixel = background;
    return XCreateWindow(
        display, parent, x, y, width, height, border_width, CopyFromParent,
        InputOutput, CopyFromParent, CWBorderPixel | CWBackPixel, &attributes);
}

int XDestroyWindow(Display *display, Window window)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection != nullptr) {
        xcb_destroy_window(connection, static_cast<xcb_window_t>(window));
    }
    return connection != nullptr;
}

int XMapWindow(Display *display, Window window)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection != nullptr) {
        xcb_map_window(connection, static_cast<xcb_window_t>(window));
    }
    return connection != nullptr;
}

int XMapRaised(Display *display, Window window)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection == nullptr)
        return 0;
    const std::uint32_t mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(
        connection, static_cast<xcb_window_t>(window),
        XCB_CONFIG_WINDOW_STACK_MODE, &mode);
    xcb_map_window(connection, static_cast<xcb_window_t>(window));
    return 1;
}

int XUnmapWindow(Display *display, Window window)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    if (connection != nullptr) {
        xcb_unmap_window(connection, static_cast<xcb_window_t>(window));
    }
    return connection != nullptr;
}

int XSelectInput(Display *display, Window window, long event_mask)
{
    auto *connection = xmin::client::x11::xlib_connection(display);
    const std::uint32_t value = static_cast<std::uint32_t>(event_mask);
    if (connection != nullptr) {
        xcb_change_window_attributes(
            connection, static_cast<xcb_window_t>(window), XCB_CW_EVENT_MASK,
            &value);
    }
    return connection != nullptr;
}

} // extern "C"
