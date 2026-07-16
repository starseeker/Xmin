#include "xmin/config.h"

#include <xcb/bigreq.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/dbe.h>
#include <xcb/ge.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/screensaver.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xc_misc.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#include <xcb/xinerama.h>
#include <xcb/xkb.h>
#include <xcb/xtest.h>
#include <xcb/xcbext.h>

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#if XMIN_HAVE_MITSHM
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

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
    const char *stage = "Generic Event version";
    int result = 0;

    if (error != NULL || generic == NULL || generic->major_version != 1 ||
        generic->minor_version != 0)
        goto cleanup;
    stage = "XC-MISC version";
    misc = xcb_xc_misc_get_version_reply(
        connection, xcb_xc_misc_get_version(connection, 1, 1), &error);
    if (error != NULL || misc == NULL || misc->server_major_version != 1 ||
        misc->server_minor_version < 1)
        goto cleanup;
    stage = "BIG-REQUESTS enable";
    big = xcb_big_requests_enable_reply(
        connection, xcb_big_requests_enable(connection), &error);
    if (error != NULL || big == NULL || big->maximum_request_length <= 65535U)
        goto cleanup;
    result = 1;

cleanup:
    if (!result)
        fprintf(stderr, "foundation extension acceptance failed at %s\n",
                stage);
    free(error);
    free(big);
    free(misc);
    free(generic);
    return result;
}

static int
test_compatibility_queries(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_xinerama_query_version_reply_t *xinerama_version =
        xcb_xinerama_query_version_reply(
            connection, xcb_xinerama_query_version(connection, 1, 1), &error);
    xcb_xinerama_is_active_reply_t *active = NULL;
    xcb_xinerama_query_screens_reply_t *screens = NULL;
    xcb_screensaver_query_version_reply_t *saver_version = NULL;
    xcb_screensaver_query_info_reply_t *saver_info = NULL;
    xcb_xinerama_screen_info_t *layout = NULL;
    uint32_t saver_mask = XCB_SCREENSAVER_EVENT_NOTIFY_MASK |
        XCB_SCREENSAVER_EVENT_CYCLE_MASK;
    const char *stage = "Xinerama version";
    int saver_selected = 0;
    int result = 0;

    if (error != NULL || xinerama_version == NULL ||
        xinerama_version->major < 1)
        goto cleanup;
    stage = "Xinerama active/screens";
    active = xcb_xinerama_is_active_reply(
        connection, xcb_xinerama_is_active(connection), &error);
    screens = xcb_xinerama_query_screens_reply(
        connection, xcb_xinerama_query_screens(connection), &error);
    if (error != NULL || active == NULL || active->state != 1 ||
        screens == NULL || screens->number != 1 ||
        xcb_xinerama_query_screens_screen_info_length(screens) != 1)
        goto cleanup;
    layout = xcb_xinerama_query_screens_screen_info(screens);
    if (layout == NULL || layout->x_org != 0 || layout->y_org != 0 ||
        layout->width != screen->width_in_pixels ||
        layout->height != screen->height_in_pixels)
        goto cleanup;

    stage = "ScreenSaver version";
    saver_version = xcb_screensaver_query_version_reply(
        connection, xcb_screensaver_query_version(connection, 1, 1), &error);
    if (error != NULL || saver_version == NULL ||
        saver_version->server_major_version < 1)
        goto cleanup;
    if (!checked(connection,
                 xcb_screensaver_select_input_checked(connection, screen->root,
                                                      saver_mask),
                 "ScreenSaver SelectInput"))
        goto cleanup;
    saver_selected = 1;
    stage = "ScreenSaver info";
    saver_info = xcb_screensaver_query_info_reply(
        connection, xcb_screensaver_query_info(connection, screen->root),
        &error);
    if (error != NULL || saver_info == NULL ||
        saver_info->state != XCB_SCREENSAVER_STATE_OFF ||
        saver_info->saver_window == XCB_NONE ||
        saver_info->event_mask != saver_mask)
        goto cleanup;
    result = 1;

cleanup:
    if (saver_selected)
        xcb_screensaver_select_input(connection, screen->root, 0);
    if (!result) {
        fprintf(stderr, "Xinerama or ScreenSaver compatibility query failed at %s\n",
                stage);
        if (saver_info != NULL)
            fprintf(stderr,
                    "ScreenSaver state=%u window=%u root=%u mask=0x%x\n",
                    saver_info->state, saver_info->saver_window,
                    screen->root, saver_info->event_mask);
    }
    free(error);
    free(saver_info);
    free(saver_version);
    free(screens);
    free(active);
    free(xinerama_version);
    return result;
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

static int
find_a8_format(const xcb_render_query_pict_formats_reply_t *formats,
               xcb_render_pictformat_t *format)
{
    xcb_render_pictforminfo_iterator_t iterator =
        xcb_render_query_pict_formats_formats_iterator(formats);

    while (iterator.rem != 0) {
        const xcb_render_pictforminfo_t *info = iterator.data;

        if (info->type == XCB_RENDER_PICT_TYPE_DIRECT && info->depth == 8 &&
            info->direct.red_mask == 0 && info->direct.green_mask == 0 &&
            info->direct.blue_mask == 0 && info->direct.alpha_shift == 0 &&
            info->direct.alpha_mask == 0xffU) {
            *format = info->id;
            return 1;
        }
        xcb_render_pictforminfo_next(&iterator);
    }
    return 0;
}

static int
read_drawable_pixel(xcb_connection_t *connection, xcb_drawable_t drawable,
                    int16_t x, int16_t y, uint32_t *pixel)
{
    xcb_generic_error_t *error = NULL;
    xcb_get_image_reply_t *image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                      x, y, 1, 1, UINT32_MAX),
        &error);
    int result = error == NULL && image != NULL &&
        xcb_get_image_data_length(image) >= 4;

    if (result)
        memcpy(pixel, xcb_get_image_data(image), sizeof(*pixel));
    free(error);
    free(image);
    return result;
}

