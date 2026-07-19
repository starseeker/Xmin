#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

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
    uint32_t acquire_syncobj;
    uint32_t release_syncobj;
    uint64_t acquire_point;
    uint64_t release_point;
    uint32_t options;
    uint8_t pad0[4];
    uint64_t target_msc;
    uint64_t divisor;
    uint64_t remainder;
} xmin_present_pixmap_synced_request_t;

_Static_assert(sizeof(xmin_present_pixmap_synced_request_t) == 88,
               "PresentPixmapSynced wire request must be 88 bytes");

static xcb_void_cookie_t
present_pixmap_synced_checked(xcb_connection_t *connection,
                              xcb_window_t window, xcb_pixmap_t pixmap,
                              uint32_t serial)
{
    static const xcb_protocol_request_t protocol = {
        .count = 2,
        .ext = &xcb_present_id,
        .opcode = 5,
        .isvoid = 1
    };
    xmin_present_pixmap_synced_request_t request = {
        .window = window,
        .pixmap = pixmap,
        .serial = serial
    };
    struct iovec parts[4];
    xcb_void_cookie_t cookie;

    parts[2].iov_base = &request;
    parts[2].iov_len = sizeof(request);
    parts[3].iov_base = NULL;
    parts[3].iov_len = 0;
    cookie.sequence = xcb_send_request(
        connection, XCB_REQUEST_CHECKED, parts + 2, &protocol);
    return cookie;
}

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    int result = error == NULL && xcb_connection_has_error(connection) == 0;

    if (!result) {
        fprintf(stderr, "%s failed with X11 error %u\n", operation,
                error == NULL ? 0U : error->error_code);
    }
    free(error);
    return result;
}

static int
checked_error(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              uint8_t expected, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    int result = error != NULL && error->error_code == expected;

    if (!result) {
        fprintf(stderr, "%s returned X11 error %u instead of %u\n",
                operation, error == NULL ? 0U : error->error_code,
                expected);
    }
    free(error);
    return result;
}

static xcb_generic_event_t *
next_event(xcb_connection_t *connection)
{
    int attempt;

    for (attempt = 0; attempt < 40; ++attempt) {
        xcb_generic_event_t *event = xcb_poll_for_event(connection);

        if (event != NULL)
            return event;
        {
            struct pollfd ready = {
                .fd = xcb_get_file_descriptor(connection),
                .events = POLLIN
            };

            (void) poll(&ready, 1, 50);
        }
    }
    return NULL;
}

static int
wait_for_configure(xcb_connection_t *connection, uint8_t opcode,
                   xcb_present_event_t event_id, xcb_window_t window,
                   uint16_t width, uint16_t height)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event = next_event(connection);

        if (event == NULL)
            return 0;
        if ((event->response_type & 0x7fU) == XCB_GE_GENERIC) {
            xcb_present_configure_notify_event_t *configure =
                (xcb_present_configure_notify_event_t *) event;

            if (configure->extension == opcode &&
                configure->event_type == XCB_PRESENT_CONFIGURE_NOTIFY &&
                configure->event == event_id &&
                configure->window == window && configure->width == width &&
                configure->height == height &&
                configure->pixmap_width == width &&
                configure->pixmap_height == height) {
                free(event);
                return 1;
            }
        }
        free(event);
    }
    return 0;
}

static int
wait_for_pixmap(xcb_connection_t *connection, uint8_t opcode,
                xcb_present_event_t primary_event,
                xcb_present_event_t notify_event, xcb_window_t window,
                xcb_pixmap_t pixmap, uint32_t serial,
                uint32_t notify_serial, xcb_sync_fence_t idle_fence,
                uint64_t *msc)
{
    int saw_primary = 0;
    int saw_notify = 0;
    int saw_idle = 0;
    int attempt;

    for (attempt = 0; attempt < 40; ++attempt) {
        xcb_generic_event_t *event = next_event(connection);

        if (event == NULL)
            return 0;
        if ((event->response_type & 0x7fU) == XCB_GE_GENERIC) {
            xcb_present_generic_event_t *generic =
                (xcb_present_generic_event_t *) event;

            if (generic->extension == opcode &&
                generic->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *complete =
                    (xcb_present_complete_notify_event_t *) event;

                if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP &&
                    complete->mode == XCB_PRESENT_COMPLETE_MODE_COPY &&
                    complete->ust != 0) {
                    if (complete->event == primary_event &&
                        complete->window == window &&
                        complete->serial == serial) {
                        *msc = complete->msc;
                        saw_primary = 1;
                    }
                    else if (complete->event == notify_event &&
                             complete->serial == notify_serial) {
                        saw_notify = 1;
                    }
                }
            }
            else if (generic->extension == opcode &&
                     generic->evtype == XCB_PRESENT_IDLE_NOTIFY) {
                xcb_present_idle_notify_event_t *idle =
                    (xcb_present_idle_notify_event_t *) event;

                if (idle->event == primary_event && idle->window == window &&
                    idle->serial == serial && idle->pixmap == pixmap &&
                    idle->idle_fence == idle_fence) {
                    saw_idle = 1;
                }
            }
        }
        free(event);
        if (saw_primary && saw_notify && saw_idle)
            return 1;
    }
    return 0;
}

