#ifndef XMIN_TESTS_INTEGRATION_XCB_GE_QUERY_H
#define XMIN_TESTS_INTEGRATION_XCB_GE_QUERY_H

#include <stdint.h>
#include <sys/uio.h>

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

/*
 * Generic Event is part of the X11 protocol surface but many distributions do
 * not ship a generated libxcb ge.h binding.  Keep this tiny version query on
 * the wire so the conformance test does not acquire a packaging dependency.
 */
typedef struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    uint16_t client_major_version;
    uint16_t client_minor_version;
} xmin_ge_query_version_request_t;

typedef struct {
    uint8_t response_type;
    uint8_t pad0;
    uint16_t sequence;
    uint32_t length;
    uint16_t major_version;
    uint16_t minor_version;
    uint8_t pad1[20];
} xmin_ge_query_version_reply_t;

static xmin_ge_query_version_reply_t *
xmin_query_ge_version(xcb_connection_t *connection,
                      xcb_generic_error_t **error)
{
    static xcb_extension_t extension = {
        .name = "Generic Event Extension",
        .global_id = 0
    };
    static const xcb_protocol_request_t protocol = {
        .count = 2,
        .ext = &extension,
        .opcode = 0,
        .isvoid = 0
    };
    xmin_ge_query_version_request_t request = {
        .client_major_version = 1,
        .client_minor_version = 0
    };
    struct iovec parts[4];
    unsigned int sequence;

    parts[2].iov_base = &request;
    parts[2].iov_len = sizeof(request);
    parts[3].iov_base = NULL;
    parts[3].iov_len = (size_t) (-(int) sizeof(request)) & 3U;
    sequence = xcb_send_request(connection, XCB_REQUEST_CHECKED, parts + 2,
                                &protocol);
    return xcb_wait_for_reply(connection, sequence, error);
}

#endif
