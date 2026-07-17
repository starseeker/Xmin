#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == NULL && xcb_connection_has_error(connection) == 0)
        return 1;
    if (error == NULL) {
        fprintf(stderr, "%s closed XCB connection (%d)\n", operation,
                xcb_connection_has_error(connection));
        return 0;
    }
    fprintf(stderr, "%s failed with X error %u (minor %u)\n",
            operation, error->error_code, error->minor_code);
    free(error);
    return 0;
}

static xcb_screen_t *
screen_for(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

static xcb_atom_t
intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, (uint16_t) strlen(name), name), NULL);
    xcb_atom_t atom = reply == NULL ? XCB_ATOM_NONE : reply->atom;
    free(reply);
    return atom;
}

static int
find_visual_format(const xcb_render_query_pict_formats_reply_t *formats,
                   xcb_visualid_t visual, xcb_render_pictformat_t *format)
{
    xcb_render_pictscreen_iterator_t screens =
        xcb_render_query_pict_formats_screens_iterator(formats);
    while (screens.rem != 0) {
        xcb_render_pictdepth_iterator_t depths =
            xcb_render_pictscreen_depths_iterator(screens.data);
        while (depths.rem != 0) {
            xcb_render_pictvisual_iterator_t visuals =
                xcb_render_pictdepth_visuals_iterator(depths.data);
            while (visuals.rem != 0) {
                if (visuals.data->visual == visual) {
                    *format = visuals.data->format;
                    return 1;
                }
                xcb_render_pictvisual_next(&visuals);
            }
            xcb_render_pictdepth_next(&depths);
        }
        xcb_render_pictscreen_next(&screens);
    }
    return 0;
}