static int
test_render(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_render_query_version_reply_t *version =
        xcb_render_query_version_reply(
            connection, xcb_render_query_version(connection, 0, 11), &error);
    xcb_render_query_pict_formats_reply_t *formats = NULL;
    xcb_render_pictformat_t format = XCB_NONE;
    xcb_render_pictformat_t a8_format = XCB_NONE;
    xcb_window_t window = XCB_NONE;
    xcb_render_picture_t destination = XCB_NONE;
    xcb_render_picture_t green_source = XCB_NONE;
    xcb_render_picture_t black_source = XCB_NONE;
    xcb_render_picture_t blue_source = XCB_NONE;
    xcb_render_picture_t red_source = XCB_NONE;
    xcb_render_picture_t half_red_source = XCB_NONE;
    xcb_render_glyphset_t glyphset = XCB_NONE;
    xcb_render_glyphset_t glyphset_reference = XCB_NONE;
    const xcb_render_color_t green = { 0, 0xffffU, 0, 0xffffU };
    const xcb_render_color_t black = { 0, 0, 0, 0xffffU };
    const xcb_render_color_t blue = { 0, 0, 0xffffU, 0xffffU };
    const xcb_render_color_t red = { 0xffffU, 0, 0, 0xffffU };
    const xcb_render_color_t half_red = { 0x8000U, 0, 0, 0x8000U };
    const xcb_render_trapezoid_t trapezoid = {
        .top = 2 * 65536,
        .bottom = 14 * 65536,
        .left = {
            .p1 = { 4 * 65536, 2 * 65536 },
            .p2 = { 4 * 65536, 14 * 65536 }
        },
        .right = {
            .p1 = { 16 * 65536, 2 * 65536 },
            .p2 = { 16 * 65536, 14 * 65536 }
        }
    };
    const uint32_t glyph_id = 1;
    const xcb_render_glyphinfo_t glyph_info = {
        .width = 8,
        .height = 8,
        .x = 0,
        .y = 0,
        .x_off = 8,
        .y_off = 0
    };
    uint8_t glyph_data[64];
    uint8_t glyph_commands[12] = { 0 };
    int16_t glyph_x = 20;
    int16_t glyph_y = 8;
    uint32_t pixel = 0;
    const char *stage = "querying RENDER version";
    int result = 0;

    if (error != NULL || version == NULL || version->major_version != 0 ||
        version->minor_version < 11)
        goto cleanup;
    free(version);
    version = NULL;
    stage = "querying RENDER formats";
    formats = xcb_render_query_pict_formats_reply(
        connection, xcb_render_query_pict_formats(connection), &error);
    if (error != NULL || formats == NULL || formats->num_formats == 0 ||
        formats->num_screens == 0 || formats->num_visuals == 0 ||
        !find_visual_format(formats, screen->root_visual, &format) ||
        !find_a8_format(formats, &a8_format))
        goto cleanup;

    stage = "creating RENDER destination";
    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    xcb_map_window(connection, window);
    destination = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_render_create_picture_checked(
                     connection, destination, window, format, 0, NULL),
                 "RENDER CreatePicture"))
        goto cleanup;
    green_source = xcb_generate_id(connection);
    black_source = xcb_generate_id(connection);
    blue_source = xcb_generate_id(connection);
    red_source = xcb_generate_id(connection);
    half_red_source = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_render_create_solid_fill_checked(
                     connection, green_source, green),
                 "RENDER CreateSolidFill green") ||
        !checked(connection,
                 xcb_render_create_solid_fill_checked(
                     connection, black_source, black),
                 "RENDER CreateSolidFill black") ||
        !checked(connection,
                 xcb_render_create_solid_fill_checked(
                     connection, blue_source, blue),
                 "RENDER CreateSolidFill blue") ||
        !checked(connection,
                 xcb_render_create_solid_fill_checked(
                     connection, red_source, red),
                 "RENDER CreateSolidFill red") ||
        !checked(connection,
                 xcb_render_create_solid_fill_checked(
                     connection, half_red_source, half_red),
                 "RENDER CreateSolidFill half-red"))
        goto cleanup;

    stage = "compositing an opaque solid";
    if (!checked(connection,
                 xcb_render_composite_checked(
                     connection, XCB_RENDER_PICT_OP_SRC, green_source, XCB_NONE,
                     destination, 0, 0, 0, 0, 0, 0, 32, 24),
                 "RENDER Composite"))
        goto cleanup;
    if (!read_drawable_pixel(connection, window, 16, 12, &pixel) ||
        (pixel & 0x00ffffffU) != 0x0000ff00U)
        goto cleanup;

    stage = "alpha compositing";
    if (!checked(connection,
                 xcb_render_composite_checked(
                     connection, XCB_RENDER_PICT_OP_SRC, blue_source, XCB_NONE,
                     destination, 0, 0, 0, 0, 0, 0, 32, 24),
                 "RENDER blue background") ||
        !checked(connection,
                 xcb_render_composite_checked(
                     connection, XCB_RENDER_PICT_OP_OVER, half_red_source,
                     XCB_NONE, destination, 0, 0, 0, 0, 0, 0, 32, 24),
                 "RENDER alpha Composite") ||
        !read_drawable_pixel(connection, window, 16, 12, &pixel) ||
        ((pixel >> 16) & 0xffU) < 120 ||
        ((pixel >> 16) & 0xffU) > 136 ||
        ((pixel >> 8) & 0xffU) > 8 ||
        (pixel & 0xffU) < 120 || (pixel & 0xffU) > 136)
        goto cleanup;

    stage = "rendering an antialiased trapezoid";
    if (!checked(connection,
                 xcb_render_composite_checked(
                     connection, XCB_RENDER_PICT_OP_SRC, black_source,
                     XCB_NONE, destination, 0, 0, 0, 0, 0, 0, 32, 24),
                 "RENDER clear before trapezoid") ||
        !checked(connection,
                 xcb_render_trapezoids_checked(
                     connection, XCB_RENDER_PICT_OP_OVER, blue_source,
                     destination, a8_format, 0, 0, 1, &trapezoid),
                 "RENDER Trapezoids") ||
        !read_drawable_pixel(connection, window, 8, 8, &pixel) ||
        (pixel & 0x00ffffffU) != 0x000000ffU ||
        !read_drawable_pixel(connection, window, 20, 8, &pixel) ||
        (pixel & 0x00ffffffU) != 0)
        goto cleanup;

    stage = "rendering an A8 glyph";
    memset(glyph_data, 0xff, sizeof(glyph_data));
    glyph_commands[0] = 1;
    memcpy(glyph_commands + 4, &glyph_x, sizeof(glyph_x));
    memcpy(glyph_commands + 6, &glyph_y, sizeof(glyph_y));
    glyph_commands[8] = (uint8_t) glyph_id;
    glyphset = xcb_generate_id(connection);
    glyphset_reference = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_render_create_glyph_set_checked(
                     connection, glyphset, a8_format),
                 "RENDER CreateGlyphSet") ||
        !checked(connection,
                 xcb_render_add_glyphs_checked(
                     connection, glyphset, 1, &glyph_id, &glyph_info,
                     sizeof(glyph_data), glyph_data),
                 "RENDER AddGlyphs") ||
        !checked(connection,
                 xcb_render_reference_glyph_set_checked(
                     connection, glyphset_reference, glyphset),
                 "RENDER ReferenceGlyphSet") ||
        !checked(connection,
                 xcb_render_composite_checked(
                     connection, XCB_RENDER_PICT_OP_SRC, black_source,
                     XCB_NONE, destination, 0, 0, 0, 0, 0, 0, 32, 24),
                 "RENDER clear before glyph") ||
        !checked(connection,
                 xcb_render_composite_glyphs_8_checked(
                     connection, XCB_RENDER_PICT_OP_OVER, red_source,
                     destination, a8_format, glyphset_reference, 0, 0,
                     sizeof(glyph_commands), glyph_commands),
                 "RENDER CompositeGlyphs8") ||
        !read_drawable_pixel(connection, window, 22, 10, &pixel) ||
        (pixel & 0x00ffffffU) != 0x00ff0000U)
        goto cleanup;
    result = 1;

