#ifndef XMIN_XCB_PRESENT_H
#define XMIN_XCB_PRESENT_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCB_PRESENT_MAJOR_VERSION 1
#define XCB_PRESENT_MINOR_VERSION 4
#define XCB_PRESENT_QUERY_VERSION 0
#define XCB_PRESENT_PIXMAP 1
#define XCB_PRESENT_OPTION_ASYNC 1
#define XCB_PRESENT_OPTION_COPY 2

extern xcb_extension_t xcb_present_id;

typedef struct { unsigned int sequence; } xcb_present_query_version_cookie_t;

typedef struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    uint32_t major_version;
    uint32_t minor_version;
} xcb_present_query_version_request_t;

typedef struct {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t length;
    uint32_t major_version;
    uint32_t minor_version;
    uint8_t pad1[16];
} xcb_present_query_version_reply_t;

typedef struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    xcb_window_t window;
    xcb_pixmap_t pixmap;
    uint32_t serial;
    uint32_t valid;
    uint32_t update;
    int16_t x_off;
    int16_t y_off;
    uint32_t target_crtc;
    uint32_t wait_fence;
    uint32_t idle_fence;
    uint32_t options;
    uint8_t pad0[4];
    uint64_t target_msc;
    uint64_t divisor;
    uint64_t remainder;
} xcb_present_pixmap_request_t;

xcb_present_query_version_cookie_t xcb_present_query_version(
    xcb_connection_t *connection, uint32_t major_version,
    uint32_t minor_version);
xcb_present_query_version_reply_t *xcb_present_query_version_reply(
    xcb_connection_t *connection, xcb_present_query_version_cookie_t cookie,
    xcb_generic_error_t **error);
xcb_void_cookie_t xcb_present_pixmap_checked(
    xcb_connection_t *connection, xcb_window_t window, xcb_pixmap_t pixmap,
    uint32_t serial, uint32_t valid, uint32_t update, int16_t x_off,
    int16_t y_off, uint32_t target_crtc, uint32_t wait_fence,
    uint32_t idle_fence, uint32_t options, uint64_t target_msc,
    uint64_t divisor, uint64_t remainder, uint32_t notifies_len,
    const void *notifies);

#ifdef __cplusplus
}
#endif

#endif
