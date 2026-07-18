#ifndef XMIN_GLX_XCB_H
#define XMIN_GLX_XCB_H

#include <GL/glx.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xcb_connection_t;

/*
 * Create an opaque Display token that lets Xmin's software-direct GLX ABI
 * query and present X drawables through an existing XCB connection. The token
 * is private to Xmin and must only be passed to the glX entry points exported
 * by Xmin::GL.
 */
Display *xminGlxCreateXcbDisplay(struct xcb_connection_t *connection);
void xminGlxDestroyXcbDisplay(Display *display);

#ifdef __cplusplus
}
#endif

#endif