cleanup:
    if (glyphset_reference != XCB_NONE)
        xcb_render_free_glyph_set(connection, glyphset_reference);
    if (glyphset != XCB_NONE)
        xcb_render_free_glyph_set(connection, glyphset);
    if (half_red_source != XCB_NONE)
        xcb_render_free_picture(connection, half_red_source);
    if (red_source != XCB_NONE)
        xcb_render_free_picture(connection, red_source);
    if (blue_source != XCB_NONE)
        xcb_render_free_picture(connection, blue_source);
    if (black_source != XCB_NONE)
        xcb_render_free_picture(connection, black_source);
    if (green_source != XCB_NONE)
        xcb_render_free_picture(connection, green_source);
    if (destination != XCB_NONE)
        xcb_render_free_picture(connection, destination);
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    if (!result)
        fprintf(stderr, "RENDER acceptance failed while %s (pixel 0x%08x)\n",
                stage, pixel);
    free(error);
    free(formats);
    free(version);
    return result;
}

static int
test_randr(xcb_connection_t *connection, xcb_window_t root)
{
    static const char property_name[] = "_XMIN_RANDR_TEST";
    xcb_generic_error_t *error = NULL;
    xcb_randr_query_version_reply_t *version = xcb_randr_query_version_reply(
        connection, xcb_randr_query_version(connection, 1, 6), &error);
    xcb_randr_get_screen_resources_current_reply_t *resources = NULL;
    xcb_randr_get_output_info_reply_t *output = NULL;
    xcb_randr_get_crtc_info_reply_t *crtc = NULL;
    xcb_intern_atom_reply_t *property_atom = NULL;
    xcb_randr_list_output_properties_reply_t *properties = NULL;
    xcb_randr_get_output_property_reply_t *property = NULL;
    xcb_randr_output_t output_id = XCB_NONE;
    const uint32_t property_value = 0x584d494eU;
    const char *stage = "querying RANDR version";
    int property_created = 0;
    int result = 0;

    if (error != NULL || version == NULL || version->major_version != 1 ||
        version->minor_version < 2)
        goto cleanup;
    free(version);
    version = NULL;
    stage = "querying RANDR resources";
    resources = xcb_randr_get_screen_resources_current_reply(
        connection, xcb_randr_get_screen_resources_current(connection, root),
        &error);
    if (error != NULL || resources == NULL || resources->num_crtcs < 1 ||
        resources->num_outputs < 1 || resources->num_modes < 1)
        goto cleanup;
    output_id = xcb_randr_get_screen_resources_current_outputs(resources)[0];
    stage = "querying RANDR output";
    output = xcb_randr_get_output_info_reply(
        connection,
        xcb_randr_get_output_info(
            connection, output_id,
            resources->config_timestamp),
        &error);
    if (error != NULL || output == NULL ||
        output->connection != XCB_RANDR_CONNECTION_CONNECTED ||
        output->crtc == XCB_NONE || output->num_modes < 1 ||
        output->name_len == 0)
        goto cleanup;
    stage = "querying RANDR CRTC";
    crtc = xcb_randr_get_crtc_info_reply(
        connection,
        xcb_randr_get_crtc_info(connection, output->crtc,
                                resources->config_timestamp),
        &error);
    if (error != NULL || crtc == NULL || crtc->width != 96 ||
        crtc->height != 80 || crtc->mode == XCB_NONE ||
        crtc->num_outputs < 1)
        goto cleanup;

    stage = "creating a RANDR output property";
    property_atom = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, sizeof(property_name) - 1,
                        property_name),
        &error);
    if (error != NULL || property_atom == NULL ||
        property_atom->atom == XCB_NONE ||
        !checked(connection,
                 xcb_randr_change_output_property_checked(
                     connection, output_id, property_atom->atom,
                     XCB_ATOM_INTEGER, 32, XCB_PROP_MODE_REPLACE, 1,
                     &property_value),
                 "RANDR ChangeOutputProperty"))
        goto cleanup;
    property_created = 1;

    stage = "listing the RANDR output property";
    properties = xcb_randr_list_output_properties_reply(
        connection, xcb_randr_list_output_properties(connection, output_id),
        &error);
    if (error != NULL || properties == NULL || properties->num_atoms == 0) {
        goto cleanup;
    }
    {
        xcb_atom_t *atoms =
            xcb_randr_list_output_properties_atoms(properties);
        int count = xcb_randr_list_output_properties_atoms_length(properties);
        int found = 0;
        int i;

        for (i = 0; i < count; ++i) {
            if (atoms[i] == property_atom->atom) {
                found = 1;
                break;
            }
        }
        if (!found)
            goto cleanup;
    }

    stage = "reading the RANDR output property";
    property = xcb_randr_get_output_property_reply(
        connection,
        xcb_randr_get_output_property(
            connection, output_id, property_atom->atom, XCB_ATOM_INTEGER,
            0, 1, 0, 0),
        &error);
    if (error != NULL || property == NULL || property->format != 32 ||
        property->type != XCB_ATOM_INTEGER || property->num_items != 1 ||
        property->bytes_after != 0 ||
        xcb_randr_get_output_property_data_length(property) != 4 ||
        memcmp(xcb_randr_get_output_property_data(property), &property_value,
               sizeof(property_value)) != 0)
        goto cleanup;

    stage = "deleting the RANDR output property";
    if (!checked(connection,
                 xcb_randr_delete_output_property_checked(
                     connection, output_id, property_atom->atom),
                 "RANDR DeleteOutputProperty"))
        goto cleanup;
    property_created = 0;
    free(properties);
    properties = xcb_randr_list_output_properties_reply(
        connection, xcb_randr_list_output_properties(connection, output_id),
        &error);
    if (error != NULL || properties == NULL)
        goto cleanup;
    {
        xcb_atom_t *atoms =
            xcb_randr_list_output_properties_atoms(properties);
        int count = xcb_randr_list_output_properties_atoms_length(properties);
        int i;

        for (i = 0; i < count; ++i) {
            if (atoms[i] == property_atom->atom)
                goto cleanup;
        }
    }
    result = 1;

