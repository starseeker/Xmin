#ifndef XMIN_XCB_CONNECT_H
#define XMIN_XCB_CONNECT_H

#include <stddef.h>

#include <xcb/xcb.h>

typedef struct xmin_xcb_session {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    int display_number;
    int screen_number;
} xmin_xcb_session;

int xmin_xcb_connect(xmin_xcb_session *session,
                     const char *display,
                     char *error,
                     size_t error_size);
void xmin_xcb_disconnect(xmin_xcb_session *session);

#endif
