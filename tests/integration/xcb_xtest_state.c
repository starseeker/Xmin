#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xtest.h>

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);

    if (error == NULL)
        return 1;
    fprintf(stderr, "%s failed with X11 error %u\n",
            operation, error->error_code);
    free(error);
    return 0;
}

int
main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection) != 0) {
        fprintf(stderr, "unable to connect to DISPLAY\n");
        return 1;
    }
    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    while (screen_number-- > 0)
        xcb_screen_next(&screens);
    xcb_screen_t *screen = screens.data;
    if (screen == NULL) {
        fprintf(stderr, "DISPLAY has no selected screen\n");
        xcb_disconnect(connection);
        return 1;
    }

    int passed = 0;
    xcb_generic_error_t *error = NULL;
    xcb_test_get_version_reply_t *version = xcb_test_get_version_reply(
        connection, xcb_test_get_version(connection, 2, 2), &error);
    if (error != NULL || version == NULL || version->major_version != 2 ||
        version->minor_version < 2) {
        fprintf(stderr, "XTEST 2.2 negotiation failed\n");
        goto cleanup;
    }

    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_PRESS, 96, 0, XCB_NONE,
                     0, 0, 0),
                 "XTEST KeyPress")) {
        goto cleanup;
    }
    xcb_query_keymap_reply_t *keys = xcb_query_keymap_reply(
        connection, xcb_query_keymap(connection), &error);
    if (error != NULL || keys == NULL || (keys->keys[12] & 1U) == 0) {
        fprintf(stderr, "XTEST key state was not observable\n");
        free(keys);
        goto cleanup;
    }
    free(keys);
    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_RELEASE, 96, 0, XCB_NONE,
                     0, 0, 0),
                 "XTEST KeyRelease") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_MOTION_NOTIFY, 0, 0, screen->root,
                     17, 19, 0),
                 "XTEST absolute motion")) {
        goto cleanup;
    }

    xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL ||
        pointer->root_x != 17 || pointer->root_y != 19) {
        fprintf(stderr, "XTEST motion state was not observable\n");
        free(pointer);
        goto cleanup;
    }
    free(pointer);
    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_BUTTON_PRESS, 1, 0, XCB_NONE,
                     0, 0, 0),
                 "XTEST ButtonPress")) {
        goto cleanup;
    }
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL ||
        (pointer->mask & XCB_BUTTON_MASK_1) == 0) {
        fprintf(stderr, "XTEST button state was not observable\n");
        free(pointer);
        goto cleanup;
    }
    free(pointer);

    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_BUTTON_RELEASE, 1, 0, XCB_NONE,
                     0, 0, 0),
                 "XTEST ButtonRelease") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_MOTION_NOTIFY, 0, 0, screen->root,
                     (int16_t) (screen->width_in_pixels / 2),
                     (int16_t) (screen->height_in_pixels / 2), 0),
                 "restore pointer") ||
        !checked(connection, xcb_test_grab_control_checked(connection, 1),
                 "XTEST GrabControl")) {
        goto cleanup;
    }

    xcb_test_compare_cursor_reply_t *cursor = xcb_test_compare_cursor_reply(
        connection,
        xcb_test_compare_cursor(
            connection, screen->root, XCB_TEST_CURSOR_CURRENT),
        &error);
    if (error != NULL || cursor == NULL || !cursor->same) {
        fprintf(stderr, "XTEST current-cursor comparison failed\n");
        free(cursor);
        goto cleanup;
    }
    free(cursor);
    passed = 1;

cleanup:
    free(error);
    free(version);
    xcb_disconnect(connection);
    return passed ? 0 : 1;
}