cleanup:
    if (property_created)
        xcb_randr_delete_output_property(connection, output_id,
                                         property_atom->atom);
    if (!result)
        fprintf(stderr, "RANDR acceptance failed while %s\n", stage);
    free(error);
    free(property);
    free(properties);
    free(property_atom);
    free(crtc);
    free(output);
    free(resources);
    free(version);
    return result;
}

static int
wait_for_selection(xcb_connection_t *connection, uint8_t first_event,
                   xcb_window_t observer, xcb_window_t owner,
                   xcb_atom_t selection)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            uint8_t type = event->response_type & 0x7fU;

            if (type == first_event + XCB_XFIXES_SELECTION_NOTIFY) {
                xcb_xfixes_selection_notify_event_t *notify =
                    (xcb_xfixes_selection_notify_event_t *) event;

                if (notify->subtype ==
                        XCB_XFIXES_SELECTION_EVENT_SET_SELECTION_OWNER &&
                    notify->window == observer && notify->owner == owner &&
                    notify->selection == selection) {
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

static int
wait_for_cursor(xcb_connection_t *connection, uint8_t first_event,
                xcb_window_t window)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            uint8_t type = event->response_type & 0x7fU;

            if (type == first_event + XCB_XFIXES_CURSOR_NOTIFY) {
                xcb_xfixes_cursor_notify_event_t *notify =
                    (xcb_xfixes_cursor_notify_event_t *) event;

                if (notify->subtype == XCB_XFIXES_CURSOR_NOTIFY_DISPLAY_CURSOR &&
                    notify->window == window && notify->cursor_serial != 0) {
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

static int
test_xfixes(xcb_connection_t *connection, xcb_screen_t *screen)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection, &xcb_xfixes_id);
    xcb_generic_error_t *error = NULL;
    xcb_xfixes_query_version_reply_t *version = xcb_xfixes_query_version_reply(
        connection, xcb_xfixes_query_version(connection, 5, 0), &error);
    xcb_xfixes_fetch_region_reply_t *fetched = NULL;
    xcb_intern_atom_reply_t *clipboard = NULL;
    xcb_xfixes_region_t region = xcb_generate_id(connection);
    xcb_window_t owner = xcb_generate_id(connection);
    xcb_pixmap_t cursor_source = XCB_NONE;
    xcb_pixmap_t cursor_mask = XCB_NONE;
    xcb_cursor_t cursor = XCB_NONE;
    const xcb_rectangle_t rectangle = { 3, 4, 20, 10 };
    int result = 0;

    if (extension == NULL || !extension->present || error != NULL ||
        version == NULL || version->major_version < 5)
        goto cleanup;
    if (!checked(connection,
                 xcb_xfixes_create_region_checked(connection, region, 1,
                                                   &rectangle),
                 "XFIXES CreateRegion"))
        goto cleanup;
    fetched = xcb_xfixes_fetch_region_reply(
        connection, xcb_xfixes_fetch_region(connection, region), &error);
    if (error != NULL || fetched == NULL ||
        xcb_xfixes_fetch_region_rectangles_length(fetched) != 1 ||
        fetched->extents.x != rectangle.x ||
        fetched->extents.y != rectangle.y ||
        fetched->extents.width != rectangle.width ||
        fetched->extents.height != rectangle.height)
        goto cleanup;
    if (!checked(connection,
                 xcb_xfixes_destroy_region_checked(connection, region),
                 "XFIXES DestroyRegion"))
        goto cleanup;
    region = XCB_NONE;
    clipboard = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, 0, 9, "CLIPBOARD"), &error);
    if (error != NULL || clipboard == NULL || clipboard->atom == XCB_NONE)
        goto cleanup;
    xcb_create_window(connection, screen->root_depth, owner, screen->root,
                      0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    if (!checked(connection,
                 xcb_xfixes_select_selection_input_checked(
                     connection, screen->root, clipboard->atom,
                     XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER),
                 "XFIXES SelectSelectionInput") ||
        !checked(connection,
                 xcb_set_selection_owner_checked(connection, owner,
                                                 clipboard->atom,
                                                 XCB_CURRENT_TIME),
                 "Set CLIPBOARD owner") ||
        !wait_for_selection(connection, extension->first_event, screen->root,
                            owner, clipboard->atom))
        goto cleanup;
    cursor_source = xcb_generate_id(connection);
    cursor_mask = xcb_generate_id(connection);
    cursor = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_pixmap_checked(connection, 1, cursor_source,
                                           screen->root, 1, 1),
                 "Create cursor source") ||
        !checked(connection,
                 xcb_create_pixmap_checked(connection, 1, cursor_mask,
                                           screen->root, 1, 1),
                 "Create cursor mask") ||
        !checked(connection,
                 xcb_create_cursor_checked(connection, cursor, cursor_source,
                                           cursor_mask, 0xffff, 0xffff, 0xffff,
                                           0, 0, 0, 0, 0),
                 "Create cursor") ||
        !checked(connection,
                 xcb_xfixes_select_cursor_input_checked(
                     connection, screen->root,
                     XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR),
                 "XFIXES SelectCursorInput") ||
        !checked(connection,
                 xcb_change_window_attributes_checked(
                     connection, screen->root, XCB_CW_CURSOR,
                     (const uint32_t *) &cursor),
                 "Change root cursor") ||
        !wait_for_cursor(connection, extension->first_event, screen->root))
        goto cleanup;
    result = 1;

