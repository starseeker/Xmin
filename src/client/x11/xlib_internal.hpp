#ifndef XMIN_CLIENT_X11_XLIB_INTERNAL_HPP
#define XMIN_CLIENT_X11_XLIB_INTERNAL_HPP

#include <X11/Xlib.h>
#include <xcb/xcb.h>

namespace xmin::client::x11 {

xcb_connection_t *xlib_connection(Display *display) noexcept;
const xcb_connection_t *xlib_connection(const Display *display) noexcept;
void xlib_dispatch_error(
    Display *display, const xcb_generic_error_t *error) noexcept;
void xlib_dispatch_io_error(Display *display) noexcept;
void xlib_pump_events(Display *display) noexcept;
void xlib_forget_events(Display *display) noexcept;
void xlib_init_image(XImage *image) noexcept;

} // namespace xmin::client::x11

#endif
