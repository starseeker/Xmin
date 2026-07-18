#ifndef XMIN_XCB_GLX_H
#define XMIN_XCB_GLX_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCB_GLX_MAJOR_VERSION 1
#define XCB_GLX_MINOR_VERSION 4
#define XCB_GLX_QUERY_VERSION 7
#define XCB_GLX_BUFFER_SWAP_COMPLETE 1

extern xcb_extension_t xcb_glx_id;
typedef uint32_t xcb_glx_drawable_t;
typedef struct { unsigned int sequence; } xcb_glx_query_version_cookie_t;
typedef struct {
    uint8_t major_opcode; uint8_t minor_opcode; uint16_t length;
    uint32_t major_version; uint32_t minor_version;
} xcb_glx_query_version_request_t;
typedef struct {
    uint8_t response_type; uint8_t pad0; uint16_t sequence; uint32_t length;
    uint32_t major_version; uint32_t minor_version; uint8_t pad1[16];
} xcb_glx_query_version_reply_t;
typedef struct {
    uint8_t response_type; uint8_t pad0; uint16_t sequence;
    uint16_t event_type; uint8_t pad1[2]; xcb_glx_drawable_t drawable;
    uint32_t ust_hi; uint32_t ust_lo; uint32_t msc_hi; uint32_t msc_lo;
    uint32_t sbc;
} xcb_glx_buffer_swap_complete_event_t;

xcb_glx_query_version_cookie_t xcb_glx_query_version(
    xcb_connection_t *connection, uint32_t major_version,
    uint32_t minor_version);
xcb_glx_query_version_reply_t *xcb_glx_query_version_reply(
    xcb_connection_t *connection, xcb_glx_query_version_cookie_t cookie,
    xcb_generic_error_t **error);

#ifdef __cplusplus
}
#endif
#endif
