#include <xcb/xcb.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static xcb_screen_t *
default_screen(xcb_connection_t *connection)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (setup == NULL)
        return NULL;
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    return screens.rem == 0 ? NULL : screens.data;
}

static int
wait_for_window_event(xcb_connection_t *connection, uint8_t type,
                      xcb_window_t window)
{
    for (;;) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        if (event == NULL)
            return 0;
        const uint8_t received = event->response_type & 0x7fU;
        int matches = 0;
        if (received == XCB_MAP_NOTIFY && type == XCB_MAP_NOTIFY) {
            const xcb_map_notify_event_t *mapped =
                (const xcb_map_notify_event_t *) event;
            matches = mapped->event == window && mapped->window == window;
        }
        else if (received == XCB_UNMAP_NOTIFY && type == XCB_UNMAP_NOTIFY) {
            const xcb_unmap_notify_event_t *unmapped =
                (const xcb_unmap_notify_event_t *) event;
            matches = unmapped->event == window && unmapped->window == window;
        }
        free(event);
        if (matches)
            return 1;
    }
}

int
main(void)
{
    int preferred_screen = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &preferred_screen);
    if (connection == NULL || xcb_connection_has_error(connection) != 0)
        return 1;
    xcb_screen_t *screen = default_screen(connection);
    if (screen == NULL) {
        xcb_disconnect(connection);
        return 2;
    }

    const xcb_window_t window = xcb_generate_id(connection);
    const uint32_t window_values[] = {XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      4, 5, 8, 8, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_EVENT_MASK, window_values);
    xcb_map_window(connection, window);
    if (xcb_flush(connection) <= 0 ||
        !wait_for_window_event(connection, XCB_MAP_NOTIFY, window)) {
        xcb_disconnect(connection);
        return 3;
    }

    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, window), NULL);
    if (geometry == NULL || geometry->width != 8 || geometry->height != 8 ||
        geometry->depth != screen->root_depth) {
        free(geometry);
        xcb_disconnect(connection);
        return 4;
    }
    free(geometry);

    const xcb_gcontext_t graphics = xcb_generate_id(connection);
    xcb_create_gc(connection, graphics, window, 0, NULL);
    uint32_t pixels[64];
    for (size_t index = 0; index < 64; ++index)
        pixels[index] = 0x0000ff00U;
    xcb_put_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, graphics,
                  8, 8, 0, 0, 0, screen->root_depth,
                  sizeof(pixels), (const uint8_t *) pixels);
    xcb_get_image_reply_t *image = xcb_get_image_reply(
        connection,
        xcb_get_image_unchecked(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                                0, 0, 8, 8, UINT32_MAX),
        NULL);
    uint32_t observed = 0;
    if (image != NULL && xcb_get_image_data_length(image) >= 4)
        memcpy(&observed, xcb_get_image_data(image), sizeof(observed));
    free(image);
    if (observed != pixels[0]) {
        xcb_disconnect(connection);
        return 5;
    }

    xcb_free_gc(connection, graphics);
    xcb_destroy_window(connection, window);
    if (xcb_flush(connection) <= 0 ||
        !wait_for_window_event(connection, XCB_UNMAP_NOTIFY, window)) {
        xcb_disconnect(connection);
        return 6;
    }
    xcb_disconnect(connection);
    return 0;
}
