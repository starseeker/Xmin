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
    xcb_screen_iterator_t screens;
    xcb_screen_t *screen;
    xcb_generic_error_t *error = NULL;
    xcb_intern_atom_reply_t *property_atom = NULL;
    xcb_intern_atom_reply_t *selection_atom = NULL;
    xcb_intern_atom_reply_t *message_atom = NULL;
    xcb_get_property_reply_t *property = NULL;
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
    if (!checked(connection,
                 xcb_delete_property_checked(connection, screen->root,
                                             property_atom->atom),
                 "DeleteProperty"))
        goto cleanup;
    property_created = 0;

    stage = "creating and configuring a window hierarchy";
    parent = xcb_generate_id(connection);
    child = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, parent, screen->root,
                     2, 3, 40, 30, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
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
    free(message_atom);
    free(selection_atom);
    free(property_atom);
    if (connection != NULL)
        xcb_disconnect(connection);
    return result;
}