cleanup:
    if (cursor != XCB_NONE) {
        uint32_t inherited_cursor = XCB_NONE;

        xcb_change_window_attributes(connection, screen->root, XCB_CW_CURSOR,
                                     &inherited_cursor);
        xcb_free_cursor(connection, cursor);
    }
    if (cursor_mask != XCB_NONE)
        xcb_free_pixmap(connection, cursor_mask);
    if (cursor_source != XCB_NONE)
        xcb_free_pixmap(connection, cursor_source);
    if (clipboard != NULL && clipboard->atom != XCB_NONE)
        xcb_set_selection_owner(connection, XCB_NONE, clipboard->atom,
                                XCB_CURRENT_TIME);
    xcb_destroy_window(connection, owner);
    if (region != XCB_NONE)
        xcb_xfixes_destroy_region(connection, region);
    if (!result)
        fprintf(stderr, "XFIXES region, selection, or cursor behavior failed\n");
    free(error);
    free(clipboard);
    free(fetched);
    free(version);
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

    if (error != NULL || version == NULL || version->major_version < 1)
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
    if (!result)
        fprintf(stderr, "SHAPE version or bounding-region query failed\n");
    free(error);
    free(extents);
    free(version);
    return result;
}

static int
test_sync(xcb_connection_t *connection)
{
    xcb_generic_error_t *error = NULL;
    xcb_sync_initialize_reply_t *version = xcb_sync_initialize_reply(
        connection, xcb_sync_initialize(connection, 3, 1), &error);
    xcb_sync_query_counter_reply_t *queried = NULL;
    xcb_sync_counter_t counter = xcb_generate_id(connection);
    const xcb_sync_int64_t initial = { 1, 0x23456789U };
    int result = 0;

    if (error != NULL || version == NULL || version->major_version < 3)
        goto cleanup;
    if (!checked(connection,
                 xcb_sync_create_counter_checked(connection, counter, initial),
                 "SYNC CreateCounter"))
        goto cleanup;
    queried = xcb_sync_query_counter_reply(
        connection, xcb_sync_query_counter(connection, counter), &error);
    if (error != NULL || queried == NULL ||
        queried->counter_value.hi != initial.hi ||
        queried->counter_value.lo != initial.lo)
        goto cleanup;
    if (!checked(connection,
                 xcb_sync_destroy_counter_checked(connection, counter),
                 "SYNC DestroyCounter"))
        goto cleanup;
    counter = XCB_NONE;
    result = 1;

cleanup:
    if (counter != XCB_NONE)
        xcb_sync_destroy_counter(connection, counter);
    if (!result)
        fprintf(stderr, "SYNC version or counter round trip failed\n");
    free(error);
    free(queried);
    free(version);
    return result;
}

static int
test_input(xcb_connection_t *connection)
{
    xcb_generic_error_t *error = NULL;
    xcb_input_xi_query_version_reply_t *version =
        xcb_input_xi_query_version_reply(
            connection, xcb_input_xi_query_version(connection, 2, 3), &error);
    xcb_input_xi_query_device_reply_t *devices = NULL;
    int result = 0;

    if (error != NULL || version == NULL || version->major_version != 2 ||
        version->minor_version < 3)
        goto cleanup;
    devices = xcb_input_xi_query_device_reply(
        connection,
        xcb_input_xi_query_device(connection, XCB_INPUT_DEVICE_ALL_MASTER),
        &error);
    if (error != NULL || devices == NULL || devices->num_infos < 2)
        goto cleanup;
    result = 1;

cleanup:
    if (!result)
        fprintf(stderr, "XInput2 version or master-device query failed\n");
    free(error);
    free(devices);
    free(version);
    return result;
}

static int
test_xkb(xcb_connection_t *connection)
{
    xcb_generic_error_t *error = NULL;
    xcb_xkb_use_extension_reply_t *version = xcb_xkb_use_extension_reply(
        connection, xcb_xkb_use_extension(connection, 1, 0), &error);
    xcb_xkb_get_state_reply_t *state = NULL;
    uint8_t initial_locked = 0;
    uint8_t toggled_locked = 0;
    int restore_needed = 0;
    int result = 0;

    if (error != NULL || version == NULL || !version->supported ||
        version->serverMajor != 1)
        goto cleanup;
    state = xcb_xkb_get_state_reply(
        connection,
        xcb_xkb_get_state(connection, XCB_XKB_ID_USE_CORE_KBD), &error);
    if (error != NULL || state == NULL || state->deviceID == 0)
        goto cleanup;
    initial_locked = state->lockedMods;
    toggled_locked = initial_locked ^ XCB_MOD_MASK_LOCK;
    free(state);
    state = NULL;
    if (!checked(connection,
                 xcb_xkb_latch_lock_state_checked(
                     connection, XCB_XKB_ID_USE_CORE_KBD, XCB_MOD_MASK_LOCK,
                     toggled_locked, 0, 0, 0, 0, 0),
                 "XKB LatchLockState"))
        goto cleanup;
    restore_needed = 1;
    state = xcb_xkb_get_state_reply(
        connection,
        xcb_xkb_get_state(connection, XCB_XKB_ID_USE_CORE_KBD), &error);
    if (error != NULL || state == NULL ||
        (state->lockedMods & XCB_MOD_MASK_LOCK) !=
            (toggled_locked & XCB_MOD_MASK_LOCK) ||
        (state->mods & XCB_MOD_MASK_LOCK) !=
            (toggled_locked & XCB_MOD_MASK_LOCK))
        goto cleanup;
    if (!checked(connection,
                 xcb_xkb_latch_lock_state_checked(
                     connection, XCB_XKB_ID_USE_CORE_KBD, XCB_MOD_MASK_LOCK,
                     initial_locked, 0, 0, 0, 0, 0),
                 "XKB restore LatchLockState"))
        goto cleanup;
    restore_needed = 0;
    free(state);
    state = xcb_xkb_get_state_reply(
        connection,
        xcb_xkb_get_state(connection, XCB_XKB_ID_USE_CORE_KBD), &error);
    if (error != NULL || state == NULL ||
        (state->lockedMods & XCB_MOD_MASK_LOCK) !=
            (initial_locked & XCB_MOD_MASK_LOCK))
        goto cleanup;
    result = 1;

cleanup:
    if (restore_needed) {
        xcb_xkb_latch_lock_state(connection, XCB_XKB_ID_USE_CORE_KBD,
                                 XCB_MOD_MASK_LOCK, initial_locked,
                                 0, 0, 0, 0, 0);
    }
    if (!result)
        fprintf(stderr, "XKB version or reversible lock-state test failed\n");
    free(error);
    free(state);
    free(version);
    return result;
}

static int
wait_for_test_keys(xcb_connection_t *connection)
{
    int saw_a_press = 0;
    int saw_a_release = 0;
    int saw_b_press = 0;
    int saw_b_release = 0;
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            uint8_t type = event->response_type & 0x7fU;

            if (type == XCB_KEY_PRESS) {
                uint8_t detail = ((xcb_key_press_event_t *) event)->detail;

                if (detail == 38)
                    saw_a_press = 1;
                else if (detail == 56)
                    saw_b_press = 1;
            }
            else if (type == XCB_KEY_RELEASE) {
                uint8_t detail = ((xcb_key_release_event_t *) event)->detail;

                if (detail == 38)
                    saw_a_release = 1;
                else if (detail == 56)
                    saw_b_release = 1;
            }
            free(event);
        }
        if (saw_a_press && saw_a_release && saw_b_press && saw_b_release)
            return 1;
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