static int
wait_for_msc(xcb_connection_t *connection, uint8_t opcode,
             xcb_present_event_t event_id, uint32_t serial,
             uint64_t minimum_msc)
{
    int attempt;

    for (attempt = 0; attempt < 40; ++attempt) {
        xcb_generic_event_t *event = next_event(connection);

        if (event == NULL)
            return 0;
        if ((event->response_type & 0x7fU) == XCB_GE_GENERIC) {
            xcb_present_complete_notify_event_t *complete =
                (xcb_present_complete_notify_event_t *) event;

            if (complete->extension == opcode &&
                complete->event_type == XCB_PRESENT_COMPLETE_NOTIFY &&
                complete->event == event_id && complete->serial == serial &&
                complete->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC &&
                complete->mode == XCB_PRESENT_COMPLETE_MODE_COPY &&
                complete->ust != 0 && complete->msc >= minimum_msc) {
                free(event);
                return 1;
            }
        }
        free(event);
    }
    return 0;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup(connection)).data;
    const xcb_query_extension_reply_t *extension = NULL;
    xcb_generic_error_t *error = NULL;
    xcb_present_query_version_reply_t *version = NULL;
    xcb_present_query_capabilities_reply_t *capabilities = NULL;
    xcb_sync_initialize_reply_t *sync_version = NULL;
    xcb_sync_query_fence_reply_t *fence_reply = NULL;
    xcb_window_t window = XCB_NONE;
    xcb_window_t notify_window = XCB_NONE;
    xcb_pixmap_t pixmap = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    xcb_present_event_t event_id = XCB_NONE;
    xcb_present_event_t notify_event_id = XCB_NONE;
    xcb_present_event_t redirect_event_id = XCB_NONE;
    xcb_sync_fence_t idle_fence = XCB_NONE;
    xcb_get_image_reply_t *image = NULL;
    xcb_present_notify_t notify;
    const uint32_t serial = 0x584d494eU;
    const uint32_t notify_serial = 0x4e4f5449U;
    const uint32_t msc_serial = 0x4d53434eU;
    uint32_t green = 0x0000ff00U;
    uint32_t configure_values[2] = {40, 28};
    uint32_t pixel = 0;
    uint64_t presented_msc = 0;
    const char *stage = "connecting";
    int passed = 0;

    if (xcb_connection_has_error(connection) || screen == NULL)
        goto cleanup;
    extension = xcb_get_extension_data(connection, &xcb_present_id);
    if (extension == NULL || !extension->present)
        goto cleanup;

    stage = "negotiating Present and SYNC";
    version = xcb_present_query_version_reply(
        connection, xcb_present_query_version(connection, 1, 4), &error);
    if (error != NULL || version == NULL || version->major_version != 1 ||
        version->minor_version < 2)
        goto cleanup;
    sync_version = xcb_sync_initialize_reply(
        connection, xcb_sync_initialize(connection, 3, 1), &error);
    if (error != NULL || sync_version == NULL)
        goto cleanup;
    capabilities = xcb_present_query_capabilities_reply(
        connection,
        xcb_present_query_capabilities(connection, screen->root), &error);
    if (error != NULL || capabilities == NULL ||
        capabilities->capabilities != XCB_PRESENT_CAPABILITY_NONE)
        goto cleanup;

    window = xcb_generate_id(connection);
    notify_window = xcb_generate_id(connection);
    pixmap = xcb_generate_id(connection);
    graphics = xcb_generate_id(connection);
    event_id = xcb_generate_id(connection);
    notify_event_id = xcb_generate_id(connection);
    redirect_event_id = xcb_generate_id(connection);
    idle_fence = xcb_generate_id(connection);
    stage = "creating Present resources";
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, window, screen->root,
                     0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, 0, NULL),
                 "CreateWindow") ||
        !checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, notify_window,
                     screen->root, 45, 0, 8, 8, 0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                     0, NULL),
                 "Create notify window") ||
        !checked(connection, xcb_map_window_checked(connection, window),
                 "MapWindow") ||
        !checked(connection,
                 xcb_map_window_checked(connection, notify_window),
                 "Map notify window") ||
        !checked(connection,
                 xcb_create_pixmap_checked(
                     connection, screen->root_depth, pixmap, screen->root,
                     32, 24),
                 "CreatePixmap") ||
        !checked(connection,
                 xcb_create_gc_checked(connection, graphics, pixmap,
                                       XCB_GC_FOREGROUND, &green),
                 "CreateGC") ||
        !checked(connection,
                 xcb_poly_fill_rectangle_checked(
                     connection, pixmap, graphics, 1,
                     &(xcb_rectangle_t){0, 0, 32, 24}),
                 "Fill pixmap") ||
        !checked(connection,
                 xcb_sync_create_fence_checked(
                     connection, screen->root, idle_fence, 0),
                 "Create idle fence") ||
        !checked(connection,
                 xcb_present_select_input_checked(
                     connection, event_id, window,
                     XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                     XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                     XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY),
                 "Select primary Present input") ||
        !checked(connection,
                 xcb_present_select_input_checked(
                     connection, notify_event_id, notify_window,
                     XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY),
                 "Select notify Present input")) {
        goto cleanup;
    }

    stage = "receiving ConfigureNotify";
    if (!checked(connection,
                 xcb_configure_window_checked(
                     connection, window,
                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                     configure_values),
                 "ConfigureWindow") ||
        !wait_for_configure(connection, extension->major_opcode, event_id,
                            window, 40, 28)) {
        goto cleanup;
    }

    notify.window = notify_window;
    notify.serial = notify_serial;
    stage = "presenting software pixmap";
    if (!checked(connection,
                 xcb_present_pixmap_checked(
                     connection, window, pixmap, serial, XCB_NONE, XCB_NONE,
                     0, 0, XCB_NONE, XCB_NONE, idle_fence, 0,
                     0, 0, 0, 1, &notify),
                 "PresentPixmap") ||
        !wait_for_pixmap(connection, extension->major_opcode, event_id,
                         notify_event_id, window, pixmap, serial,
                         notify_serial, idle_fence, &presented_msc)) {
        goto cleanup;
    }
    fence_reply = xcb_sync_query_fence_reply(
        connection, xcb_sync_query_fence(connection, idle_fence), &error);
    if (error != NULL || fence_reply == NULL || !fence_reply->triggered)
        goto cleanup;
    free(fence_reply);
    fence_reply = NULL;

    stage = "notifying a future MSC";
    if (!checked(connection,
                 xcb_present_notify_msc_checked(
                     connection, window, msc_serial, presented_msc + 1,
                     0, 0),
                 "Present NotifyMSC") ||
        !wait_for_msc(connection, extension->major_opcode, event_id,
                      msc_serial, presented_msc + 1)) {
        goto cleanup;
    }

    stage = "reading presented pixels";
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      16, 12, 1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != green)
        goto cleanup;

    stage = "checking Present errors";
    if (!checked_error(
            connection,
            xcb_present_select_input_checked(
                connection, redirect_event_id, window,
                XCB_PRESENT_EVENT_MASK_REDIRECT_NOTIFY),
            XCB_VALUE, "Present RedirectNotify selection") ||
        !checked_error(
            connection,
            xcb_present_pixmap_checked(
                connection, window, pixmap, serial + 1,
                XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                1U << 31, 0, 0, 0, 0, NULL),
            XCB_VALUE, "Present invalid options")) {
        goto cleanup;
    }
    if (version->minor_version >= 4 &&
        !checked_error(
            connection,
            present_pixmap_synced_checked(connection, window, pixmap,
                                          serial + 2),
            XCB_MATCH, "PresentPixmapSynced software rejection")) {
        goto cleanup;
    }

    stage = "removing Present selections";
    if (!checked(connection,
                 xcb_present_select_input_checked(
                     connection, event_id, window,
                     XCB_PRESENT_EVENT_MASK_NO_EVENT),
                 "Remove primary Present input") ||
        !checked(connection,
                 xcb_present_select_input_checked(
                     connection, notify_event_id, notify_window,
                     XCB_PRESENT_EVENT_MASK_NO_EVENT),
                 "Remove notify Present input")) {
        goto cleanup;
    }
    passed = 1;

cleanup:
    if (!passed)
        fprintf(stderr, "Present state test failed while %s\n", stage);
    if (idle_fence != XCB_NONE)
        xcb_sync_destroy_fence(connection, idle_fence);
    if (graphics != XCB_NONE)
        xcb_free_gc(connection, graphics);
    if (pixmap != XCB_NONE)
        xcb_free_pixmap(connection, pixmap);
    if (notify_window != XCB_NONE)
        xcb_destroy_window(connection, notify_window);
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    free(image);
    free(fence_reply);
    free(sync_version);
    free(capabilities);
    free(version);
    free(error);
    xcb_disconnect(connection);
    return passed ? 0 : 1;
}
