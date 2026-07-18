#include <xcb/xcb.h>

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);

    if (error == NULL)
        return 1;
    fprintf(stderr, "%s failed with X11 error %u\n", operation,
            error->error_code);
    free(error);
    return 0;
}

static int
checked_error(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              uint8_t expected, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);

    if (error != NULL && error->error_code == expected) {
        free(error);
        return 1;
    }
    fprintf(stderr, "%s returned X11 error %u instead of %u\n", operation,
            error == NULL ? 0 : error->error_code, expected);
    free(error);
    return 0;
}

static int
atom_is_listed(const xcb_list_properties_reply_t *properties, xcb_atom_t atom)
{
    xcb_atom_t *atoms = xcb_list_properties_atoms(properties);
    int count = xcb_list_properties_atoms_length(properties);
    int i;

    for (i = 0; i < count; ++i) {
        if (atoms[i] == atom)
            return 1;
    }
    return 0;
}

static int
child_is_listed(const xcb_query_tree_reply_t *tree, xcb_window_t child)
{
    xcb_window_t *children = xcb_query_tree_children(tree);
    int count = xcb_query_tree_children_length(tree);
    int i;

    for (i = 0; i < count; ++i) {
        if (children[i] == child)
            return 1;
    }
    return 0;
}

static int
wait_for_client_message(xcb_connection_t *connection, xcb_window_t window,
                        xcb_atom_t type, uint32_t value)
{
    int attempt;

    xcb_flush(connection);
    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            uint8_t response_type = event->response_type;

            if ((response_type & 0x7fU) == XCB_CLIENT_MESSAGE) {
                xcb_client_message_event_t *message =
                    (xcb_client_message_event_t *) event;

                if ((response_type & 0x80U) != 0 &&
                    message->format == 32 && message->window == window &&
                    message->type == type && message->data.data32[0] == value) {
                    free(event);
                    return 1;
                }
            }
            free(event);
        }
        {
            struct pollfd ready = {
                .fd = xcb_get_file_descriptor(connection),
                .events = POLLIN
            };

            (void) poll(&ready, 1, 100);
        }
    }
    return 0;
}