static int
test_xtest(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_test_get_version_reply_t *version = xcb_test_get_version_reply(
        connection, xcb_test_get_version(connection, 2, 2), &error);
    xcb_query_pointer_reply_t *pointer = NULL;
    xcb_get_keyboard_mapping_reply_t *mapping_a = NULL;
    xcb_get_keyboard_mapping_reply_t *mapping_b = NULL;
    xcb_window_t window = XCB_NONE;
    uint32_t event_mask = XCB_EVENT_MASK_KEY_PRESS |
        XCB_EVENT_MASK_KEY_RELEASE;
    int result = 0;

    if (error != NULL || version == NULL || version->major_version < 2)
        goto cleanup;
    if (!checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
                     screen->root, 17, 19, 0),
                 "XTEST FakeInput"))
        goto cleanup;
    pointer = xcb_query_pointer_reply(
        connection, xcb_query_pointer(connection, screen->root), &error);
    if (error != NULL || pointer == NULL || pointer->root_x != 17 ||
        pointer->root_y != 19)
        goto cleanup;

    mapping_a = xcb_get_keyboard_mapping_reply(
        connection, xcb_get_keyboard_mapping(connection, 38, 1), &error);
    mapping_b = xcb_get_keyboard_mapping_reply(
        connection, xcb_get_keyboard_mapping(connection, 56, 1), &error);
    if (error != NULL || mapping_a == NULL || mapping_b == NULL ||
        xcb_get_keyboard_mapping_keysyms_length(mapping_a) < 2 ||
        xcb_get_keyboard_mapping_keysyms(mapping_a)[0] != 0x00000061U ||
        xcb_get_keyboard_mapping_keysyms(mapping_a)[1] != 0x00000041U ||
        xcb_get_keyboard_mapping_keysyms_length(mapping_b) < 2 ||
        xcb_get_keyboard_mapping_keysyms(mapping_b)[0] != 0x00000062U ||
        xcb_get_keyboard_mapping_keysyms(mapping_b)[1] != 0x00000042U)
        goto cleanup;
    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_EVENT_MASK, &event_mask);
    xcb_map_window(connection, window);
    if (!checked(connection,
                 xcb_set_input_focus_checked(
                     connection, XCB_INPUT_FOCUS_POINTER_ROOT, window,
                     XCB_CURRENT_TIME),
                 "SetInputFocus") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_PRESS, 38, XCB_CURRENT_TIME,
                     XCB_NONE, 0, 0, 0),
                 "XTEST key press") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_RELEASE, 38, XCB_CURRENT_TIME,
                     XCB_NONE, 0, 0, 0),
                 "XTEST a release") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_PRESS, 56, XCB_CURRENT_TIME,
                     XCB_NONE, 0, 0, 0),
                 "XTEST b press") ||
        !checked(connection,
                 xcb_test_fake_input_checked(
                     connection, XCB_KEY_RELEASE, 56, XCB_CURRENT_TIME,
                     XCB_NONE, 0, 0, 0),
                 "XTEST b release") ||
        !wait_for_test_keys(connection))
        goto cleanup;
    result = 1;

cleanup:
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    if (!result)
        fprintf(stderr, "XTEST pointer/key injection or US keymap failed\n");
    free(error);
    free(mapping_b);
    free(mapping_a);
    free(pointer);
    free(version);
    return result;
}

