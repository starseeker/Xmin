#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <xcb/bigreq.h>
#include <xcb/ge.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xc_misc.h>
#include <xcb/xtest.h>

static int checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
                   const char *operation);

static xcb_genericevent_query_version_reply_t *
query_generic_event_version(xcb_connection_t *connection,
                            xcb_generic_error_t **error)
{
    static xcb_extension_t extension = {
        .name = "Generic Event Extension",
        .global_id = 0
    };
    static const xcb_protocol_request_t protocol = {
        .count = 2,
        .ext = &extension,
        .opcode = XCB_GENERICEVENT_QUERY_VERSION,
        .isvoid = 0
    };
    xcb_genericevent_query_version_request_t request = {
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

static int
test_foundation_extensions(xcb_connection_t *connection)
{
    xcb_generic_error_t *error = NULL;
    xcb_genericevent_query_version_reply_t *generic =
        query_generic_event_version(connection, &error);
    xcb_xc_misc_get_version_reply_t *misc = NULL;
    xcb_big_requests_enable_reply_t *big = NULL;
    int result = 0;

    if (error != NULL || generic == NULL || generic->major_version != 1 ||
        generic->minor_version != 0)
        goto cleanup;
    misc = xcb_xc_misc_get_version_reply(
        connection, xcb_xc_misc_get_version(connection, 1, 1), &error);
    if (error != NULL || misc == NULL || misc->server_major_version != 1 ||
        misc->server_minor_version < 1)
        goto cleanup;
    big = xcb_big_requests_enable_reply(
        connection, xcb_big_requests_enable(connection), &error);
    if (error != NULL || big == NULL || big->maximum_request_length <= 65535U)
        goto cleanup;
    result = 1;

cleanup:
    free(error);
    free(big);
    free(misc);
    free(generic);
    return result;
}

static int
test_shape(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_shape_query_version_reply_t *version = xcb_shape_query_version_reply(
        connection, xcb_shape_query_version(connection), &error);
    xcb_shape_query_extents_reply_t *extents = NULL;
    xcb_window_t window = xcb_generate_id(connection);
    const xcb_rectangle_t rectangle = { 2, 3, 40, 30 };
    int result = 0;

    if (error != NULL || version == NULL || version->major_version != 1 ||
        version->minor_version != 1)
        goto cleanup;
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 64, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    if (!checked(connection,
                 xcb_shape_rectangles_checked(
                     connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                     XCB_CLIP_ORDERING_UNSORTED, window, 0, 0, 1, &rectangle),
                 "SHAPE Rectangles"))
        goto cleanup;
    extents = xcb_shape_query_extents_reply(
        connection, xcb_shape_query_extents(connection, window), &error);
    if (error != NULL || extents == NULL || !extents->bounding_shaped ||
        extents->bounding_shape_extents_x != rectangle.x ||
        extents->bounding_shape_extents_y != rectangle.y ||
        extents->bounding_shape_extents_width != rectangle.width ||
        extents->bounding_shape_extents_height != rectangle.height)
        goto cleanup;
    result = 1;

cleanup:
    xcb_destroy_window(connection, window);
    free(error);
    free(extents);
    free(version);
    return result;
}

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

static xcb_generic_event_t *
wait_event_type(xcb_connection_t *connection, uint8_t type)
{
    xcb_generic_event_t *event = poll_event_type(connection, type);
    if (event != NULL)
        return event;
    struct pollfd descriptor = {
        .fd = xcb_get_file_descriptor(connection),
        .events = POLLIN,
        .revents = 0,
    };
    int ready;
    do {
        ready = poll(&descriptor, 1, 2000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
        return NULL;
    return poll_event_type(connection, type);
}

int
main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    xcb_connection_t *observer = NULL;
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
    if (!test_foundation_extensions(connection)) {
        fprintf(stderr, "foundation extension negotiation failed\n");
        xcb_disconnect(connection);
        return 1;
    }
    if (!test_shape(connection, screen)) {
        fprintf(stderr, "SHAPE extension round trip failed\n");
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
    const uint32_t child_event_mask =
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE;
    const xcb_window_t reparent_parent = xcb_generate_id(connection);
    const xcb_window_t child = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, XCB_COPY_FROM_PARENT, reparent_parent,
                     screen->root, 50, 5, 30, 30, 0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                     XCB_CW_EVENT_MASK, &child_event_mask),
                 "create reparent destination") ||
        !checked(connection,
                 xcb_map_window_checked(connection, reparent_parent),
                 "map reparent destination") ||
        !checked(connection,
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
    event = wait_event_type(connection, XCB_KEY_RELEASE);
    xcb_key_release_event_t *repeat_release =
        (xcb_key_release_event_t *) event;
    if (repeat_release == NULL || repeat_release->detail != 96 ||
        repeat_release->event != screen->root) {
        fprintf(stderr, "key repeat release failed\n");
        free(event);
        goto cleanup;
    }
    const xcb_timestamp_t repeat_time = repeat_release->time;
    free(event);
    event = wait_event_type(connection, XCB_KEY_PRESS);
    key_event = (xcb_key_press_event_t *) event;
    if (key_event == NULL || key_event->detail != 96 ||
        key_event->event != screen->root || key_event->time != repeat_time) {
        fprintf(stderr, "key repeat press failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    keys = xcb_query_keymap_reply(
        connection, xcb_query_keymap(connection), &error);
    if (error != NULL || keys == NULL || (keys->keys[12] & 1U) == 0) {
        fprintf(stderr, "key repeat changed persistent key state\n");
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

    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "focus child for lifecycle transition")) {
        goto cleanup;
    }
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection, xcb_unmap_window_checked(connection, child),
                 "unmap focused pointer child")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "unmap lifecycle FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "unmap lifecycle FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child || leave_event->child != XCB_NONE ||
        leave_event->event_x != 7 || leave_event->event_y != 9 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "unmap lifecycle LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != screen->root || enter_event->child != XCB_NONE ||
        enter_event->root_x != 17 || enter_event->root_y != 19 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "unmap lifecycle EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection, xcb_map_window_checked(connection, child),
                 "remap pointer child")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != screen->root || leave_event->child != XCB_NONE ||
        leave_event->root_x != 17 || leave_event->root_y != 19 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "map lifecycle LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child || enter_event->child != XCB_NONE ||
        enter_event->event_x != 7 || enter_event->event_y != 9 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "map lifecycle EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    const uint32_t reparent_position[] = {5, 5};
    if (!checked(connection,
                 xcb_configure_window_checked(
                     connection, reparent_parent,
                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                     reparent_position),
                 "position reparent destination") ||
        !checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "focus child for reparent transition")) {
        goto cleanup;
    }
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection,
                 xcb_reparent_window_checked(
                     connection, child, reparent_parent, 5, 5),
                 "reparent focused pointer child")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "reparent lifecycle FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "reparent lifecycle FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR ||
        leave_event->event != child || leave_event->child != XCB_NONE ||
        leave_event->event_x != 7 || leave_event->event_y != 9 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "reparent unmap LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR ||
        enter_event->event != reparent_parent ||
        enter_event->child != XCB_NONE ||
        enter_event->event_x != 12 || enter_event->event_y != 14 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "reparent unmap EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->child != XCB_NONE ||
        leave_event->event_x != 12 || leave_event->event_y != 14 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "reparent map LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child || enter_event->child != XCB_NONE ||
        enter_event->event_x != 7 || enter_event->event_y != 9 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "reparent map EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    xcb_get_input_focus_reply_t *reparent_focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || reparent_focus == NULL ||
        reparent_focus->focus != screen->root ||
        reparent_focus->revert_to != XCB_INPUT_FOCUS_NONE) {
        fprintf(stderr, "reparent lifecycle focus state failed\n");
        free(reparent_focus);
        goto cleanup;
    }
    free(reparent_focus);

    xcb_grab_pointer_reply_t *explicit_pointer = xcb_grab_pointer_reply(
        connection,
        xcb_grab_pointer(
            connection, 0, reparent_parent,
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
            XCB_CURRENT_TIME),
        &error);
    if (error != NULL || explicit_pointer == NULL ||
        explicit_pointer->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "explicit pointer grab failed\n");
        free(explicit_pointer);
        goto cleanup;
    }
    free(explicit_pointer);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child ||
        leave_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit pointer grab LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != reparent_parent ||
        enter_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit pointer grab EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_ungrab_pointer_checked(connection, XCB_CURRENT_TIME),
                 "explicit pointer ungrab")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "explicit pointer ungrab LeaveNotify failed\n");
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
        fprintf(stderr, "explicit pointer ungrab EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    xcb_grab_keyboard_cookie_t explicit_keyboard_cookie =
        xcb_grab_keyboard(
            connection, 0, child, XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_keyboard_reply_t *explicit_keyboard =
        xcb_grab_keyboard_reply(
            connection, explicit_keyboard_cookie, &error);
    if (error != NULL || explicit_keyboard == NULL ||
        explicit_keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr,
                "explicit keyboard grab failed: error=%u status=%u "
                "connection=%d sequence=%u\n",
                error == NULL ? 0 : error->error_code,
                explicit_keyboard == NULL ? 255 : explicit_keyboard->status,
                xcb_connection_has_error(connection),
                explicit_keyboard_cookie.sequence);
        free(error);
        error = NULL;
        free(explicit_keyboard);
        goto cleanup;
    }
    free(explicit_keyboard);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit keyboard grab pointer FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit keyboard grab parent FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root ||
        focus_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit keyboard grab root FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_VIRTUAL ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit keyboard grab parent FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_GRAB) {
        fprintf(stderr, "explicit keyboard grab FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection,
                 xcb_ungrab_keyboard_checked(connection, XCB_CURRENT_TIME),
                 "explicit keyboard ungrab")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "explicit keyboard ungrab FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "explicit keyboard ungrab FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    xcb_grab_pointer_reply_t *view_loss_grab = xcb_grab_pointer_reply(
        connection,
        xcb_grab_pointer(
            connection, 0, reparent_parent,
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, child, XCB_NONE,
            XCB_CURRENT_TIME),
        &error);
    if (error != NULL || view_loss_grab == NULL ||
        view_loss_grab->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "view-loss pointer grab failed\n");
        free(view_loss_grab);
        goto cleanup;
    }
    free(view_loss_grab);
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection, xcb_unmap_window_checked(connection, child),
                 "unmap pointer confinement window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "view-loss ungrab LeaveNotify failed\n");
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
        fprintf(stderr, "view-loss ungrab EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "view-loss normal LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != reparent_parent ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "view-loss normal EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection, xcb_map_window_checked(connection, child),
                 "remap pointer confinement window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "view-loss remap LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "view-loss remap EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    xcb_grab_keyboard_reply_t *view_loss_keyboard =
        xcb_grab_keyboard_reply(
            connection,
            xcb_grab_keyboard(
                connection, 0, child, XCB_CURRENT_TIME,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
            &error);
    if (error != NULL || view_loss_keyboard == NULL ||
        view_loss_keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "view-loss keyboard grab failed\n");
        free(view_loss_keyboard);
        goto cleanup;
    }
    free(view_loss_keyboard);
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection, xcb_unmap_window_checked(connection, child),
                 "unmap keyboard grab window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "keyboard view-loss FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != screen->root ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "keyboard view-loss FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "keyboard view-loss LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != reparent_parent ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "keyboard view-loss EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection, xcb_map_window_checked(connection, child),
                 "remap keyboard grab window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "keyboard view-loss remap LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "keyboard view-loss remap EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);

    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "focus keyboard grab window")) {
        goto cleanup;
    }
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    view_loss_keyboard = xcb_grab_keyboard_reply(
        connection,
        xcb_grab_keyboard(
            connection, 0, child, XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        &error);
    if (error != NULL || view_loss_keyboard == NULL ||
        view_loss_keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "same-focus keyboard grab failed\n");
        free(view_loss_keyboard);
        goto cleanup;
    }
    free(view_loss_keyboard);
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection, xcb_unmap_window_checked(connection, child),
                 "unmap focused keyboard grab window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "same-focus ungrab FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_NONLINEAR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "same-focus ungrab FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus reversion FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus reversion FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus keyboard LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != reparent_parent ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus keyboard EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    if (!checked(connection, xcb_map_window_checked(connection, child),
                 "remap focused keyboard grab window")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != reparent_parent ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus remap LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != child ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "same-focus remap EnterNotify failed\n");
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

    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "focus child for destroy transition")) {
        goto cleanup;
    }
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    if (!checked(connection, xcb_destroy_window_checked(connection, child),
                 "destroy focused pointer child")) {
        goto cleanup;
    }
    event = poll_event_type(connection, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != child ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "destroy lifecycle FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_NORMAL) {
        fprintf(stderr, "destroy lifecycle FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        leave_event->event != child || leave_event->child != XCB_NONE ||
        leave_event->event_x != 7 || leave_event->event_y != 9 ||
        leave_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        leave_event->same_screen_focus != 3) {
        fprintf(stderr, "destroy lifecycle LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = poll_event_type(connection, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        enter_event->event != reparent_parent ||
        enter_event->child != XCB_NONE ||
        enter_event->event_x != 12 || enter_event->event_y != 14 ||
        enter_event->mode != XCB_NOTIFY_MODE_NORMAL ||
        enter_event->same_screen_focus != 3) {
        fprintf(stderr, "destroy lifecycle EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    xcb_get_input_focus_reply_t *destroy_focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || destroy_focus == NULL ||
        destroy_focus->focus != reparent_parent ||
        destroy_focus->revert_to != XCB_INPUT_FOCUS_NONE) {
        fprintf(stderr, "destroy lifecycle focus state failed\n");
        free(destroy_focus);
        goto cleanup;
    }
    free(destroy_focus);

    int observer_screen_number = 0;
    observer = xcb_connect(NULL, &observer_screen_number);
    if (observer == NULL || xcb_connection_has_error(observer) != 0) {
        fprintf(stderr, "unable to connect disconnect observer\n");
        goto cleanup;
    }
    const xcb_setup_t *observer_setup = xcb_get_setup(observer);
    xcb_screen_iterator_t observer_screens =
        xcb_setup_roots_iterator(observer_setup);
    while (observer_screen_number-- > 0)
        xcb_screen_next(&observer_screens);
    if (observer_screens.data == NULL ||
        observer_screens.data->root != screen->root) {
        fprintf(stderr, "disconnect observer selected the wrong screen\n");
        goto cleanup;
    }
    const uint32_t observer_mask =
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
        XCB_EVENT_MASK_FOCUS_CHANGE;
    if (!checked(observer,
                 xcb_change_window_attributes_checked(
                     observer, screen->root, XCB_CW_EVENT_MASK,
                     &observer_mask),
                 "select observer root events") ||
        !checked(observer,
                 xcb_change_window_attributes_checked(
                     observer, reparent_parent, XCB_CW_EVENT_MASK,
                     &observer_mask),
                 "select observer child events")) {
        goto cleanup;
    }
    xcb_grab_pointer_reply_t *disconnect_pointer =
        xcb_grab_pointer_reply(
            connection,
            xcb_grab_pointer(
                connection, 0, screen->root,
                XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
            &error);
    if (error != NULL || disconnect_pointer == NULL ||
        disconnect_pointer->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "disconnect pointer grab failed\n");
        free(disconnect_pointer);
        goto cleanup;
    }
    free(disconnect_pointer);
    xcb_grab_keyboard_reply_t *disconnect_keyboard =
        xcb_grab_keyboard_reply(
            connection,
            xcb_grab_keyboard(
                connection, 0, screen->root, XCB_CURRENT_TIME,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
            &error);
    if (error != NULL || disconnect_keyboard == NULL ||
        disconnect_keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "disconnect keyboard grab failed\n");
        free(disconnect_keyboard);
        goto cleanup;
    }
    free(disconnect_keyboard);
    while ((event = xcb_poll_for_event(connection)) != NULL)
        free(event);
    while ((event = xcb_poll_for_event(observer)) != NULL)
        free(event);

    const xcb_window_t root = screen->root;
    xcb_disconnect(connection);
    connection = NULL;
    event = wait_event_type(observer, XCB_LEAVE_NOTIFY);
    leave_event = (xcb_leave_notify_event_t *) event;
    if (leave_event == NULL ||
        leave_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        leave_event->event != root ||
        leave_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "disconnect pointer LeaveNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = wait_event_type(observer, XCB_ENTER_NOTIFY);
    enter_event = (xcb_enter_notify_event_t *) event;
    if (enter_event == NULL ||
        enter_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        enter_event->event != reparent_parent ||
        enter_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "disconnect pointer EnterNotify failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = wait_event_type(observer, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_POINTER ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "disconnect keyboard pointer FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = wait_event_type(observer, XCB_FOCUS_OUT);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_INFERIOR ||
        focus_event->event != root ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "disconnect keyboard root FocusOut failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    event = wait_event_type(observer, XCB_FOCUS_IN);
    focus_event = (xcb_focus_out_event_t *) event;
    if (focus_event == NULL ||
        focus_event->detail != XCB_NOTIFY_DETAIL_ANCESTOR ||
        focus_event->event != reparent_parent ||
        focus_event->mode != XCB_NOTIFY_MODE_UNGRAB) {
        fprintf(stderr, "disconnect keyboard FocusIn failed\n");
        free(event);
        goto cleanup;
    }
    free(event);
    passed = 1;

cleanup:
    free(error);
    free(version);
    if (connection != NULL)
        xcb_disconnect(connection);
    if (observer != NULL)
        xcb_disconnect(observer);
    return passed ? 0 : 1;
}