int
main(void)
{
    static const char property_name[] = "_XMIN_CORE_PROPERTY";
    static const char selection_name[] = "_XMIN_CORE_SELECTION";
    static const char message_name[] = "_XMIN_CORE_MESSAGE";
    static const char color_name[] = "red";
    const uint32_t property_values[] = { 0x584d494eU, 0x434f5245U };
    xcb_connection_t *connection = NULL;
    xcb_connection_t *second_connection = NULL;
    xcb_screen_iterator_t screens;
    xcb_screen_t *screen;
    xcb_generic_error_t *error = NULL;
    xcb_intern_atom_reply_t *property_atom = NULL;
    xcb_intern_atom_reply_t *selection_atom = NULL;
    xcb_intern_atom_reply_t *message_atom = NULL;
    xcb_get_property_reply_t *property = NULL;
    xcb_get_property_reply_t *rotated_property = NULL;
    xcb_list_properties_reply_t *properties = NULL;
    xcb_query_tree_reply_t *tree = NULL;
    xcb_get_geometry_reply_t *geometry = NULL;
    xcb_get_window_attributes_reply_t *attributes = NULL;
    xcb_translate_coordinates_reply_t *translated = NULL;
    xcb_get_selection_owner_reply_t *owner = NULL;
    xcb_alloc_named_color_reply_t *named_color = NULL;
    xcb_alloc_color_reply_t *allocated_color = NULL;
    xcb_lookup_color_reply_t *looked_up_color = NULL;
    xcb_list_installed_colormaps_reply_t *installed_colormaps = NULL;
    xcb_query_colors_reply_t *queried_color = NULL;
    xcb_get_image_reply_t *image = NULL;
    xcb_query_best_size_reply_t *best_size = NULL;
    xcb_query_pointer_reply_t *pointer = NULL;
    xcb_get_motion_events_reply_t *motion = NULL;
    xcb_query_keymap_reply_t *keymap = NULL;
    xcb_get_keyboard_mapping_reply_t *keyboard_mapping = NULL;
    xcb_get_keyboard_control_reply_t *keyboard_control = NULL;
    xcb_get_pointer_control_reply_t *pointer_control = NULL;
    xcb_get_pointer_mapping_reply_t *pointer_mapping = NULL;
    xcb_get_modifier_mapping_reply_t *modifier_mapping = NULL;
    xcb_set_pointer_mapping_reply_t *set_pointer_mapping = NULL;
    xcb_set_modifier_mapping_reply_t *set_modifier_mapping = NULL;
    xcb_get_input_focus_reply_t *focus = NULL;
    xcb_grab_pointer_reply_t *pointer_grab = NULL;
    xcb_grab_keyboard_reply_t *keyboard_grab = NULL;
    xcb_window_t parent = XCB_NONE;
    xcb_window_t child = XCB_NONE;
    xcb_pixmap_t pixmap = XCB_NONE;
    xcb_pixmap_t bitmap = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    xcb_gcontext_t bitmap_graphics = XCB_NONE;
    xcb_colormap_t colormap = XCB_NONE;
    xcb_colormap_t copied_colormap = XCB_NONE;
    uint32_t pixel = 0;
    uint32_t event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_PROPERTY_CHANGE;
    int screen_number = 0;
    const char *stage = "connecting";
    int selection_owned = 0;
    int property_created = 0;
    int message_property_created = 0;
    int result = 1;

    connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection))
        goto cleanup;
    screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0 && screens.rem != 0)
        xcb_screen_next(&screens);
    if (screens.rem == 0)
        goto cleanup;
    screen = screens.data;

    stage = "interning atoms";
    property_atom = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, sizeof(property_name) - 1,
                        property_name),
        &error);
    selection_atom = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, sizeof(selection_name) - 1,
                        selection_name),
        &error);
    message_atom = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, sizeof(message_name) - 1, message_name),
        &error);
    if (error != NULL || property_atom == NULL || selection_atom == NULL ||
        message_atom == NULL || property_atom->atom == XCB_NONE ||
        selection_atom->atom == XCB_NONE || message_atom->atom == XCB_NONE)
        goto cleanup;

    stage = "round-tripping a root property";
    if (!checked(connection,
                 xcb_change_property_checked(
                     connection, XCB_PROP_MODE_REPLACE, screen->root,
                     property_atom->atom, XCB_ATOM_INTEGER, 32, 2,
                     property_values),
                 "ChangeProperty"))
        goto cleanup;
    property_created = 1;
    property = xcb_get_property_reply(
        connection,
        xcb_get_property(connection, 0, screen->root, property_atom->atom,
                         XCB_ATOM_INTEGER, 0, 2),
        &error);
    properties = xcb_list_properties_reply(
        connection, xcb_list_properties(connection, screen->root), &error);
    if (error != NULL || property == NULL || property->type != XCB_ATOM_INTEGER ||
        property->format != 32 || property->value_len != 2 ||
        xcb_get_property_value_length(property) !=
            (int) sizeof(property_values) ||
        memcmp(xcb_get_property_value(property), property_values,
               sizeof(property_values)) != 0 ||
        properties == NULL ||
        !atom_is_listed(properties, property_atom->atom))
        goto cleanup;

    stage = "rotating property values";
    {
        const uint32_t rotated_value = 0x524f5441U;
        const xcb_atom_t atoms[] = {
            property_atom->atom, message_atom->atom
        };

        if (!checked(connection,
                     xcb_change_property_checked(
                         connection, XCB_PROP_MODE_REPLACE, screen->root,
                         message_atom->atom, XCB_ATOM_INTEGER, 32, 1,
                         &rotated_value),
                     "ChangeProperty before RotateProperties")) {
            goto cleanup;
        }
        message_property_created = 1;
        {
            const xcb_atom_t duplicates[] = {
                property_atom->atom, property_atom->atom
            };
            const xcb_atom_t missing[] = {
                property_atom->atom, selection_atom->atom
            };

            if (!checked_error(
                    connection,
                    xcb_rotate_properties_checked(
                        connection, screen->root, 2, 1, duplicates),
                    XCB_MATCH, "RotateProperties duplicate validation") ||
                !checked_error(
                    connection,
                    xcb_rotate_properties_checked(
                        connection, screen->root, 2, 1, missing),
                    XCB_MATCH, "RotateProperties missing validation")) {
                goto cleanup;
            }
        }
        if (!checked(connection,
                     xcb_rotate_properties_checked(
                         connection, screen->root, 2, 1, atoms),
                     "RotateProperties")) {
            goto cleanup;
        }
        free(property);
        property = xcb_get_property_reply(
            connection,
            xcb_get_property(connection, 0, screen->root,
                             property_atom->atom, XCB_ATOM_INTEGER, 0, 2),
            &error);
        rotated_property = xcb_get_property_reply(
            connection,
            xcb_get_property(connection, 0, screen->root,
                             message_atom->atom, XCB_ATOM_INTEGER, 0, 2),
            &error);
        if (error != NULL || property == NULL ||
            property->type != XCB_ATOM_INTEGER || property->format != 32 ||
            property->value_len != 1 ||
            memcmp(xcb_get_property_value(property), &rotated_value,
                   sizeof(rotated_value)) != 0 || rotated_property == NULL ||
            rotated_property->type != XCB_ATOM_INTEGER ||
            rotated_property->format != 32 ||
            rotated_property->value_len != 2 ||
            memcmp(xcb_get_property_value(rotated_property), property_values,
                   sizeof(property_values)) != 0) {
            goto cleanup;
        }
        if (!checked(connection,
                     xcb_rotate_properties_checked(
                         connection, screen->root, 2, -1, atoms),
                     "RotateProperties with negative delta")) {
            goto cleanup;
        }
        free(property);
        property = xcb_get_property_reply(
            connection,
            xcb_get_property(connection, 0, screen->root,
                             property_atom->atom, XCB_ATOM_INTEGER, 0, 2),
            &error);
        free(rotated_property);
        rotated_property = xcb_get_property_reply(
            connection,
            xcb_get_property(connection, 0, screen->root,
                             message_atom->atom, XCB_ATOM_INTEGER, 0, 2),
            &error);
        if (error != NULL || property == NULL || property->value_len != 2 ||
            memcmp(xcb_get_property_value(property), property_values,
                   sizeof(property_values)) != 0 || rotated_property == NULL ||
            rotated_property->value_len != 1 ||
            memcmp(xcb_get_property_value(rotated_property), &rotated_value,
                   sizeof(rotated_value)) != 0) {
            goto cleanup;
        }
    }
    if (!checked(connection,
                 xcb_delete_property_checked(connection, screen->root,
                                             property_atom->atom),
                 "DeleteProperty") ||
        !checked(connection,
                 xcb_delete_property_checked(connection, screen->root,
                                             message_atom->atom),
                 "Delete rotated property"))
        goto cleanup;
    property_created = 0;
    message_property_created = 0;

    stage = "creating and configuring a window hierarchy";
    parent = xcb_generate_id(connection);
    child = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, parent, screen->root,
                     2, 3, 60, 50, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, XCB_CW_EVENT_MASK, &event_mask),
                 "Create parent window") ||
        !checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, child, parent, 1, 2,
                     10, 8, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, XCB_CW_EVENT_MASK, &event_mask),
                 "Create child window") ||
        !checked(connection, xcb_map_window_checked(connection, parent),
                 "Map parent window") ||
        !checked(connection, xcb_map_window_checked(connection, child),
                 "Map child window"))
        goto cleanup;
    tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, parent),
                                &error);
    attributes = xcb_get_window_attributes_reply(
        connection, xcb_get_window_attributes(connection, child), &error);
    if (error != NULL || tree == NULL || tree->parent != screen->root ||
        !child_is_listed(tree, child) || attributes == NULL ||
        attributes->map_state != XCB_MAP_STATE_VIEWABLE)
        goto cleanup;
    {
        const uint32_t configuration[] = { 5, 6, 12, 9 };

        if (!checked(connection,
                     xcb_configure_window_checked(
                         connection, child,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT,
                         configuration),
                     "ConfigureWindow"))
            goto cleanup;
    }
    geometry = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, child), &error);
    translated = xcb_translate_coordinates_reply(
        connection,
        xcb_translate_coordinates(connection, child, screen->root, 0, 0),
        &error);
    if (error != NULL || geometry == NULL || geometry->root != screen->root ||
        geometry->x != 5 || geometry->y != 6 || geometry->width != 12 ||
        geometry->height != 9 || translated == NULL ||
        translated->dst_x != 7 || translated->dst_y != 9)
        goto cleanup;

    stage = "querying the preferred tile size";
    best_size = xcb_query_best_size_reply(
        connection,
        xcb_query_best_size(connection, XCB_QUERY_SHAPE_OF_FASTEST_TILE,
                            child, 13, 7),
        &error);
    if (error != NULL || best_size == NULL || best_size->width != 16 ||
        best_size->height != 7)
        goto cleanup;
    free(best_size);
    best_size = xcb_query_best_size_reply(
        connection,
        xcb_query_best_size(connection, XCB_QUERY_SHAPE_OF_LARGEST_CURSOR,
                            screen->root, UINT16_MAX, UINT16_MAX),
        &error);
    if (error != NULL || best_size == NULL ||
        best_size->width != screen->width_in_pixels ||
        best_size->height != screen->height_in_pixels) {
        goto cleanup;
    }

    stage = "querying the initial input snapshot";
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL || !pointer->same_screen ||
        pointer->root != screen->root || pointer->child != parent ||
        pointer->root_x != screen->width_in_pixels / 2 ||
        pointer->root_y != screen->height_in_pixels / 2 ||
        pointer->win_x != pointer->root_x ||
        pointer->win_y != pointer->root_y || pointer->mask != 0) {
        goto cleanup;
    }
    free(pointer);
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, child), &error);
    motion = xcb_get_motion_events_reply(
        connection,
        xcb_get_motion_events(connection, screen->root, XCB_CURRENT_TIME,
                              XCB_CURRENT_TIME),
        &error);
    keymap = xcb_query_keymap_reply(
        connection, xcb_query_keymap(connection), &error);
    if (error != NULL || pointer == NULL || !pointer->same_screen ||
        pointer->root != screen->root || pointer->child != XCB_NONE ||
        pointer->root_x != screen->width_in_pixels / 2 ||
        pointer->root_y != screen->height_in_pixels / 2 ||
        pointer->win_x != pointer->root_x - translated->dst_x ||
        pointer->win_y != pointer->root_y - translated->dst_y ||
        pointer->mask != 0 || motion == NULL ||
        xcb_get_motion_events_events_length(motion) != 0 || keymap == NULL) {
        goto cleanup;
    }
    {
        static const uint8_t clear_keymap[32] = { 0 };

        if (memcmp(keymap->keys, clear_keymap, sizeof(clear_keymap)) != 0)
            goto cleanup;
    }

    stage = "querying fixed core input maps and controls";
    keyboard_mapping = xcb_get_keyboard_mapping_reply(
        connection, xcb_get_keyboard_mapping(connection, 96, 1), &error);
    keyboard_control = xcb_get_keyboard_control_reply(
        connection, xcb_get_keyboard_control(connection), &error);
    pointer_control = xcb_get_pointer_control_reply(
        connection, xcb_get_pointer_control(connection), &error);
    pointer_mapping = xcb_get_pointer_mapping_reply(
        connection, xcb_get_pointer_mapping(connection), &error);
    modifier_mapping = xcb_get_modifier_mapping_reply(
        connection, xcb_get_modifier_mapping(connection), &error);
    {
        static const uint8_t expected_modifiers[32] = {
            50, 62, 0, 0, 66, 0, 0, 0, 37, 105, 0, 0, 64, 108, 204, 205,
            77, 0, 0, 0, 203, 0, 0, 0, 133, 134, 206, 0, 92, 0, 0, 0
        };
        uint8_t *pointer_buttons = pointer_mapping == NULL
            ? NULL
            : xcb_get_pointer_mapping_map(pointer_mapping);

        if (error != NULL || keyboard_mapping == NULL ||
            keyboard_mapping->keysyms_per_keycode != 7 ||
            xcb_get_keyboard_mapping_keysyms_length(keyboard_mapping) != 7 ||
            xcb_get_keyboard_mapping_keysyms(keyboard_mapping)[0] !=
                0x0000ffc9U ||
            keyboard_control == NULL ||
            keyboard_control->global_auto_repeat != 1 ||
            keyboard_control->led_mask != 0 ||
            keyboard_control->key_click_percent != 0 ||
            keyboard_control->bell_percent != 50 ||
            keyboard_control->bell_pitch != 400 ||
            keyboard_control->bell_duration != 100 ||
            pointer_control == NULL ||
            pointer_control->acceleration_numerator != 2 ||
            pointer_control->acceleration_denominator != 1 ||
            pointer_control->threshold != 4 || pointer_mapping == NULL ||
            xcb_get_pointer_mapping_map_length(pointer_mapping) != 10 ||
            pointer_buttons == NULL || pointer_buttons[0] != 1 ||
            pointer_buttons[9] != 10 || modifier_mapping == NULL ||
            modifier_mapping->keycodes_per_modifier != 4 ||
            xcb_get_modifier_mapping_keycodes_length(modifier_mapping) != 32 ||
            memcmp(xcb_get_modifier_mapping_keycodes(modifier_mapping),
                   expected_modifiers, sizeof(expected_modifiers)) != 0) {
            goto cleanup;
        }
    }

    stage = "mutating typed core input maps and controls";
    {
        static const xcb_keysym_t narrow_symbols[2] = {
            0x00000078U, 0x00000058U
        };
        static const xcb_keysym_t wide_symbols[8] = {
            0x00000031U, 0x00000021U, 0x00000032U, 0x00000040U,
            0x00000033U, 0x00000023U, 0x00000034U, 0x00000024U
        };
        static const xcb_keysym_t original_symbols[7] = {
            0x0000ffc9U, 0, 0x0000ffc9U, 0, 0, 0, 0
        };
        uint32_t keyboard_values[2] = { 23, 17 };
        uint32_t repeat_values[2] = { 96, XCB_AUTO_REPEAT_MODE_OFF };
        uint8_t swapped_buttons[10] = { 2, 1, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint8_t default_buttons[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint8_t duplicate_buttons[10] = { 1, 1, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint8_t empty_modifier = 0;
        static const uint8_t expected_modifiers[32] = {
            50, 62, 0, 0, 66, 0, 0, 0, 37, 105, 0, 0, 64, 108, 204, 205,
            77, 0, 0, 0, 203, 0, 0, 0, 133, 134, 206, 0, 92, 0, 0, 0
        };

        stage = "rejecting an invalid core keyboard map";
        if (!checked_error(
                connection,
                xcb_change_keyboard_mapping_checked(
                    connection, 1, 96, 0, original_symbols),
                XCB_VALUE, "ChangeKeyboardMapping zero width") ||
            !checked(connection,
                     xcb_change_keyboard_mapping_checked(
                         connection, 1, 96, 2, narrow_symbols),
                     "ChangeKeyboardMapping narrow")) {
            goto cleanup;
        }
        stage = "querying a narrow core keyboard map";
        free(keyboard_mapping);
        keyboard_mapping = xcb_get_keyboard_mapping_reply(
            connection, xcb_get_keyboard_mapping(connection, 96, 1), &error);
        if (error != NULL || keyboard_mapping == NULL ||
            keyboard_mapping->keysyms_per_keycode != 7 ||
            xcb_get_keyboard_mapping_keysyms(keyboard_mapping)[0] !=
                narrow_symbols[0] ||
            xcb_get_keyboard_mapping_keysyms(keyboard_mapping)[1] !=
                narrow_symbols[1] ||
            xcb_get_keyboard_mapping_keysyms(keyboard_mapping)[2] !=
                narrow_symbols[0] ||
            xcb_get_keyboard_mapping_keysyms(keyboard_mapping)[3] !=
                narrow_symbols[1]) {
            goto cleanup;
        }
        stage = "expanding a core keyboard map";
        if (!checked(connection,
                     xcb_change_keyboard_mapping_checked(
                         connection, 1, 96, 8, wide_symbols),
                     "ChangeKeyboardMapping wide")) {
            goto cleanup;
        }
        free(keyboard_mapping);
        keyboard_mapping = xcb_get_keyboard_mapping_reply(
            connection, xcb_get_keyboard_mapping(connection, 96, 1), &error);
        if (error != NULL || keyboard_mapping == NULL ||
            keyboard_mapping->keysyms_per_keycode < 8 ||
            memcmp(xcb_get_keyboard_mapping_keysyms(keyboard_mapping),
                   wide_symbols, sizeof(wide_symbols)) != 0 ||
            !checked(connection,
                     xcb_change_keyboard_mapping_checked(
                         connection, 1, 96, 7, original_symbols),
                     "Restore keyboard mapping")) {
            goto cleanup;
        }

        stage = "mutating typed core input maps and controls";
        if (!checked(connection,
                     xcb_change_keyboard_control_checked(
                         connection,
                         XCB_KB_KEY_CLICK_PERCENT | XCB_KB_BELL_PERCENT,
                         keyboard_values),
                     "ChangeKeyboardControl feedback") ||
            !checked(connection,
                     xcb_change_keyboard_control_checked(
                         connection, XCB_KB_KEY | XCB_KB_AUTO_REPEAT_MODE,
                         repeat_values),
                     "ChangeKeyboardControl repeat")) {
            goto cleanup;
        }
        free(keyboard_control);
        keyboard_control = xcb_get_keyboard_control_reply(
            connection, xcb_get_keyboard_control(connection), &error);
        if (error != NULL || keyboard_control == NULL ||
            keyboard_control->key_click_percent != 23 ||
            keyboard_control->bell_percent != 17 ||
            (keyboard_control->auto_repeats[12] & 1U) != 0 ||
            !checked_error(
                connection,
                xcb_change_pointer_control_checked(
                    connection, 3, 0, 7, 1, 1),
                XCB_VALUE, "ChangePointerControl invalid denominator") ||
            !checked(connection,
                     xcb_change_pointer_control_checked(
                         connection, 3, 2, 7, 1, 1),
                     "ChangePointerControl")) {
            goto cleanup;
        }
        free(pointer_control);
        pointer_control = xcb_get_pointer_control_reply(
            connection, xcb_get_pointer_control(connection), &error);
        if (error != NULL || pointer_control == NULL ||
            pointer_control->acceleration_numerator != 3 ||
            pointer_control->acceleration_denominator != 2 ||
            pointer_control->threshold != 7 ||
            !checked(connection, xcb_bell_checked(connection, 25), "Bell") ||
            !checked_error(connection, xcb_bell_checked(connection, 101),
                           XCB_VALUE, "Bell invalid percentage")) {
            goto cleanup;
        }

        set_pointer_mapping = xcb_set_pointer_mapping_reply(
            connection,
            xcb_set_pointer_mapping(connection, 10, swapped_buttons),
            &error);
        if (error != NULL || set_pointer_mapping == NULL ||
            set_pointer_mapping->status != XCB_MAPPING_STATUS_SUCCESS)
            goto cleanup;
        free(set_pointer_mapping);
        set_pointer_mapping = NULL;
        free(pointer_mapping);
        pointer_mapping = xcb_get_pointer_mapping_reply(
            connection, xcb_get_pointer_mapping(connection), &error);
        if (error != NULL || pointer_mapping == NULL ||
            xcb_get_pointer_mapping_map(pointer_mapping)[0] != 2 ||
            xcb_get_pointer_mapping_map(pointer_mapping)[1] != 1)
            goto cleanup;
        set_pointer_mapping = xcb_set_pointer_mapping_reply(
            connection,
            xcb_set_pointer_mapping(connection, 10, duplicate_buttons),
            &error);
        if (set_pointer_mapping != NULL || error == NULL ||
            error->error_code != XCB_VALUE)
            goto cleanup;
        free(error);
        error = NULL;
        set_pointer_mapping = xcb_set_pointer_mapping_reply(
            connection,
            xcb_set_pointer_mapping(connection, 10, default_buttons),
            &error);
        if (error != NULL || set_pointer_mapping == NULL ||
            set_pointer_mapping->status != XCB_MAPPING_STATUS_SUCCESS)
            goto cleanup;

        set_modifier_mapping = xcb_set_modifier_mapping_reply(
            connection,
            xcb_set_modifier_mapping(connection, 0, &empty_modifier),
            &error);
        if (error != NULL || set_modifier_mapping == NULL ||
            set_modifier_mapping->status != XCB_MAPPING_STATUS_SUCCESS)
            goto cleanup;
        free(set_modifier_mapping);
        set_modifier_mapping = NULL;
        free(modifier_mapping);
        modifier_mapping = xcb_get_modifier_mapping_reply(
            connection, xcb_get_modifier_mapping(connection), &error);
        if (error != NULL || modifier_mapping == NULL ||
            modifier_mapping->keycodes_per_modifier != 0)
            goto cleanup;
        set_modifier_mapping = xcb_set_modifier_mapping_reply(
            connection,
            xcb_set_modifier_mapping(connection, 4, expected_modifiers),
            &error);
        if (error != NULL || set_modifier_mapping == NULL ||
            set_modifier_mapping->status != XCB_MAPPING_STATUS_SUCCESS)
            goto cleanup;

        {
            xcb_generic_event_t *event;
            unsigned int keyboard_notifications = 0;
            unsigned int pointer_notifications = 0;
            unsigned int modifier_notifications = 0;

            while ((event = xcb_poll_for_event(connection)) != NULL) {
                if ((event->response_type & 0x7fU) == XCB_MAPPING_NOTIFY) {
                    xcb_mapping_notify_event_t *mapping =
                        (xcb_mapping_notify_event_t *) event;
                    if (mapping->request == XCB_MAPPING_KEYBOARD &&
                        mapping->first_keycode == 96 && mapping->count == 1)
                        ++keyboard_notifications;
                    else if (mapping->request == XCB_MAPPING_POINTER)
                        ++pointer_notifications;
                    else if (mapping->request == XCB_MAPPING_MODIFIER)
                        ++modifier_notifications;
                }
                free(event);
            }
            if (keyboard_notifications != 3 ||
                pointer_notifications != 2 ||
                modifier_notifications != 2) {
                goto cleanup;
            }
        }

        keyboard_values[0] = UINT32_MAX;
        keyboard_values[1] = UINT32_MAX;
        repeat_values[1] = XCB_AUTO_REPEAT_MODE_DEFAULT;
        if (!checked(connection,
                     xcb_change_keyboard_control_checked(
                         connection,
                         XCB_KB_KEY_CLICK_PERCENT | XCB_KB_BELL_PERCENT,
                         keyboard_values),
                     "Restore keyboard feedback") ||
            !checked(connection,
                     xcb_change_keyboard_control_checked(
                         connection, XCB_KB_KEY | XCB_KB_AUTO_REPEAT_MODE,
                         repeat_values),
                     "Restore keyboard repeat") ||
            !checked(connection,
                     xcb_change_pointer_control_checked(
                         connection, -1, -1, -1, 1, 1),
                     "Restore pointer controls")) {
            goto cleanup;
        }
    }

    stage = "warping the pointer with source constraints";
    if (!checked(connection,
                 xcb_warp_pointer_checked(
                     connection, child, screen->root, 0, 0, 0, 0, 1, 1),
                 "WarpPointer outside source window")) {
        goto cleanup;
    }
    free(pointer);
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL ||
        pointer->root_x != screen->width_in_pixels / 2 ||
        pointer->root_y != screen->height_in_pixels / 2)
        goto cleanup;
    if (!checked(connection,
                 xcb_warp_pointer_checked(
                     connection, XCB_NONE, child, 0, 0, 0, 0, 3, 4),
                 "WarpPointer into child")) {
        goto cleanup;
    }
    free(pointer);
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, child), &error);
    if (error != NULL || pointer == NULL ||
        pointer->root_x != translated->dst_x + 3 ||
        pointer->root_y != translated->dst_y + 4 ||
        pointer->win_x != 3 || pointer->win_y != 4)
        goto cleanup;
    if (!checked(connection,
                 xcb_warp_pointer_checked(
                     connection, XCB_NONE, XCB_NONE, 0, 0, 0, 0,
                     -100, -100),
                 "relative WarpPointer clamp")) {
        goto cleanup;
    }
    free(pointer);
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL || pointer->root_x != 0 ||
        pointer->root_y != 0 || pointer->child != XCB_NONE)
        goto cleanup;

    stage = "updating input focus and applying parent reversion";
    focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || focus == NULL ||
        focus->focus != XCB_INPUT_FOCUS_POINTER_ROOT ||
        focus->revert_to != XCB_INPUT_FOCUS_NONE)
        goto cleanup;
    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, child,
                     XCB_CURRENT_TIME),
                 "SetInputFocus child")) {
        goto cleanup;
    }
    free(focus);
    focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || focus == NULL || focus->focus != child ||
        focus->revert_to != XCB_INPUT_FOCUS_PARENT)
        goto cleanup;
    if (!checked(connection, xcb_unmap_window_checked(connection, child),
                 "Unmap focused child")) {
        goto cleanup;
    }
    free(focus);
    focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || focus == NULL || focus->focus != parent ||
        focus->revert_to != XCB_INPUT_FOCUS_NONE ||
        !checked_error(
            connection,
            xcb_set_input_focus_checked(
                connection, XCB_INPUT_FOCUS_NONE, child, XCB_CURRENT_TIME),
            XCB_MATCH, "SetInputFocus unviewable child") ||
        !checked(connection, xcb_map_window_checked(connection, child),
                 "Remap focused child") ||
        !checked_error(
            connection,
            xcb_set_input_focus_checked(
                connection, XCB_INPUT_FOCUS_FOLLOW_KEYBOARD, child,
                XCB_CURRENT_TIME),
            XCB_VALUE, "SetInputFocus invalid revert mode") ||
        !checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_PARENT, screen->root,
                     XCB_CURRENT_TIME),
                 "SetInputFocus actual root")) {
        goto cleanup;
    }
    free(focus);
    focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &error);
    if (error != NULL || focus == NULL || focus->focus != screen->root ||
        focus->revert_to != XCB_INPUT_FOCUS_PARENT ||
        !checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_NONE,
                     XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME),
                 "Restore pointer-root focus")) {
        goto cleanup;
    }

    stage = "coordinating active input grabs across clients";
    second_connection = xcb_connect(NULL, NULL);
    if (second_connection == NULL ||
        xcb_connection_has_error(second_connection)) {
        goto cleanup;
    }
    pointer_grab = xcb_grab_pointer_reply(
        connection,
        xcb_grab_pointer(
            connection, 0, child, XCB_EVENT_MASK_BUTTON_PRESS,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
            XCB_CURRENT_TIME),
        &error);
    if (error != NULL || pointer_grab == NULL ||
        pointer_grab->status != XCB_GRAB_STATUS_SUCCESS)
        goto cleanup;
    free(pointer_grab);
    pointer_grab = xcb_grab_pointer_reply(
        second_connection,
        xcb_grab_pointer(
            second_connection, 0, screen->root, XCB_EVENT_MASK_BUTTON_PRESS,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
            XCB_CURRENT_TIME),
        &error);
    if (error != NULL || pointer_grab == NULL ||
        pointer_grab->status != XCB_GRAB_STATUS_ALREADY_GRABBED ||
        !checked(connection,
                 xcb_change_active_pointer_grab_checked(
                     connection, XCB_NONE, XCB_CURRENT_TIME,
                     XCB_EVENT_MASK_BUTTON_RELEASE),
                 "ChangeActivePointerGrab") ||
        !checked_error(
            connection,
            xcb_allow_events_checked(
                connection, UINT8_MAX, XCB_CURRENT_TIME),
            XCB_VALUE, "AllowEvents invalid mode") ||
        !checked(connection,
                 xcb_allow_events_checked(
                     connection, XCB_ALLOW_ASYNC_POINTER, XCB_CURRENT_TIME),
                 "AllowEvents") ||
        !checked(connection,
                 xcb_ungrab_pointer_checked(connection, XCB_CURRENT_TIME),
                 "UngrabPointer")) {
        goto cleanup;
    }
    keyboard_grab = xcb_grab_keyboard_reply(
        connection,
        xcb_grab_keyboard(
            connection, 0, child, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC),
        &error);
    if (error != NULL || keyboard_grab == NULL ||
        keyboard_grab->status != XCB_GRAB_STATUS_SUCCESS)
        goto cleanup;
    free(keyboard_grab);
    keyboard_grab = xcb_grab_keyboard_reply(
        second_connection,
        xcb_grab_keyboard(
            second_connection, 0, screen->root, XCB_CURRENT_TIME,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        &error);
    if (error != NULL || keyboard_grab == NULL ||
        keyboard_grab->status != XCB_GRAB_STATUS_ALREADY_GRABBED ||
        !checked(connection,
                 xcb_ungrab_keyboard_checked(connection, XCB_CURRENT_TIME),
                 "UngrabKeyboard")) {
        goto cleanup;
    }

    stage = "subtracting and arbitrating passive input grabs";
    if (!checked(connection,
                 xcb_grab_key_checked(
                     connection, 0, child, XCB_MOD_MASK_ANY, XCB_GRAB_ANY,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
                 "GrabKey wildcard") ||
        !checked_error(
            second_connection,
            xcb_grab_key_checked(
                second_connection, 0, child, XCB_MOD_MASK_SHIFT, 38,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
            XCB_ACCESS, "GrabKey conflicting exact") ||
        !checked(connection,
                 xcb_ungrab_key_checked(
                     connection, 38, child, XCB_MOD_MASK_SHIFT),
                 "UngrabKey carve wildcard") ||
        !checked(second_connection,
                 xcb_grab_key_checked(
                     second_connection, 0, child, XCB_MOD_MASK_SHIFT, 38,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
                 "GrabKey carved exact") ||
        !checked_error(
            second_connection,
            xcb_grab_key_checked(
                second_connection, 0, child, XCB_MOD_MASK_SHIFT, 39,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
            XCB_ACCESS, "GrabKey wildcard remainder") ||
        !checked(second_connection,
                 xcb_ungrab_key_checked(
                     second_connection, 38, child, XCB_MOD_MASK_SHIFT),
                 "UngrabKey second client") ||
        !checked(connection,
                 xcb_ungrab_key_checked(
                     connection, XCB_GRAB_ANY, child, XCB_MOD_MASK_ANY),
                 "UngrabKey wildcard") ||
        !checked(connection,
                 xcb_grab_button_checked(
                     connection, 0, child,
                     XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                     XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY),
                 "GrabButton wildcard") ||
        !checked_error(
            second_connection,
            xcb_grab_button_checked(
                second_connection, 0, child, XCB_EVENT_MASK_BUTTON_PRESS,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_CONTROL),
            XCB_ACCESS, "GrabButton conflicting exact") ||
        !checked(connection,
                 xcb_ungrab_button_checked(
                     connection, XCB_BUTTON_INDEX_1, child,
                     XCB_MOD_MASK_CONTROL),
                 "UngrabButton carve wildcard") ||
        !checked(second_connection,
                 xcb_grab_button_checked(
                     second_connection, 0, child,
                     XCB_EVENT_MASK_BUTTON_PRESS,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                     XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_CONTROL),
                 "GrabButton carved exact") ||
        !checked_error(
            second_connection,
            xcb_grab_button_checked(
                second_connection, 0, child,
                XCB_EVENT_MASK_BUTTON_PRESS,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                XCB_NONE, XCB_BUTTON_INDEX_2, XCB_MOD_MASK_CONTROL),
            XCB_ACCESS, "GrabButton wildcard remainder") ||
        !checked(second_connection,
                 xcb_ungrab_button_checked(
                     second_connection, XCB_BUTTON_INDEX_1, child,
                     XCB_MOD_MASK_CONTROL),
                 "UngrabButton second client") ||
        !checked(connection,
                 xcb_ungrab_button_checked(
                     connection, XCB_BUTTON_INDEX_ANY, child,
                     XCB_MOD_MASK_ANY),
                 "UngrabButton wildcard")) {
        goto cleanup;
    }

    stage = "copying pixmap pixels into a window";
    pixmap = xcb_generate_id(connection);
    graphics = xcb_generate_id(connection);
    {
        const uint32_t red = 0x00ff0000U;
        const uint32_t blue = 0x000000ffU;
        const uint32_t green = 0x0000ff00U;
        const uint32_t background = 0x00123456U;
        const xcb_rectangle_t rectangle = { 0, 0, 12, 9 };
        const xcb_rectangle_t point = { 1, 0, 1, 1 };

        if (!checked(connection,
                     xcb_create_pixmap_checked(connection, screen->root_depth,
                                               pixmap, screen->root, 12, 9),
                     "CreatePixmap") ||
            !checked(connection,
                     xcb_create_gc_checked(connection, graphics, pixmap,
                                           XCB_GC_FOREGROUND, &red),
                     "CreateGC") ||
            !checked(connection,
                     xcb_poly_fill_rectangle_checked(
                         connection, pixmap, graphics, 1, &rectangle),
                     "PolyFillRectangle") ||
            !checked(connection,
                     xcb_put_image_checked(
                         connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap,
                         graphics, 1, 1, 6, 4, 0, 24,
                         sizeof(blue), (const uint8_t *) &blue),
                     "PutImage") ||
            !checked(connection,
                     xcb_copy_area_checked(connection, pixmap, child, graphics,
                                           0, 0, 0, 0, 12, 9),
                     "CopyArea") ||
            !checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics, XCB_GC_FOREGROUND, &green),
                     "ChangeGC") ||
            !checked(connection,
                     xcb_poly_fill_rectangle_checked(
                         connection, child, graphics, 1, &point),
                     "PolyFillRectangle after ChangeGC") ||
            !checked(connection,
                     xcb_change_window_attributes_checked(
                         connection, child, XCB_CW_BACK_PIXEL, &background),
                     "ChangeWindowAttributes") ||
            !checked(connection,
                     xcb_clear_area_checked(
                         connection, 0, child, 0, 0, 1, 1),
                     "ClearArea"))
            goto cleanup;
    }
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, child, 6, 4,
                      1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x000000ffU)
        goto cleanup;
    stage = "checking changed GC and window background";
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, child, 0, 0,
                      2, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 8)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x00123456U)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image) + 4, sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x0000ff00U)
        goto cleanup;
    stage = "copying root inferiors into a pixmap";
    {
        const uint32_t include_values[] = {
            0x00ffff00U,
            0x0000ffffU,
            XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS
        };
        const uint32_t clip_by_children =
            XCB_SUBWINDOW_MODE_CLIP_BY_CHILDREN;

        if (!checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics,
                         XCB_GC_FOREGROUND | XCB_GC_BACKGROUND |
                             XCB_GC_SUBWINDOW_MODE,
                         include_values),
                     "ChangeGC IncludeInferiors") ||
            !checked(connection,
                     xcb_copy_area_checked(
                         connection, screen->root, pixmap, graphics,
                         translated->dst_x + 1, translated->dst_y,
                         0, 0, 1, 1),
                     "CopyArea IncludeInferiors") ||
            !checked(connection,
                     xcb_copy_plane_checked(
                         connection, screen->root, pixmap, graphics,
                         translated->dst_x + 1, translated->dst_y,
                         1, 0, 1, 1, 0x00000100U),
                     "CopyPlane IncludeInferiors") ||
            !checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics, XCB_GC_SUBWINDOW_MODE,
                         &clip_by_children),
                     "restore GC subwindow mode")) {
            goto cleanup;
        }
    }
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap,
                      0, 0, 2, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 8)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x0000ff00U)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image) + 4, sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x00ffff00U)
        goto cleanup;
    stage = "drawing core point and line primitives";
    {
        const uint32_t stroke = 0x00fedcbaU;
        const xcb_point_t point = { 2, 1 };
        const xcb_point_t line[] = { { 3, 1 }, { 5, 1 } };
        const xcb_segment_t segment = { 6, 1, 6, 3 };
        const xcb_rectangle_t outline = { 8, 1, 2, 2 };

        if (!checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics, XCB_GC_FOREGROUND, &stroke),
                     "ChangeGC for core primitives") ||
            !checked(connection,
                     xcb_poly_point_checked(
                         connection, XCB_COORD_MODE_ORIGIN, child, graphics,
                         1, &point),
                     "PolyPoint") ||
            !checked(connection,
                     xcb_poly_line_checked(
                         connection, XCB_COORD_MODE_ORIGIN, child, graphics,
                         2, line),
                     "PolyLine") ||
            !checked(connection,
                     xcb_poly_segment_checked(
                         connection, child, graphics, 1, &segment),
                     "PolySegment") ||
            !checked(connection,
                     xcb_poly_rectangle_checked(
                         connection, child, graphics, 1, &outline),
                     "PolyRectangle")) {
            goto cleanup;
        }
    }
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, child, 0, 0,
                      11, 4, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 11 * 4 * 4)
        goto cleanup;
    {
        static const unsigned stroke_pixels[][2] = {
            { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 },
            { 6, 1 }, { 6, 2 }, { 6, 3 },
            { 8, 1 }, { 9, 1 }, { 10, 1 }, { 8, 2 }, { 10, 2 },
            { 8, 3 }, { 9, 3 }, { 10, 3 },
        };
        size_t index;

        for (index = 0;
             index < sizeof(stroke_pixels) / sizeof(stroke_pixels[0]);
             ++index) {
            const size_t offset =
                (stroke_pixels[index][1] * 11 + stroke_pixels[index][0]) * 4;

            memcpy(&pixel, xcb_get_image_data(image) + offset,
                   sizeof(pixel));
            if ((pixel & 0x00ffffffU) != 0x00fedcbaU)
                goto cleanup;
        }
        memcpy(&pixel, xcb_get_image_data(image) + (2 * 11 + 9) * 4,
               sizeof(pixel));
        if ((pixel & 0x00ffffffU) != 0x00ff0000U)
            goto cleanup;
    }

    stage = "copying a bitmap plane through GC colors";
    bitmap = xcb_generate_id(connection);
    bitmap_graphics = xcb_generate_id(connection);
    {
        uint8_t bitmap_data[4] = { 0, 0, 0, 0 };
        const uint32_t colors[] = { 0x00ffff00U, 0x0000ffffU };
        const xcb_setup_t *setup = xcb_get_setup(connection);

        bitmap_data[0] = setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_LSB_FIRST
            ? 0x05U
            : 0xa0U;
        if (!checked(connection,
                     xcb_create_pixmap_checked(
                         connection, 1, bitmap, screen->root, 8, 1),
                     "CreatePixmap bitmap") ||
            !checked(connection,
                     xcb_create_gc_checked(
                         connection, bitmap_graphics, bitmap, 0, NULL),
                     "CreateGC bitmap") ||
            !checked(connection,
                     xcb_put_image_checked(
                         connection, XCB_IMAGE_FORMAT_Z_PIXMAP, bitmap,
                         bitmap_graphics, 8, 1, 0, 0, 0, 1,
                         sizeof(bitmap_data), bitmap_data),
                     "PutImage bitmap") ||
            !checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics,
                         XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, colors),
                     "ChangeGC CopyPlane colors") ||
            !checked(connection,
                     xcb_copy_plane_checked(
                         connection, bitmap, child, graphics, 0, 0, 0, 5,
                         8, 1, 1),
                     "CopyPlane")) {
            goto cleanup;
        }
    }
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, child, 0, 5,
                      8, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 8 * 4)
        goto cleanup;
    {
        unsigned x;

        for (x = 0; x < 8; ++x) {
            const uint32_t expected = x == 0 || x == 2
                ? 0x00ffff00U
                : 0x0000ffffU;
            memcpy(&pixel, xcb_get_image_data(image) + x * 4,
                   sizeof(pixel));
            if ((pixel & 0x00ffffffU) != expected)
                goto cleanup;
        }
    }
    stage = "checking child pixels through the root";
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, screen->root,
                      translated->dst_x + 6, translated->dst_y + 4,
                      1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x000000ffU)
        goto cleanup;

    stage = "clipping raster operations to a rectangle union";
    {
        const uint32_t clip_values[] = { XCB_GX_XOR, 0x00ffffffU };
        const xcb_rectangle_t clips[] = {
            { 0, 7, 3, 2 }, { 2, 7, 3, 2 }
        };
        const xcb_rectangle_t unsorted[] = {
            { 0, 8, 1, 1 }, { 0, 7, 1, 1 }
        };
        const xcb_rectangle_t fill = { 0, 7, 8, 2 };

        if (!checked(connection,
                     xcb_change_gc_checked(
                         connection, graphics,
                         XCB_GC_FUNCTION | XCB_GC_FOREGROUND, clip_values),
                     "ChangeGC for clipped raster") ||
            !checked_error(
                connection,
                xcb_set_clip_rectangles_checked(
                    connection, XCB_CLIP_ORDERING_Y_SORTED, graphics,
                    0, 0, 2, unsorted),
                XCB_MATCH, "SetClipRectangles ordering validation") ||
            !checked(connection,
                     xcb_set_clip_rectangles_checked(
                         connection, XCB_CLIP_ORDERING_UNSORTED, graphics,
                         1, 0, 2, clips),
                     "SetClipRectangles") ||
            !checked(connection,
                     xcb_poly_fill_rectangle_checked(
                         connection, child, graphics, 1, &fill),
                     "PolyFillRectangle with clip")) {
            goto cleanup;
        }
    }
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, child, 0, 7,
                      8, 2, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 8 * 2 * 4)
        goto cleanup;
    {
        unsigned y;

        for (y = 0; y < 2; ++y) {
            unsigned x;

            for (x = 0; x < 8; ++x) {
                const uint32_t expected = x >= 1 && x < 6
                    ? 0x0000ffffU
                    : 0x00ff0000U;
                memcpy(&pixel,
                       xcb_get_image_data(image) + (y * 8 + x) * 4,
                       sizeof(pixel));
                if ((pixel & 0x00ffffffU) != expected)
                    goto cleanup;
            }
        }
    }

    stage = "round-tripping selection ownership";
    if (!checked(connection,
                 xcb_set_selection_owner_checked(
                     connection, child, selection_atom->atom,
                     XCB_CURRENT_TIME),
                 "SetSelectionOwner"))
        goto cleanup;
    selection_owned = 1;
    owner = xcb_get_selection_owner_reply(
        connection,
        xcb_get_selection_owner(connection, selection_atom->atom), &error);
    if (error != NULL || owner == NULL || owner->owner != child)
        goto cleanup;

    stage = "allocating and querying a named color";
    named_color = xcb_alloc_named_color_reply(
        connection,
        xcb_alloc_named_color(connection, screen->default_colormap,
                              sizeof(color_name) - 1, color_name),
        &error);
    if (error != NULL || named_color == NULL || named_color->exact_red < 65000U ||
        named_color->exact_green != 0 || named_color->exact_blue != 0)
        goto cleanup;
    queried_color = xcb_query_colors_reply(
        connection,
        xcb_query_colors(connection, screen->default_colormap, 1,
                         &named_color->pixel),
        &error);
    if (error != NULL || queried_color == NULL ||
        xcb_query_colors_colors_length(queried_color) != 1 ||
        xcb_query_colors_colors(queried_color)[0].red < 65000U)
        goto cleanup;

    stage = "exercising fixed TrueColor colormaps";
    colormap = xcb_generate_id(connection);
    copied_colormap = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_colormap_checked(
                     connection, XCB_COLORMAP_ALLOC_NONE, colormap,
                     screen->root, screen->root_visual),
                 "CreateColormap"))
        goto cleanup;
    allocated_color = xcb_alloc_color_reply(
        connection,
        xcb_alloc_color(connection, colormap, 0x1234U, 0x5678U, 0x9abcU),
        &error);
    looked_up_color = xcb_lookup_color_reply(
        connection,
        xcb_lookup_color(connection, colormap, sizeof(color_name) - 1,
                         color_name),
        &error);
    if (error != NULL || allocated_color == NULL ||
        allocated_color->pixel != 0x0012569aU ||
        allocated_color->red != 0x1212U ||
        allocated_color->green != 0x5656U ||
        allocated_color->blue != 0x9a9aU || looked_up_color == NULL ||
        looked_up_color->exact_red < 65000U ||
        looked_up_color->visual_red < 65000U)
        goto cleanup;
    if (!checked(connection,
                 xcb_free_colors_checked(connection, colormap, 0, 1,
                                         &allocated_color->pixel),
                 "FreeColors") ||
        !checked(connection,
                 xcb_copy_colormap_and_free_checked(
                     connection, copied_colormap, colormap),
                 "CopyColormapAndFree") ||
        !checked(connection,
                 xcb_install_colormap_checked(connection, copied_colormap),
                 "InstallColormap"))
        goto cleanup;
    installed_colormaps = xcb_list_installed_colormaps_reply(
        connection,
        xcb_list_installed_colormaps(connection, screen->root), &error);
    if (error != NULL || installed_colormaps == NULL ||
        xcb_list_installed_colormaps_cmaps_length(installed_colormaps) != 1 ||
        xcb_list_installed_colormaps_cmaps(installed_colormaps)[0] !=
            copied_colormap)
        goto cleanup;
    if (!checked(connection,
                 xcb_uninstall_colormap_checked(connection, copied_colormap),
                 "UninstallColormap"))
        goto cleanup;

    stage = "delivering a synthetic client message";
    {
        xcb_client_message_event_t message;

        memset(&message, 0, sizeof(message));
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = parent;
        message.type = message_atom->atom;
        message.data.data32[0] = 0x584d494eU;
        if (!checked(connection,
                     xcb_send_event_checked(
                         connection, 0, parent, XCB_EVENT_MASK_NO_EVENT,
                         (const char *) &message),
                     "SendEvent") ||
            !wait_for_client_message(connection, parent, message_atom->atom,
                                     message.data.data32[0]))
            goto cleanup;
    }
    result = 0;

