#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <stdio.h>
#include <stdlib.h>

struct required_extension {
    const char *name;
    xcb_extension_t *extension;
};

int
main(void)
{
    const struct required_extension required[] = {
        { "RENDER", &xcb_render_id },
        { "RANDR", &xcb_randr_id },
        { "SHAPE", &xcb_shape_id },
        { "MIT-SHM", &xcb_shm_id },
        { "SYNC", &xcb_sync_id },
        { "XFIXES", &xcb_xfixes_id },
        { "XKEYBOARD", &xcb_xkb_id },
    };
    xcb_render_query_pict_formats_reply_t *formats = NULL;
    struct xkb_context *context = NULL;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    xcb_connection_t *connection;
    int32_t device_id;
    int screen = -1;
    size_t i;
    int result = 1;

    connection = xcb_connect(NULL, &screen);
    if (xcb_connection_has_error(connection) != 0 || screen < 0) {
        fprintf(stderr, "libXminClient could not connect to DISPLAY\n");
        goto cleanup;
    }

    for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        const xcb_query_extension_reply_t *reply =
            xcb_get_extension_data(connection, required[i].extension);
        if (reply == NULL || !reply->present) {
            fprintf(stderr, "required Xmin extension is absent: %s\n",
                    required[i].name);
            goto cleanup;
        }
    }

    formats = xcb_render_query_pict_formats_reply(
        connection, xcb_render_query_pict_formats(connection), NULL);
    if (formats == NULL ||
        xcb_render_util_find_standard_format(formats,
                                             XCB_PICT_STANDARD_ARGB_32) == NULL) {
        fprintf(stderr, "Xmin has no ARGB32 RENDER format\n");
        goto cleanup;
    }

    if (!xkb_x11_setup_xkb_extension(
            connection,
            XKB_X11_MIN_MAJOR_XKB_VERSION,
            XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            NULL, NULL, NULL, NULL)) {
        fprintf(stderr, "xkbcommon-x11 could not initialize XKB\n");
        goto cleanup;
    }
    device_id = xkb_x11_get_core_keyboard_device_id(connection);
    if (device_id < 0) {
        fprintf(stderr, "xkbcommon-x11 found no core keyboard\n");
        goto cleanup;
    }
    context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if (context == NULL)
        goto cleanup;
    keymap = xkb_x11_keymap_new_from_device(
        context, connection, device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == NULL) {
        fprintf(stderr, "xkbcommon-x11 could not construct the server keymap\n");
        goto cleanup;
    }
    state = xkb_x11_state_new_from_device(keymap, connection, device_id);
    if (state == NULL) {
        fprintf(stderr, "xkbcommon-x11 could not construct keyboard state\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    free(formats);
    if (connection != NULL)
        xcb_disconnect(connection);
    return result;
}
