#ifndef XMIN_X11_XLIB_XCB_H
#define XMIN_X11_XLIB_XCB_H

#include <X11/Xlib.h>
#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XlibOwnsEventQueue = 0,
    XCBOwnsEventQueue = 1
} XEventQueueOwner;

xcb_connection_t *XGetXCBConnection(Display *display);
void XSetEventQueueOwner(Display *display, XEventQueueOwner owner);

#ifdef __cplusplus
}
#endif

#endif