static int
wait_for_damage(xcb_connection_t *connection, uint8_t first_event,
                xcb_damage_damage_t damage, xcb_drawable_t drawable)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            uint8_t type = event->response_type & 0x7fU;

            if (type == first_event + XCB_DAMAGE_NOTIFY) {
                xcb_damage_notify_event_t *notify =
                    (xcb_damage_notify_event_t *) event;

                if (notify->damage == damage && notify->drawable == drawable &&
                    notify->area.width != 0 && notify->area.height != 0) {
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

static int
test_composite_damage(xcb_connection_t *connection, xcb_screen_t *screen)
{
    const xcb_query_extension_reply_t *damage_extension =
        xcb_get_extension_data(connection, &xcb_damage_id);
    xcb_generic_error_t *error = NULL;
    xcb_composite_query_version_reply_t *composite =
        xcb_composite_query_version_reply(
            connection, xcb_composite_query_version(connection, 0, 4),
            &error);
    xcb_damage_query_version_reply_t *damage_version = NULL;
    xcb_window_t window = xcb_generate_id(connection);
    xcb_gcontext_t gc = XCB_NONE;
    xcb_damage_damage_t damage = XCB_NONE;
    xcb_rectangle_t rectangle = { 3, 4, 12, 9 };
    uint32_t foreground = 0x00ff0000U;
    int redirected = 0;
    int result = 0;

    if (damage_extension == NULL || !damage_extension->present ||
        error != NULL || composite == NULL || composite->major_version != 0 ||
        composite->minor_version < 4)
        goto cleanup;
    damage_version = xcb_damage_query_version_reply(
        connection, xcb_damage_query_version(connection, 1, 1), &error);
    if (error != NULL || damage_version == NULL ||
        damage_version->major_version < 1)
        goto cleanup;
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    if (!checked(connection, xcb_map_window_checked(connection, window),
                 "Map DAMAGE window"))
        goto cleanup;
    if (!checked(connection,
                 xcb_composite_redirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "COMPOSITE RedirectWindow"))
        goto cleanup;
    redirected = 1;
    damage = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_damage_create_checked(
                     connection, damage, window,
                     XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY),
                 "DAMAGE Create"))
        goto cleanup;
    if (!checked(connection,
                 xcb_damage_subtract_checked(connection, damage, XCB_NONE,
                                             XCB_NONE),
                 "DAMAGE Subtract"))
        goto cleanup;
    gc = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_create_gc_checked(connection, gc, window,
                                       XCB_GC_FOREGROUND, &foreground),
                 "Create DAMAGE GC") ||
        !checked(connection,
                 xcb_poly_fill_rectangle_checked(connection, window, gc, 1,
                                                 &rectangle),
                 "Draw DAMAGE rectangle") ||
        !wait_for_damage(connection, damage_extension->first_event, damage,
                         window) ||
        !checked(connection,
                 xcb_damage_subtract_checked(connection, damage, XCB_NONE,
                                             XCB_NONE),
                 "DAMAGE Subtract notification") ||
        !checked(connection, xcb_damage_destroy_checked(connection, damage),
                 "DAMAGE Destroy"))
        goto cleanup;
    damage = XCB_NONE;
    if (!checked(connection,
                 xcb_composite_unredirect_window_checked(
                     connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                 "COMPOSITE UnredirectWindow"))
        goto cleanup;
    redirected = 0;
    result = 1;

cleanup:
    if (gc != XCB_NONE)
        xcb_free_gc(connection, gc);
    if (damage != XCB_NONE)
        xcb_damage_destroy(connection, damage);
    if (redirected)
        xcb_composite_unredirect_window(
            connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
    xcb_destroy_window(connection, window);
    if (!result)
        fprintf(stderr, "COMPOSITE or DAMAGE resource round trip failed\n");
    free(error);
    free(damage_version);
    free(composite);
    return result;
}

static int
wait_for_present_pixmap(xcb_connection_t *connection, uint8_t opcode,
                        xcb_present_event_t event_id, uint32_t serial,
                        xcb_pixmap_t pixmap, uint64_t *msc)
{
    int saw_complete = 0;
    int saw_idle = 0;
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            if ((event->response_type & 0x7fU) == XCB_GE_GENERIC) {
                xcb_present_generic_event_t *generic =
                    (xcb_present_generic_event_t *) event;

                if (generic->extension == opcode &&
                    generic->event == event_id) {
                    if (generic->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
                        xcb_present_complete_notify_event_t *complete =
                            (xcb_present_complete_notify_event_t *) event;

                        if (complete->serial == serial &&
                            complete->kind ==
                                XCB_PRESENT_COMPLETE_KIND_PIXMAP &&
                            complete->mode == XCB_PRESENT_COMPLETE_MODE_COPY &&
                            complete->ust != 0) {
                            *msc = complete->msc;
                            saw_complete = 1;
                        }
                    }
                    else if (generic->evtype == XCB_PRESENT_IDLE_NOTIFY) {
                        xcb_present_idle_notify_event_t *idle =
                            (xcb_present_idle_notify_event_t *) event;

                        if (idle->serial == serial && idle->pixmap == pixmap)
                            saw_idle = 1;
                    }
                }
            }
            free(event);
        }
        if (saw_complete && saw_idle)
            return 1;
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

static int
wait_for_present_msc(xcb_connection_t *connection, uint8_t opcode,
                     xcb_present_event_t event_id, uint32_t serial,
                     uint64_t minimum_msc)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        xcb_generic_event_t *event;

        while ((event = xcb_poll_for_event(connection)) != NULL) {
            if ((event->response_type & 0x7fU) == XCB_GE_GENERIC) {
                xcb_present_complete_notify_event_t *complete =
                    (xcb_present_complete_notify_event_t *) event;

                if (complete->extension == opcode &&
                    complete->event_type == XCB_PRESENT_COMPLETE_NOTIFY &&
                    complete->event == event_id && complete->serial == serial &&
                    complete->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC &&
                    complete->msc >= minimum_msc && complete->ust != 0) {
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

static int
test_present(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_present_query_version_reply_t *version =
        xcb_present_query_version_reply(
            connection, xcb_present_query_version(connection, 1, 2), &error);
    xcb_present_query_capabilities_reply_t *capabilities = NULL;
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection, &xcb_present_id);
    xcb_window_t window = XCB_NONE;
    xcb_pixmap_t pixmap = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    xcb_present_event_t event_id = XCB_NONE;
    xcb_get_image_reply_t *image = NULL;
    uint32_t green = 0x0000ff00U;
    uint32_t pixel = 0;
    const uint32_t serial = 0x584d494eU;
    const uint32_t msc_serial = 0x4d53434eU;
    uint64_t presented_msc = 0;
    int result = 0;

    if (error != NULL || version == NULL || version->major_version < 1)
        goto cleanup;
    capabilities = xcb_present_query_capabilities_reply(
        connection,
        xcb_present_query_capabilities(connection, screen->root), &error);
    if (error != NULL || capabilities == NULL || extension == NULL ||
        !extension->present ||
        capabilities->capabilities != XCB_PRESENT_CAPABILITY_NONE)
        goto cleanup;

    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    xcb_map_window(connection, window);
    pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, screen->root_depth, pixmap, screen->root,
                      32, 24);
    graphics = xcb_generate_id(connection);
    xcb_create_gc(connection, graphics, pixmap, XCB_GC_FOREGROUND, &green);
    {
        const xcb_rectangle_t rectangle = { 0, 0, 32, 24 };

        xcb_poly_fill_rectangle(connection, pixmap, graphics, 1, &rectangle);
    }
    event_id = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_present_select_input_checked(
                     connection, event_id, window,
                     XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                     XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY),
                 "Present SelectInput") ||
        !checked(connection,
                 xcb_present_pixmap_checked(
                     connection, window, pixmap, serial, XCB_NONE, XCB_NONE,
                     0, 0, XCB_NONE, XCB_NONE, XCB_NONE, 0, 0, 0, 0, 0,
                     NULL),
                 "Present Pixmap") ||
        !wait_for_present_pixmap(connection, extension->major_opcode, event_id,
                                 serial, pixmap, &presented_msc) ||
        !checked(connection,
                 xcb_present_notify_msc_checked(
                     connection, window, msc_serial, presented_msc + 1, 0, 0),
                 "Present NotifyMSC") ||
        !wait_for_present_msc(connection, extension->major_opcode, event_id,
                              msc_serial, presented_msc + 1))
        goto cleanup;
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      16, 12, 1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x0000ff00U)
        goto cleanup;
    result = 1;

cleanup:
    if (graphics != XCB_NONE)
        xcb_free_gc(connection, graphics);
    if (pixmap != XCB_NONE)
        xcb_free_pixmap(connection, pixmap);
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    if (!result)
        fprintf(stderr,
                "Present pixmap/complete/idle/MSC notification or readback failed\n");
    free(error);
    free(image);
    free(capabilities);
    free(version);
    return result;
}

