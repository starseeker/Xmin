#ifndef XMIN_X11_XLIBINT_H
#define XMIN_X11_XLIBINT_H

/*
 * Focused compatibility declarations used by Qt's qxcb GLX integration.
 * Xmin does not expose or depend on libX11's private Display implementation.
 */
#include <X11/Xlib.h>
#include <X11/Xproto.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef Bool (*XminWireToEventProc)(Display *, XEvent *, xEvent *);

XminWireToEventProc XESetWireToEvent(
    Display *display, int event_number, XminWireToEventProc procedure);
int _XDefaultIOError(Display *display);

#ifdef __cplusplus
}
#endif

#endif
