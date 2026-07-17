#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xtest.h>

struct one_mask {
    xcb_input_event_mask_t header;
    uint32_t bits;
};

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == NULL && xcb_connection_has_error(connection) == 0)
        return 1;
    fprintf(stderr, "%s failed with X error %u\n", operation,
            error == NULL ? 0 : error->error_code);
    free(error);
    return 0;
}

static xcb_generic_event_t *
next_event(xcb_connection_t *connection)
{
    xcb_generic_event_t *event = xcb_poll_for_event(connection);
    if (event != NULL)
        return event;
    xcb_flush(connection);
    struct pollfd descriptor = {
        .fd = xcb_get_file_descriptor(connection),
        .events = POLLIN
    };
    if (poll(&descriptor, 1, 2000) <= 0)
        return NULL;
    return xcb_poll_for_event(connection);
}

static xcb_atom_t
intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, (uint16_t) strlen(name), name), NULL);
    const xcb_atom_t result = reply == NULL ? XCB_ATOM_NONE : reply->atom;
    free(reply);
    return result;
}

int
main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    xcb_screen_iterator_t screens =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0)
        xcb_screen_next(&screens);
    xcb_screen_t *screen = screens.data;
    const xcb_query_extension_reply_t *extension = NULL;
    xcb_input_xi_query_version_reply_t *version = NULL;
    xcb_input_xi_query_device_reply_t *devices = NULL;
    xcb_input_xi_query_pointer_reply_t *pointer = NULL;
    xcb_input_xi_get_client_pointer_reply_t *client_pointer = NULL;
    xcb_input_xi_get_selected_events_reply_t *selected = NULL;
    xcb_input_xi_get_property_reply_t *property = NULL;
    xcb_input_xi_get_focus_reply_t *focus = NULL;
    xcb_input_xi_grab_device_reply_t *grab = NULL;
    xcb_input_xi_list_properties_reply_t *properties = NULL;
    xcb_generic_error_t *error = NULL;
    xcb_generic_event_t *event = NULL;
    xcb_input_device_id_t pointer_id = 0;
    xcb_input_device_id_t keyboard_id = 0;
    int xmin_topology = 0;
    xcb_atom_t property_atom = XCB_ATOM_NONE;
    const char *stage = "connection";
    int created_property = 0;
    int selected_events = 0;
    int pointer_grabbed = 0;
    int result = 0;

    if (xcb_connection_has_error(connection) || screen == NULL)
        goto cleanup;
    extension = xcb_get_extension_data(connection, &xcb_input_id);
    if (extension == NULL || !extension->present)
        goto cleanup;

    stage = "version";
    version = xcb_input_xi_query_version_reply(
        connection, xcb_input_xi_query_version(connection, 2, 4), &error);
    if (error != NULL || version == NULL || version->major_version != 2 ||
        version->minor_version < 3)
        goto cleanup;

    stage = "device topology";
    devices = xcb_input_xi_query_device_reply(
        connection,
        xcb_input_xi_query_device(connection, XCB_INPUT_DEVICE_ALL_MASTER),
        &error);
    if (error != NULL || devices == NULL || devices->num_infos < 2)
        goto cleanup;
    xcb_input_xi_device_info_iterator_t infos =
        xcb_input_xi_query_device_infos_iterator(devices);
    while (infos.rem != 0) {
        if (!infos.data->enabled || infos.data->num_classes == 0)
            goto cleanup;
        if (infos.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_POINTER &&
            pointer_id == 0) {
            pointer_id = infos.data->deviceid;
            const int name_length =
                xcb_input_xi_device_info_name_length(infos.data);
            xmin_topology = name_length >= 4 &&
                memcmp(xcb_input_xi_device_info_name(infos.data),
                       "Xmin", 4) == 0;
        }
        if (infos.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_KEYBOARD &&
            keyboard_id == 0)
            keyboard_id = infos.data->deviceid;
        xcb_input_xi_device_info_next(&infos);
    }
    if (pointer_id == 0 || keyboard_id == 0)
        goto cleanup;

    stage = "pointer query";
    pointer = xcb_input_xi_query_pointer_reply(
        connection,
        xcb_input_xi_query_pointer(connection, screen->root, pointer_id),
        &error);
    if (error != NULL || pointer == NULL || pointer->root != screen->root ||
        !pointer->same_screen)
        goto cleanup;

    stage = "client pointer";
    if (!checked(connection,
                 xcb_input_xi_set_client_pointer_checked(
                     connection, XCB_NONE, pointer_id),
                 "XISetClientPointer"))
        goto cleanup;
    client_pointer = xcb_input_xi_get_client_pointer_reply(
        connection,
        xcb_input_xi_get_client_pointer(connection, XCB_NONE), &error);
    if (error != NULL || client_pointer == NULL || !client_pointer->set ||
        client_pointer->deviceid != pointer_id)
        goto cleanup;

    stage = "keyboard focus";
    focus = xcb_input_xi_get_focus_reply(
        connection, xcb_input_xi_get_focus(connection, keyboard_id), &error);
    if (error != NULL || focus == NULL)
        goto cleanup;
    const xcb_window_t initial_focus = focus->focus;
    if (!checked(connection,
                 xcb_input_xi_set_focus_checked(
                     connection, screen->root, XCB_CURRENT_TIME, keyboard_id),
                 "XISetFocus root") ||
        !checked(connection,
                 xcb_input_xi_set_focus_checked(
                     connection, initial_focus, XCB_CURRENT_TIME, keyboard_id),
                 "XISetFocus restore"))
        goto cleanup;

    stage = "event selection";
    struct one_mask mask = {
        .header = { .deviceid = XCB_INPUT_DEVICE_ALL_MASTER, .mask_len = 1 },
        .bits = XCB_INPUT_XI_EVENT_MASK_MOTION |
            XCB_INPUT_XI_EVENT_MASK_RAW_MOTION |
            XCB_INPUT_XI_EVENT_MASK_PROPERTY
    };
    if (!checked(connection,
                 xcb_input_xi_select_events_checked(
                     connection, screen->root, 1, &mask.header),
                 "XISelectEvents"))
        goto cleanup;
    selected_events = 1;
    selected = xcb_input_xi_get_selected_events_reply(
        connection,
        xcb_input_xi_get_selected_events(connection, screen->root), &error);
    if (error != NULL || selected == NULL || selected->num_masks != 1)
        goto cleanup;

    stage = "shared motion events";
    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
                     screen->root, 11, 13, 0),
                 "XTEST motion"))
        goto cleanup;
    int saw_motion = 0;
    int saw_raw_motion = 0;
    for (int count = 0; count < 2; ++count) {
        event = next_event(connection);
        if (event == NULL ||
            (event->response_type & 0x7fU) != XCB_GE_GENERIC)
            goto cleanup;
        xcb_ge_generic_event_t *generic = (xcb_ge_generic_event_t *) event;
        if (generic->extension != extension->major_opcode)
            goto cleanup;
        if (generic->event_type == XCB_INPUT_MOTION) {
            xcb_input_motion_event_t *motion =
                (xcb_input_motion_event_t *) event;
            if ((xmin_topology &&
                 (motion->deviceid != pointer_id ||
                  motion->sourceid != pointer_id)) ||
                motion->root != screen->root)
                goto cleanup;
            saw_motion = 1;
        }
        else if (generic->event_type == XCB_INPUT_RAW_MOTION) {
            xcb_input_raw_motion_event_t *raw =
                (xcb_input_raw_motion_event_t *) event;
            if ((xmin_topology &&
                 (raw->deviceid != pointer_id ||
                  raw->sourceid != pointer_id)) ||
                raw->deviceid == 0 || raw->sourceid == 0)
                goto cleanup;
            saw_raw_motion = 1;
        }
        free(event);
        event = NULL;
    }
    if (!saw_motion || !saw_raw_motion)
        goto cleanup;

    stage = "shared active grab";
    const uint32_t grab_mask = XCB_INPUT_XI_EVENT_MASK_MOTION;
    grab = xcb_input_xi_grab_device_reply(
        connection,
        xcb_input_xi_grab_device(
            connection, screen->root, XCB_CURRENT_TIME, XCB_NONE,
            pointer_id, 1, 1, 0, 1, &grab_mask), &error);
    if (error != NULL || grab == NULL || grab->status != 0)
        goto cleanup;
    pointer_grabbed = 1;
    if (xmin_topology) {
        if (!checked(connection,
                     xcb_test_fake_input_checked(
                         connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
                         screen->root, 17, 19, 0),
                     "XTEST grabbed motion"))
            goto cleanup;
        saw_motion = 0;
        saw_raw_motion = 0;
        for (int count = 0; count < 2; ++count) {
            event = next_event(connection);
            if (event == NULL ||
                (event->response_type & 0x7fU) != XCB_GE_GENERIC ||
                ((xcb_ge_generic_event_t *) event)->extension !=
                    extension->major_opcode)
                goto cleanup;
            const uint16_t event_type =
                ((xcb_ge_generic_event_t *) event)->event_type;
            saw_motion = saw_motion || event_type == XCB_INPUT_MOTION;
            saw_raw_motion =
                saw_raw_motion || event_type == XCB_INPUT_RAW_MOTION;
            free(event);
            event = NULL;
        }
        if (!saw_motion || !saw_raw_motion)
            goto cleanup;
    }
    if (!checked(connection,
                 xcb_input_xi_ungrab_device_checked(
                     connection, XCB_CURRENT_TIME, pointer_id),
                 "XIUngrabDevice"))
        goto cleanup;
    pointer_grabbed = 0;

    stage = "device property";
    property_atom = intern(connection, "XMIN_XI2_TEST");
    const uint32_t value = 0x12345678U;
    if (property_atom == XCB_ATOM_NONE ||
        !checked(connection,
                 xcb_input_xi_change_property_checked(
                     connection, keyboard_id, XCB_PROP_MODE_REPLACE, 32,
                     property_atom, XCB_ATOM_CARDINAL, 1, &value),
                 "XIChangeProperty"))
        goto cleanup;
    created_property = 1;
    event = next_event(connection);
    if (event == NULL ||
        (event->response_type & 0x7fU) != XCB_GE_GENERIC ||
        ((xcb_ge_generic_event_t *) event)->extension !=
            extension->major_opcode ||
        ((xcb_ge_generic_event_t *) event)->event_type != XCB_INPUT_PROPERTY ||
        ((xcb_input_property_event_t *) event)->deviceid != keyboard_id ||
        ((xcb_input_property_event_t *) event)->property != property_atom)
        goto cleanup;
    free(event);
    event = NULL;
    property = xcb_input_xi_get_property_reply(
        connection,
        xcb_input_xi_get_property(
            connection, keyboard_id, 0, property_atom,
            XCB_ATOM_CARDINAL, 0, 1), &error);
    if (error != NULL || property == NULL || property->format != 32 ||
        property->type != XCB_ATOM_CARDINAL || property->num_items != 1)
        goto cleanup;
    xcb_input_xi_get_property_items_t items;
    xcb_input_xi_get_property_items_unpack(
        xcb_input_xi_get_property_items(property), property->num_items,
        property->format, &items);
    if (items.data32 == NULL || items.data32[0] != value)
        goto cleanup;
    properties = xcb_input_xi_list_properties_reply(
        connection, xcb_input_xi_list_properties(connection, keyboard_id),
        &error);
    if (error != NULL || properties == NULL ||
        properties->num_properties == 0)
        goto cleanup;

    result = 1;

cleanup:
    if (pointer_grabbed)
        xcb_input_xi_ungrab_device(
            connection, XCB_CURRENT_TIME, pointer_id);
    if (created_property)
        xcb_input_xi_delete_property(connection, keyboard_id, property_atom);
    if (selected_events) {
        struct one_mask none = {
            .header = { .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
                        .mask_len = 1 },
            .bits = 0
        };
        xcb_input_xi_select_events(connection, screen->root, 1, &none.header);
    }
    if (!result)
        fprintf(stderr, "XI2 state test failed while checking %s\n", stage);
    free(event);
    free(property);
    free(properties);
    free(grab);
    free(focus);
    free(selected);
    free(client_pointer);
    free(pointer);
    free(devices);
    free(version);
    free(error);
    xcb_disconnect(connection);
    return result ? 0 : 1;
}
