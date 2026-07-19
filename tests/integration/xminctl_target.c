#include "../../src/control/xmin_keysyms.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

static xcb_atom_t
intern_atom(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, (uint16_t) strlen(name), name), NULL);
    xcb_atom_t atom = reply == NULL ? XCB_ATOM_NONE : reply->atom;

    free(reply);
    return atom;
}

static xcb_keycode_t
find_keycode(xcb_connection_t *connection, xcb_keysym_t symbol)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    uint8_t count = (uint8_t) (setup->max_keycode - setup->min_keycode + 1);
    xcb_get_keyboard_mapping_reply_t *reply =
        xcb_get_keyboard_mapping_reply(
            connection,
            xcb_get_keyboard_mapping(connection, setup->min_keycode, count),
            NULL);
    xcb_keycode_t keycode = XCB_NO_SYMBOL;

    if (reply != NULL) {
        xcb_keysym_t *symbols = xcb_get_keyboard_mapping_keysyms(reply);
        unsigned int code;

        for (code = 0; code < count; ++code) {
            size_t offset = (size_t) code * reply->keysyms_per_keycode;

            if (symbols[offset] == symbol) {
                keycode = (xcb_keycode_t) (setup->min_keycode + code);
                break;
            }
        }
    }
    free(reply);
    return keycode;
}

static int
fill_window(xcb_connection_t *connection, xcb_window_t window,
            xcb_gcontext_t graphics, uint32_t pixel)
{
    const xcb_rectangle_t rectangle = { 0, 0, 80, 60 };
    xcb_generic_error_t *error;

    xcb_change_gc(connection, graphics, XCB_GC_FOREGROUND, &pixel);
    error = xcb_request_check(
        connection,
        xcb_poly_fill_rectangle_checked(connection, window, graphics,
                                        1, &rectangle));
    if (error != NULL) {
        free(error);
        return -1;
    }
    return xcb_flush(connection) > 0 ? 0 : -1;
}

int
main(int argc, char **argv)
{
    static const char automation_title[] = "xminctl-automation-target";
    static const char feedback_title[] = "xmin-viewer-feedback-target";
    static const uint32_t feedback_colors[] = {
        0x00ff0000U, 0x0000ffffU, 0x0000ff00U, 0x00ff00ffU,
        0x000000ffU, 0x00ffff00U, 0x00ffffffU, 0x00804020U,
        0x00208040U, 0x00402080U, 0x00c06020U, 0x0020c060U,
    };
    const char *title;
    size_t title_length;
    int feedback_mode;
    int screen_number = 0;
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_gcontext_t graphics;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_keycode_t a_keycode;
    uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE |
                          XCB_EVENT_MASK_BUTTON_PRESS |
                          XCB_EVENT_MASK_POINTER_MOTION |
                          XCB_EVENT_MASK_KEY_PRESS |
                          XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    uint32_t create_values[] = { 0x000000ffU, event_mask };
    uint32_t graphics_values[] = { 0x000000ffU };
    xcb_generic_error_t *create_error;
    int got_button = 0;
    int got_drag = 0;
    int got_key = 0;
    int painted_success = 0;
    int result = 1;
    size_t feedback_color = 0;

    if (argc == 1)
        feedback_mode = 0;
    else if (argc == 2 && strcmp(argv[1], "--feedback") == 0)
        feedback_mode = 1;
    else {
        fprintf(stderr, "usage: %s [--feedback]\n", argv[0]);
        return 2;
    }
    title = feedback_mode ? feedback_title : automation_title;
    title_length = strlen(title);

    connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection) != 0) {
        fprintf(stderr, "target: cannot connect to the X server\n");
        if (connection != NULL)
            xcb_disconnect(connection);
        return 2;
    }
    xcb_screen_iterator_t screens =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0 && screens.rem != 0)
        xcb_screen_next(&screens);
    if (screens.rem == 0) {
        xcb_disconnect(connection);
        return 2;
    }
    screen = screens.data;
    wm_protocols = intern_atom(connection, "WM_PROTOCOLS");
    wm_delete_window = intern_atom(connection, "WM_DELETE_WINDOW");
    a_keycode = find_keycode(connection, XK_a);
    if (wm_protocols == XCB_ATOM_NONE || wm_delete_window == XCB_ATOM_NONE ||
        a_keycode == XCB_NO_SYMBOL)
        goto cleanup;

    window = xcb_generate_id(connection);
    graphics = xcb_generate_id(connection);
    create_error = xcb_request_check(
        connection,
        xcb_create_window_checked(
            connection, screen->root_depth, window,
            screen->root, 20, 30, 80, 60, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
            XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, create_values));
    if (create_error != NULL) {
        free(create_error);
        goto cleanup;
    }
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        (uint32_t) title_length, title);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        wm_protocols, XCB_ATOM_ATOM, 32, 1,
                        &wm_delete_window);
    xcb_create_gc(connection, graphics, window, XCB_GC_FOREGROUND,
                  graphics_values);
    xcb_map_window(connection, window);
    if (fill_window(connection, window, graphics, 0x000000ffU) != 0)
        goto cleanup_window;

    for (;;) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        uint8_t type;

        if (event == NULL)
            goto cleanup_window;
        type = event->response_type & 0x7f;
        if (type == XCB_EXPOSE)
            (void) fill_window(connection, window, graphics,
                               feedback_mode ?
                               feedback_colors[feedback_color] :
                               (painted_success ? 0x0000ff00U : 0x000000ffU));
        else if (type == XCB_BUTTON_PRESS) {
            xcb_button_press_event_t *button = (xcb_button_press_event_t *) event;

            if (button->detail == 1 && button->event_x == 20 &&
                button->event_y == 20 &&
                (button->state & XCB_MOD_MASK_CONTROL) != 0)
                got_button = 1;
        }
        else if (type == XCB_MOTION_NOTIFY) {
            xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *) event;

            if (motion->event_x == 30 && motion->event_y == 30 &&
                (motion->state & XCB_MOD_MASK_CONTROL) != 0 &&
                (motion->state & XCB_BUTTON_MASK_1) != 0)
                got_drag = 1;
        }
        else if (type == XCB_KEY_PRESS) {
            xcb_key_press_event_t *key = (xcb_key_press_event_t *) event;

            if (key->detail == a_keycode)
                got_key = 1;
            if (feedback_mode) {
                feedback_color = (feedback_color + 1) %
                    (sizeof(feedback_colors) / sizeof(feedback_colors[0]));
                if (fill_window(connection, window, graphics,
                                feedback_colors[feedback_color]) != 0) {
                    free(event);
                    goto cleanup_window;
                }
            }
        }
        else if (type == XCB_CLIENT_MESSAGE) {
            xcb_client_message_event_t *message =
                (xcb_client_message_event_t *) event;

            if (message->type == wm_protocols && message->format == 32 &&
                message->data.data32[0] == wm_delete_window) {
                free(event);
                result = feedback_mode ||
                    (got_button && got_drag && got_key && painted_success)
                    ? 0 : 1;
                break;
            }
        }
        if (!feedback_mode && got_button && got_drag && got_key &&
            !painted_success) {
            painted_success =
                fill_window(connection, window, graphics, 0x0000ff00U) == 0;
        }
        free(event);
    }

cleanup_window:
    xcb_free_gc(connection, graphics);
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
cleanup:
    xcb_disconnect(connection);
    return result;
}
