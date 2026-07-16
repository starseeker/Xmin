#include <xcb/composite.h>
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
    fprintf(stderr, "%s failed with X11 error %u\n", operation,
            error == NULL ? 0U : error->error_code);
    free(error);
    return 0;
}

static int
checked_error(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              uint8_t expected, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    int result = error != NULL && error->error_code == expected;
    if (!result) {
        fprintf(stderr, "%s returned X11 error %u instead of %u\n",
                operation, error == NULL ? 0U : error->error_code,
                expected);
    }
    free(error);
    return result;
}

static int
geometry_is(xcb_connection_t *connection, xcb_drawable_t drawable,
            uint16_t width, uint16_t height)
{
    xcb_generic_error_t *error = NULL;
    xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, drawable), &error);
    int result = error == NULL && reply != NULL &&
        reply->width == width && reply->height == height;
    if (!result) {
        fprintf(stderr, "drawable 0x%x geometry was %ux%u, expected %ux%u\n",
                drawable, reply == NULL ? 0U : reply->width,
                reply == NULL ? 0U : reply->height, width, height);
    }
    free(error);
    free(reply);
    return result;
}

static int
first_pixel_is(xcb_connection_t *connection, xcb_drawable_t drawable,
               uint32_t expected)
{
    xcb_generic_error_t *error = NULL;
    xcb_get_image_reply_t *reply = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                      0, 0, 1, 1, UINT32_MAX),
        &error);
    uint32_t pixel = 0;
    int result = error == NULL && reply != NULL &&
        xcb_get_image_data_length(reply) >= (int) sizeof(pixel);
    if (result)
        memcpy(&pixel, xcb_get_image_data(reply), sizeof(pixel));
    free(error);
    free(reply);
    if (!result || (pixel & 0x00ffffffU) != expected) {
        fprintf(stderr, "drawable 0x%x pixel was 0x%08x, expected 0x%08x\n",
                drawable, pixel, expected);
        return 0;
    }
    return 1;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup(connection)).data;
    xcb_generic_error_t *error = NULL;
    xcb_composite_query_version_reply_t *composite_version = NULL;
    xcb_xfixes_query_version_reply_t *xfixes_version = NULL;
    xcb_xfixes_fetch_region_reply_t *border = NULL;
    xcb_window_t window = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    xcb_pixmap_t named = XCB_NONE;
    xcb_xfixes_region_t region = XCB_NONE;
    uint32_t values[2];
    const char *stage = "connecting";
    int passed = 0;

    if (xcb_connection_has_error(connection) || screen == NULL)
        goto cleanup;
    stage = "querying extension versions";
    composite_version = xcb_composite_query_version_reply(
        connection, xcb_composite_query_version(connection, 0, 4), &error);
    if (error != NULL || composite_version == NULL ||
        composite_version->major_version != 0 ||
        composite_version->minor_version != 4)
        goto cleanup;
    xfixes_version = xcb_xfixes_query_version_reply(
        connection, xcb_xfixes_query_version(connection, 6, 0), &error);
    if (error != NULL || xfixes_version == NULL ||
        xfixes_version->major_version < 2)
        goto cleanup;

    window = xcb_generate_id(connection);
    gc = xcb_generate_id(connection);
    named = xcb_generate_id(connection);
    region = xcb_generate_id(connection);
    values[0] = screen->black_pixel;
    values[1] = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    stage = "creating a viewable window";
    if (!checked(connection,
                 xcb_create_window_checked(
                     connection, screen->root_depth, window, screen->root,
                     5, 6, 40, 30, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values),
                 "CreateWindow") ||
        !checked(connection,
                 xcb_create_gc_checked(connection, gc, window, 0, NULL),
                 "CreateGC") ||
        !checked(connection, xcb_map_window_checked(connection, window),
                 "MapWindow"))
        goto cleanup;

    stage = "redirecting the window";
    if (!checked(connection,
                 xcb_composite_redirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "Composite RedirectWindow"))
        goto cleanup;

    values[0] = 0x00112233U;
    stage = "drawing and naming the window storage";
    if (!checked(connection,
                 xcb_change_gc_checked(
                     connection, gc, XCB_GC_FOREGROUND, values),
                 "ChangeGC") ||
        !checked(connection,
                 xcb_poly_fill_rectangle_checked(
                     connection, window, gc, 1,
                     &(xcb_rectangle_t){0, 0, 40, 30}),
                 "PolyFillRectangle") ||
        !first_pixel_is(connection, window, 0x00112233U) ||
        !checked(connection,
                 xcb_composite_name_window_pixmap_checked(
                     connection, window, named),
                 "Composite NameWindowPixmap"))
        goto cleanup;

    stage = "creating the border-clip region";
    if (!checked(connection,
                 xcb_composite_create_region_from_border_clip_checked(
                     connection, region, window),
                 "Composite CreateRegionFromBorderClip"))
        goto cleanup;
    border = xcb_xfixes_fetch_region_reply(
        connection, xcb_xfixes_fetch_region(connection, region), &error);
    if (error != NULL || border == NULL)
        goto cleanup;
    if (border->extents.x != 0 || border->extents.y != 0 ||
        border->extents.width != 40 || border->extents.height != 30) {
        fprintf(stderr, "border clip was %d,%d %ux%u\n",
                border->extents.x, border->extents.y,
                border->extents.width, border->extents.height);
        goto cleanup;
    }

    stage = "resizing with a retained named pixmap";
    values[0] = 50;
    values[1] = 35;
    if (!checked(connection,
                 xcb_configure_window_checked(
                     connection, window,
                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                     values),
                 "ConfigureWindow") ||
        !geometry_is(connection, window, 50, 35) ||
        !geometry_is(connection, named, 40, 30))
        goto cleanup;

    stage = "unredirecting the window";
    if (!checked(connection,
                 xcb_composite_unredirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "Composite UnredirectWindow"))
        goto cleanup;
    xcb_free_pixmap(connection, named);
    named = xcb_generate_id(connection);
    if (!checked_error(connection,
                       xcb_composite_name_window_pixmap_checked(
                           connection, window, named),
                       XCB_MATCH, "NameWindowPixmap while unredirected"))
        goto cleanup;

    stage = "redirecting root subwindows";
    if (!checked(connection,
                 xcb_composite_redirect_subwindows_checked(
                     connection, screen->root,
                     XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "Composite RedirectSubwindows") ||
        !checked(connection,
                 xcb_composite_name_window_pixmap_checked(
                     connection, window, named),
                 "NameWindowPixmap through subwindow redirect") ||
        !checked(connection,
                 xcb_composite_unredirect_subwindows_checked(
                     connection, screen->root,
                     XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "Composite UnredirectSubwindows"))
        goto cleanup;

    stage = "checking manual redirect exclusion";
    if (!checked(connection,
                 xcb_composite_redirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_MANUAL),
                 "Composite manual RedirectWindow") ||
        !checked_error(connection,
                       xcb_composite_redirect_window_checked(
                           connection, window,
                           XCB_COMPOSITE_REDIRECT_MANUAL),
                       XCB_ACCESS, "second manual RedirectWindow") ||
        !checked(connection,
                 xcb_composite_unredirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_MANUAL),
                 "Composite manual UnredirectWindow") ||
        !checked_error(connection,
                       xcb_composite_unredirect_window_checked(
                           connection, window,
                           XCB_COMPOSITE_REDIRECT_MANUAL),
                       XCB_VALUE, "second Composite UnredirectWindow") ||
        !checked_error(connection,
                       xcb_composite_redirect_window_checked(
                           connection, screen->root,
                           XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                       XCB_MATCH, "redirecting the root window"))
        goto cleanup;

    passed = 1;

cleanup:
    if (!passed)
        fprintf(stderr, "Composite state test failed while %s\n", stage);
    free(error);
    free(composite_version);
    free(xfixes_version);
    free(border);
    if (connection != NULL) {
        if (region != XCB_NONE)
            xcb_xfixes_destroy_region(connection, region);
        if (named != XCB_NONE)
            xcb_free_pixmap(connection, named);
        if (gc != XCB_NONE)
            xcb_free_gc(connection, gc);
        if (window != XCB_NONE)
            xcb_destroy_window(connection, window);
        xcb_disconnect(connection);
    }
    return passed ? 0 : 1;
}
