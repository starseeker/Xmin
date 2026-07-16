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

static xcb_generic_event_t *
poll_event_type(xcb_connection_t *connection, uint8_t type)
{
    xcb_generic_event_t *event;

    while ((event = xcb_poll_for_event(connection)) != NULL) {
        if ((event->response_type & 0x7fU) == type)
            return event;
        free(event);
    }
    return NULL;
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
    const uint32_t event_mask =
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE;
    if (!checked(connection,
                 xcb_change_window_attributes_checked(
                     connection, screen->root, XCB_CW_EVENT_MASK,
                     &event_mask),
                 "select core input events")) {
        xcb_disconnect(connection);
        return 1;
    }
    const xcb_window_t child = xcb_generate_id(connection);
    const uint32_t child_event_mask =
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE;
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, XCB_COPY_FROM_PARENT, child, screen->root,
                     10, 10, 30, 30, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, XCB_CW_EVENT_MASK,
                     &child_event_mask),
                 "create crossing child") ||
        !checked(connection, xcb_map_window_checked(connection, child),
                 "map crossing child")) {
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
    xcb_generic_event_t *event = poll_event_type(connection, XCB_KEY_PRESS);
    xcb_key_press_event_t *key_event = (xcb_key_press_event_t *) event;
    if (key_event == NULL || key_event->detail != 96 ||
        key_event->root != screen->root || key_event->event != screen->root ||
        key_event->state != 0 || !key_event->same_screen) {
        fprintf(stderr, "XTEST KeyPress event routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
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
    event = poll_event_type(connection, XCB_KEY_RELEASE);
    key_event = (xcb_key_press_event_t *) event;
    if (key_event == NULL || key_event->detail != 96 ||
        key_event->event != screen->root || key_event->state != 0) {
        fprintf(stderr, "XTEST KeyRelease event routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL ||
        pointer->root_x != 17 || pointer->root_y != 19) {
        fprintf(stderr, "XTEST motion state was not observable\n");
        free(pointer);
        goto cleanup;
    }
    free(pointer);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    xcb_leave_notify_event_t *leave_event =
        (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != screen->root || leave_event->child != XCB_NONE ||
        leave_event->root_x != 17 || leave_event->root_y != 19 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "XTEST LeaveNotify crossing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    xcb_enter_notify_event_t *enter_event =
        (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child || enter_event->child != XCB_NONE ||
        enter_event->event_x != 7 || enter_event->event_y != 9 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "XTEST EnterNotify crossing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_MOTION_NOTIFY);
    xcb_motion_notify_event_t *motion_event =
        (xcb_motion_notify_event_t *) event;
    if (motion_event == NULL || motion_event->detail != 0 ||
        motion_event->event != child ||
        motion_event->root_x != 17 || motion_event->root_y != 19 ||
        motion_event->event_x != 7 || motion_event->event_y != 9) {
        if (motion_event == NULL) {
            fprintf(stderr, "XTEST MotionNotify event routing failed: no event\n");
        }
        else {
            fprintf(stderr,
                    "XTEST MotionNotify event routing failed: detail=%u "
                    "event=%#x root=%#x root_xy=(%d,%d) event_xy=(%d,%d)\n",
                    motion_event->detail, motion_event->event,
                    motion_event->root, motion_event->root_x,
                    motion_event->root_y, motion_event->event_x,
                    motion_event->event_y);
        }
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_BUTTON_PRESS, 1, 0, XCB_NONE,
                     0, 0, 0),
                 "XTEST ButtonPress")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_BUTTON_PRESS);
    xcb_button_press_event_t *button_event =
        (xcb_button_press_event_t *) event;
    if (button_event == NULL || button_event->detail != 1 ||
        button_event->event != screen->root || button_event->state != 0) {
        fprintf(stderr, "XTEST ButtonPress event routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child ||
        leave_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "automatic-grab LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != screen->root ||
        enter_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "automatic-grab EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
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
    event = poll_event_type(connection, XCB_BUTTON_RELEASE);
    button_event = (xcb_button_press_event_t *) event;
    if (button_event == NULL || button_event->detail != 1 ||
        button_event->event != screen->root ||
        button_event->state != XCB_BUTTON_MASK_1) {
        fprintf(stderr, "XTEST ButtonRelease event routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != screen->root ||
        leave_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "automatic-ungrab LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child ||
        enter_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "automatic-ungrab EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child || leave_event->child != XCB_NONE ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        if (leave_event == NULL) {
            fprintf(stderr,
                    "XTEST restore LeaveNotify crossing failed: no event\n");
        }
        else {
            fprintf(stderr,
                    "XTEST restore LeaveNotify crossing failed: detail=%u "
                    "event=%#x child=%#x root_xy=(%d,%d)\n",
                    leave_event->detail, leave_event->event,
                    leave_event->child, leave_event->root_x,
                    leave_event->root_y);
        }
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != screen->root || enter_event->child != XCB_NONE) {
        fprintf(stderr, "XTEST restore EnterNotify crossing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_MOTION_NOTIFY);
    motion_event = (xcb_motion_notify_event_t *) event;
    if (motion_event == NULL || motion_event->event != screen->root) {
        fprintf(stderr, "XTEST restore MotionNotify routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_warp_pointer_checked(
                     connection, XCB_NONE, screen->root,
                     0, 0, 0, 0, 17, 19),
                 "core WarpPointer")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != screen->root) {
        fprintf(stderr, "WarpPointer LeaveNotify crossing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child) {
        fprintf(stderr, "WarpPointer EnterNotify crossing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_MOTION_NOTIFY);
    motion_event = (xcb_motion_notify_event_t *) event;
    if (motion_event == NULL || motion_event->event != child ||
        motion_event->root_x != 17 || motion_event->root_y != 19) {
        fprintf(stderr, "WarpPointer MotionNotify routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "focus crossing child")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    xcb_focus_out_event_t *focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "child NotifyPointer FocusOut routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER ||
        focus_event->event != screen->root) {
        fprintf(stderr, "root NotifyPointer FocusOut routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER_ROOT ||
        focus_event->event != screen->root) {
        fprintf(stderr, "PointerRoot FocusOut routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR_VIRTUAL ||
        focus_event->event != screen->root) {
        fprintf(stderr, "root virtual FocusIn routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR ||
        focus_event->event != child) {
        fprintf(stderr, "child FocusIn routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, screen->root,
                     XCB_CURRENT_TIME),
                 "focus root")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child) {
        fprintf(stderr, "child FocusOut routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root) {
        fprintf(stderr, "root FocusIn routing failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

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