cleanup:
    if (result != 0)
        fprintf(stderr, "core X11 acceptance failed while %s (pixel 0x%08x)\n",
                stage, pixel);
    if (connection != NULL && !xcb_connection_has_error(connection)) {
        if (selection_owned)
            xcb_set_selection_owner(connection, XCB_NONE,
                                    selection_atom->atom, XCB_CURRENT_TIME);
        if (property_created)
            xcb_delete_property(connection, screen->root,
                                property_atom->atom);
        if (message_property_created)
            xcb_delete_property(connection, screen->root,
                                message_atom->atom);
        if (graphics != XCB_NONE)
            xcb_free_gc(connection, graphics);
        if (bitmap_graphics != XCB_NONE)
            xcb_free_gc(connection, bitmap_graphics);
        if (pixmap != XCB_NONE)
            xcb_free_pixmap(connection, pixmap);
        if (bitmap != XCB_NONE)
            xcb_free_pixmap(connection, bitmap);
        if (copied_colormap != XCB_NONE)
            xcb_free_colormap(connection, copied_colormap);
        if (colormap != XCB_NONE)
            xcb_free_colormap(connection, colormap);
        if (parent != XCB_NONE)
            xcb_destroy_window(connection, parent);
        xcb_flush(connection);
    }
    free(error);
    free(keyboard_grab);
    free(pointer_grab);
    free(focus);
    free(modifier_mapping);
    free(set_modifier_mapping);
    free(pointer_mapping);
    free(set_pointer_mapping);
    free(pointer_control);
    free(keyboard_control);
    free(keyboard_mapping);
    free(keymap);
    free(motion);
    free(pointer);
    free(best_size);
    free(image);
    free(queried_color);
    free(installed_colormaps);
    free(looked_up_color);
    free(allocated_color);
    free(named_color);
    free(owner);
    free(translated);
    free(attributes);
    free(geometry);
    free(tree);
    free(properties);
    free(property);
    free(rotated_property);
    free(message_atom);
    free(selection_atom);
    free(property_atom);
    if (connection != NULL)
        xcb_disconnect(connection);
    if (second_connection != NULL)
        xcb_disconnect(second_connection);
    return result;
}