static int
test_dbe(xcb_connection_t *connection, xcb_screen_t *screen)
{
    xcb_generic_error_t *error = NULL;
    xcb_dbe_query_version_reply_t *version = xcb_dbe_query_version_reply(
        connection, xcb_dbe_query_version(connection, 1, 0), &error);
    xcb_get_image_reply_t *image = NULL;
    xcb_window_t window = XCB_NONE;
    xcb_dbe_back_buffer_t back = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    uint32_t blue = 0x000000ffU;
    uint32_t pixel = 0;
    int allocated = 0;
    int result = 0;

    if (error != NULL || version == NULL || version->major_version < 1)
        goto cleanup;
    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 32, 24, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    xcb_map_window(connection, window);
    back = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_dbe_allocate_back_buffer_checked(
                     connection, window, back, XCB_DBE_SWAP_ACTION_COPIED),
                 "DBE AllocateBackBuffer"))
        goto cleanup;
    allocated = 1;
    graphics = xcb_generate_id(connection);
    xcb_create_gc(connection, graphics, back, XCB_GC_FOREGROUND, &blue);
    {
        const xcb_rectangle_t rectangle = { 0, 0, 32, 24 };
        const xcb_dbe_swap_info_t swap = {
            .window = window,
            .swap_action = XCB_DBE_SWAP_ACTION_COPIED
        };

        xcb_poly_fill_rectangle(connection, back, graphics, 1, &rectangle);
        if (!checked(connection,
                     xcb_dbe_swap_buffers_checked(connection, 1, &swap),
                     "DBE SwapBuffers"))
            goto cleanup;
    }
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      16, 12, 1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x000000ffU)
        goto cleanup;
    if (!checked(connection,
                 xcb_dbe_deallocate_back_buffer_checked(connection, back),
                 "DBE DeallocateBackBuffer"))
        goto cleanup;
    allocated = 0;
    result = 1;

cleanup:
    if (graphics != XCB_NONE)
        xcb_free_gc(connection, graphics);
    if (allocated)
        xcb_dbe_deallocate_back_buffer(connection, back);
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    if (!result)
        fprintf(stderr, "DBE allocate/draw/swap/readback failed\n");
    free(error);
    free(image);
    free(version);
    return result;
}

#if XMIN_HAVE_MITSHM
static int
test_shm(xcb_connection_t *connection, xcb_screen_t *screen)
{
    enum { WIDTH = 32, HEIGHT = 24, BYTE_COUNT = WIDTH * HEIGHT * 4 };
    xcb_generic_error_t *error = NULL;
    xcb_shm_query_version_reply_t *version = xcb_shm_query_version_reply(
        connection, xcb_shm_query_version(connection), &error);
    xcb_get_image_reply_t *image = NULL;
    xcb_shm_seg_t segment = XCB_NONE;
    xcb_window_t window = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    int shmid = -1;
    uint32_t *pixels = (void *) -1;
    uint32_t pixel = 0;
    int attached = 0;
    int result = 0;
    size_t i;
    const char *stage = "querying the MIT-SHM version";

    if (error != NULL || version == NULL || version->major_version != 1)
        goto cleanup;
    stage = "allocating the SysV segment";
    shmid = shmget(IPC_PRIVATE, BYTE_COUNT, IPC_CREAT | 0600);
    if (shmid < 0)
        goto cleanup;
    stage = "mapping the SysV segment in the client";
    pixels = shmat(shmid, NULL, 0);
    if (pixels == (void *) -1)
        goto cleanup;
    for (i = 0; i < (size_t) WIDTH * HEIGHT; ++i)
        pixels[i] = 0x0000ff00U;

    stage = "attaching the SysV segment to Xmin";
    segment = xcb_generate_id(connection);
    if (!checked(connection,
                 xcb_shm_attach_checked(connection, segment, (uint32_t) shmid,
                                        0),
                 "MIT-SHM Attach"))
        goto cleanup;
    attached = 1;
    stage = "marking the attached segment for deletion";
    if (shmctl(shmid, IPC_RMID, NULL) != 0)
        goto cleanup;
    shmid = -1;

    stage = "creating the MIT-SHM destination drawable";
    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, WIDTH, HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    graphics = xcb_generate_id(connection);
    xcb_create_gc(connection, graphics, window, 0, NULL);
    xcb_map_window(connection, window);
    stage = "uploading with MIT-SHM PutImage";
    if (!checked(connection,
                 xcb_shm_put_image_checked(
                     connection, window, graphics, WIDTH, HEIGHT, 0, 0,
                     WIDTH, HEIGHT, 0, 0, screen->root_depth,
                     XCB_IMAGE_FORMAT_Z_PIXMAP, 0, segment, 0),
                 "MIT-SHM PutImage"))
        goto cleanup;
    stage = "reading back the MIT-SHM upload";
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      WIDTH / 2, HEIGHT / 2, 1, 1, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x0000ff00U) {
        fprintf(stderr, "unexpected MIT-SHM pixel: 0x%08x\n", pixel);
        goto cleanup;
    }
    stage = "detaching the SysV segment";
    if (!checked(connection, xcb_shm_detach_checked(connection, segment),
                 "MIT-SHM Detach"))
        goto cleanup;
    attached = 0;
    result = 1;

cleanup:
    if (attached)
        xcb_shm_detach(connection, segment);
    if (graphics != XCB_NONE)
        xcb_free_gc(connection, graphics);
    if (window != XCB_NONE)
        xcb_destroy_window(connection, window);
    if (pixels != (void *) -1)
        shmdt(pixels);
    if (shmid >= 0)
        shmctl(shmid, IPC_RMID, NULL);
    if (!result)
        fprintf(stderr, "MIT-SHM test failed while %s\n", stage);
    free(error);
    free(image);
    free(version);
    return result;
}
#endif

int
main(void)
{
    xcb_connection_t *connection;
    xcb_screen_iterator_t screens;
    xcb_screen_t *screen;
    int screen_number = 0;
    int result = 1;

    connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection)) {
        fprintf(stderr, "XCB extension client could not open Xmin\n");
        return 2;
    }
    screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0 && screens.rem != 0)
        xcb_screen_next(&screens);
    if (screens.rem == 0)
        goto cleanup;
    screen = screens.data;

    if (!test_foundation_extensions(connection) ||
        !test_compatibility_queries(connection, screen) ||
        !test_render(connection, screen) ||
        !test_randr(connection, screen->root) ||
        !test_xfixes(connection, screen) || !test_shape(connection, screen) ||
        !test_sync(connection) || !test_input(connection) ||
        !test_xkb(connection) || !test_xtest(connection, screen) ||
        !test_composite_damage(connection, screen) ||
        !test_present(connection, screen) || !test_dbe(connection, screen)
#if XMIN_HAVE_MITSHM
        || !test_shm(connection, screen)
#endif
    )
        goto cleanup;
    result = 0;

cleanup:
    xcb_flush(connection);
    xcb_disconnect(connection);
    return result;
}