int
main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    xcb_connection_t *other = NULL;
    xcb_screen_t *screen;
    xcb_generic_error_t *error = NULL;
    xcb_xfixes_query_version_reply_t *version = NULL;
    xcb_xfixes_fetch_region_reply_t *fetched = NULL;
    xcb_xfixes_get_cursor_name_reply_t *cursor_name = NULL;
    xcb_xfixes_get_cursor_image_reply_t *cursor_image = NULL;
    xcb_xfixes_get_cursor_image_and_name_reply_t *image_name = NULL;
    xcb_xfixes_get_client_disconnect_mode_reply_t *disconnect = NULL;
    xcb_render_query_pict_formats_reply_t *formats = NULL;
    xcb_render_pictformat_t visual_format = XCB_NONE;
    const xcb_query_extension_reply_t *extension;
    const xcb_query_extension_reply_t *render_extension;
    xcb_window_t child = XCB_NONE;
    xcb_window_t foreign = XCB_NONE;
    xcb_pixmap_t bitmap = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    xcb_render_picture_t picture = XCB_NONE;
    xcb_cursor_t cursor1 = XCB_NONE;
    xcb_cursor_t cursor2 = XCB_NONE;
    xcb_xfixes_barrier_t barrier = XCB_NONE;
    xcb_xfixes_region_t regions[8] = {0};
    xcb_rectangle_t rectangle = {2, 3, 12, 9};
    uint32_t values[2];
    int passed = 0;
    const char *stage = "connecting";

    if (xcb_connection_has_error(connection))
        goto cleanup;
    screen = screen_for(connection, screen_number);
    if (screen == NULL)
        goto cleanup;
    extension = xcb_get_extension_data(connection, &xcb_xfixes_id);
    render_extension = xcb_get_extension_data(connection, &xcb_render_id);
    stage = "querying XFIXES version";
    version = xcb_xfixes_query_version_reply(
        connection, xcb_xfixes_query_version(connection, 6, 0), &error);
    if (extension == NULL || !extension->present ||
        render_extension == NULL || !render_extension->present ||
        error != NULL ||
        version == NULL || version->major_version != 6)
        goto cleanup;
    stage = "querying the RENDER visual format";
    formats = xcb_render_query_pict_formats_reply(
        connection, xcb_render_query_pict_formats(connection), &error);
    if (error != NULL || formats == NULL ||
        !find_visual_format(formats, screen->root_visual, &visual_format))
        goto cleanup;

    child = xcb_generate_id(connection);
    stage = "creating the test window";
    values[0] = screen->black_pixel;
    values[1] = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, child, screen->root,
                     5, 6, 40, 30, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values),
                 "CreateWindow") ||
        !checked(connection, xcb_map_window_checked(connection, child),
                 "MapWindow"))
        goto cleanup;

    other = xcb_connect(NULL, NULL);
    stage = "testing ChangeSaveSet";
    if (xcb_connection_has_error(other))
        goto cleanup;
    foreign = xcb_generate_id(other);
    if (!checked(other,
                 xcb_create_window_checked(
                     other, screen->root_depth, foreign, screen->root,
                     0, 0, 8, 8, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, 0, NULL),
                 "Create foreign window") ||
        xcb_flush(other) <= 0 ||
        !checked(connection,
                 xcb_xfixes_change_save_set_checked(
                     connection, 0, 1, 0, foreign),
                 "XFIXES ChangeSaveSet insert") ||
        !checked(connection,
                 xcb_xfixes_change_save_set_checked(
                     connection, 1, 1, 0, foreign),
                 "XFIXES ChangeSaveSet delete"))
        goto cleanup;

    {
        stage = "testing selection input";
        xcb_atom_t selection = intern(connection, "_XMIN_SERVER_SELECTION");
        if (selection == XCB_ATOM_NONE ||
            !checked(connection,
                     xcb_xfixes_select_selection_input_checked(
                         connection, screen->root, selection, 7),
                     "XFIXES SelectSelectionInput") ||
            !checked(connection,
                     xcb_set_selection_owner_checked(
                         connection, child, selection, XCB_CURRENT_TIME),
                     "SetSelectionOwner"))
            goto cleanup;
        for (;;) {
            xcb_generic_event_t *event = xcb_wait_for_event(connection);
            if (event == NULL)
                goto cleanup;
            if ((event->response_type & 0x7fU) == extension->first_event) {
                xcb_xfixes_selection_notify_event_t *notify =
                    (xcb_xfixes_selection_notify_event_t *) event;
                int valid = notify->selection == selection &&
                    notify->owner == child;
                free(event);
                if (!valid)
                    goto cleanup;
                break;
            }
            free(event);
        }
        {
            xcb_get_input_focus_cookie_t focus_cookie =
                xcb_get_input_focus(connection);
            xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(
                connection, focus_cookie, &error);
            if (error != NULL || focus == NULL) {
                free(focus);
                goto cleanup;
            }
            free(focus);
        }
    }

    bitmap = xcb_generate_id(connection);
    stage = "creating cursor and clip prerequisites";
    gc = xcb_generate_id(connection);
    picture = xcb_generate_id(connection);
    cursor1 = xcb_generate_id(connection);
    cursor2 = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_pixmap_checked(
                     connection, 1, bitmap, screen->root, 8, 8),
                 "Create bitmap") ||
        !checked(connection,
                 xcb_create_gc_checked(connection, gc, child, 0, NULL),
                 "Create GC") ||
        !checked(connection,
                 xcb_set_clip_rectangles_checked(
                     connection, XCB_CLIP_ORDERING_UNSORTED, gc, 1, 1,
                     1, &rectangle),
                 "Set GC clip") ||
        !checked(connection,
                 xcb_render_create_picture_checked(
                     connection, picture, child, visual_format, 0, NULL),
                 "Create RENDER picture") ||
        !checked(connection,
                 xcb_render_set_picture_clip_rectangles_checked(
                     connection, picture, 0, 0, 1, &rectangle),
                 "Set picture clip") ||
        !checked(connection,
                 xcb_create_cursor_checked(
                     connection, cursor1, bitmap, bitmap,
                     0xffff, 0, 0, 0, 0, 0xffff, 0, 0),
                 "Create cursor 1") ||
        !checked(connection,
                 xcb_create_cursor_checked(
                     connection, cursor2, bitmap, bitmap,
                     0, 0xffff, 0, 0xffff, 0, 0, 0, 0),
                 "Create cursor 2") ||
        !checked(connection,
                     xcb_xfixes_select_cursor_input_checked(
                         connection, screen->root,
                         XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR),
                 "XFIXES SelectCursorInput"))
        goto cleanup;
    values[0] = cursor1;
    if (!checked(connection,
                 xcb_change_window_attributes_checked(
                     connection, screen->root, XCB_CW_CURSOR, values),
                 "Install cursor"))
        goto cleanup;

    for (;;) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        if (event == NULL)
            goto cleanup;
        if ((event->response_type & 0x7fU) == extension->first_event + 1) {
            xcb_xfixes_cursor_notify_event_t *notify =
                (xcb_xfixes_cursor_notify_event_t *) event;
            int valid = notify->window == screen->root &&
                notify->cursor_serial != 0;
            free(event);
            if (!valid)
                goto cleanup;
            break;
        }
        free(event);
    }

    stage = "reading the cursor image";
    {
        xcb_xfixes_get_cursor_image_cookie_t cookie =
            xcb_xfixes_get_cursor_image(connection);
        cursor_image = xcb_xfixes_get_cursor_image_reply(
            connection, cookie, &error);
    }
    if (error != NULL || cursor_image == NULL ||
        cursor_image->width != 8 || cursor_image->height != 8 ||
        xcb_xfixes_get_cursor_image_cursor_image_length(cursor_image) != 64) {
        goto cleanup;
    }

    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i)
        regions[i] = xcb_generate_id(connection);
    stage = "creating and combining regions";
    if (!checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, regions[0], 1, &rectangle),
                 "XFIXES CreateRegion") ||
        !checked(connection,
                 xcb_xfixes_create_region_from_bitmap_checked(
                     connection, regions[1], bitmap),
                 "XFIXES CreateRegionFromBitmap") ||
        !checked(connection,
                 xcb_xfixes_create_region_from_window_checked(
                     connection, regions[2], child, XCB_SHAPE_SK_BOUNDING),
                 "XFIXES CreateRegionFromWindow") ||
        !checked(connection,
                 xcb_xfixes_create_region_from_gc_checked(
                     connection, regions[3], gc),
                 "XFIXES CreateRegionFromGC") ||
        !checked(connection,
                 xcb_xfixes_create_region_from_picture_checked(
                     connection, regions[4], picture),
                 "XFIXES CreateRegionFromPicture") ||
        !checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, regions[5], 0, NULL),
                 "XFIXES CreateRegion destination") ||
        !checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, regions[6], 0, NULL),
                 "XFIXES CreateRegion destination 2") ||
        !checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, regions[7], 0, NULL),
                 "XFIXES CreateRegion destination 3") ||
        !checked(connection,
                 xcb_xfixes_set_region_checked(
                     connection, regions[1], 1, &rectangle),
                 "XFIXES SetRegion") ||
        !checked(connection,
                 xcb_xfixes_copy_region_checked(
                     connection, regions[0], regions[5]),
                 "XFIXES CopyRegion") ||
        !checked(connection,
                 xcb_xfixes_union_region_checked(
                     connection, regions[0], regions[2], regions[5]),
                 "XFIXES UnionRegion") ||
        !checked(connection,
                 xcb_xfixes_intersect_region_checked(
                     connection, regions[0], regions[2], regions[5]),
                 "XFIXES IntersectRegion") ||
        !checked(connection,
                 xcb_xfixes_subtract_region_checked(
                     connection, regions[2], regions[0], regions[5]),
                 "XFIXES SubtractRegion") ||
        !checked(connection,
                 xcb_xfixes_invert_region_checked(
                     connection, regions[0], (xcb_rectangle_t){0, 0, 30, 20},
                     regions[5]),
                 "XFIXES InvertRegion") ||
        !checked(connection,
                 xcb_xfixes_translate_region_checked(
                     connection, regions[5], 2, -1),
                 "XFIXES TranslateRegion") ||
        !checked(connection,
                 xcb_xfixes_region_extents_checked(
                     connection, regions[5], regions[6]),
                 "XFIXES RegionExtents"))
        goto cleanup;

    stage = "fetching and installing regions";
    fetched = xcb_xfixes_fetch_region_reply(
        connection, xcb_xfixes_fetch_region(connection, regions[0]), &error);
    if (error != NULL || fetched == NULL ||
        xcb_xfixes_fetch_region_rectangles_length(fetched) != 1)
        goto cleanup;
    stage = "testing cursor names and replacement";
    if (!checked(connection,
                 xcb_xfixes_set_gc_clip_region_checked(
                     connection, gc, regions[0], 3, 4),
                 "XFIXES SetGCClipRegion") ||
        !checked(connection,
                 xcb_xfixes_set_window_shape_region_checked(
                     connection, child, XCB_SHAPE_SK_BOUNDING, 0, 0,
                     regions[2]),
                 "XFIXES SetWindowShapeRegion") ||
        !checked(connection,
                 xcb_xfixes_set_picture_clip_region_checked(
                     connection, picture, regions[0], 1, 2),
                 "XFIXES SetPictureClipRegion") ||
        !checked(connection,
                 xcb_xfixes_expand_region_checked(
                     connection, regions[0], regions[7], 1, 2, 3, 4),
                 "XFIXES ExpandRegion"))
        goto cleanup;

    if (!checked(connection,
                 xcb_xfixes_set_cursor_name_checked(
                     connection, cursor1, 6, "source"),
                 "XFIXES SetCursorName"))
        goto cleanup;
    cursor_name = xcb_xfixes_get_cursor_name_reply(
        connection, xcb_xfixes_get_cursor_name(connection, cursor1), &error);
    if (error != NULL || cursor_name == NULL || cursor_name->nbytes != 6 ||
        memcmp(xcb_xfixes_get_cursor_name_name(cursor_name), "source", 6) != 0)
        goto cleanup;
    image_name = xcb_xfixes_get_cursor_image_and_name_reply(
        connection, xcb_xfixes_get_cursor_image_and_name(connection), &error);
    if (error != NULL || image_name == NULL || image_name->nbytes != 6 ||
        xcb_xfixes_get_cursor_image_and_name_name_length(image_name) != 6 ||
        memcmp(xcb_xfixes_get_cursor_image_and_name_name(image_name),
               "source", 6) != 0)
        goto cleanup;
    if (!checked(connection,
                 xcb_xfixes_set_cursor_name_checked(
                     connection, cursor2, 6, "target"),
                 "XFIXES SetCursorName target") ||
        !checked(connection,
                 xcb_xfixes_change_cursor_checked(
                     connection, cursor1, cursor2),
                 "XFIXES ChangeCursor") ||
        !checked(connection,
                 xcb_xfixes_change_cursor_by_name_checked(
                     connection, cursor1, 6, "target"),
                 "XFIXES ChangeCursorByName") ||
        !checked(connection,
                 xcb_xfixes_hide_cursor_checked(connection, screen->root),
                 "XFIXES HideCursor") ||
        !checked(connection,
                 xcb_xfixes_show_cursor_checked(connection, screen->root),
                 "XFIXES ShowCursor"))
        goto cleanup;

    barrier = xcb_generate_id(connection);
    stage = "testing pointer barriers";
    if (!checked(connection,
                 xcb_warp_pointer_checked(
                     connection, XCB_NONE, screen->root,
                     0, 0, 0, 0, 10, 10),
                 "Warp before barrier") ||
        !checked(connection,
                 xcb_xfixes_create_pointer_barrier_checked(
                     connection, barrier, screen->root,
                     50, 0, 50, screen->height_in_pixels, 0, 0, NULL),
                 "XFIXES CreatePointerBarrier") ||
        !checked(connection,
                 xcb_warp_pointer_checked(
                     connection, XCB_NONE, screen->root,
                     0, 0, 0, 0, 70, 10),
                 "Warp across barrier"))
        goto cleanup;
    if (!checked(connection,
                 xcb_xfixes_delete_pointer_barrier_checked(
                     connection, barrier),
                 "XFIXES DeletePointerBarrier") ||
        !checked(connection,
                 xcb_xfixes_set_client_disconnect_mode_checked(connection, 1),
                 "XFIXES SetClientDisconnectMode"))
        goto cleanup;
    disconnect = xcb_xfixes_get_client_disconnect_mode_reply(
        connection, xcb_xfixes_get_client_disconnect_mode(connection), &error);
    if (error != NULL || disconnect == NULL || disconnect->disconnect_mode != 1)
        goto cleanup;

    stage = "testing disconnect mode and destroying regions";
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        if (!checked(connection,
                     xcb_xfixes_destroy_region_checked(connection, regions[i]),
                     "XFIXES DestroyRegion"))
            goto cleanup;
        regions[i] = XCB_NONE;
    }
    passed = 1;

cleanup:
    if (!passed)
        fprintf(stderr, "XFIXES acceptance failed while %s\n", stage);
    if (!passed && error != NULL)
        fprintf(stderr, "XFIXES reply failed with X error %u (minor %u)\n",
                error->error_code, error->minor_code);
    free(error);
    free(disconnect);
    free(formats);
    free(image_name);
    free(cursor_image);
    free(cursor_name);
    free(fetched);
    free(version);
    if (other != NULL)
        xcb_disconnect(other);
    if (connection != NULL)
        xcb_disconnect(connection);
    return passed ? 0 : 1;
}
