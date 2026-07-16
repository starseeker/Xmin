#include "xmin/config.h"

#include <xcb/xcb.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
query_extension(xcb_connection_t *connection, const char *name)
{
    xcb_query_extension_cookie_t cookie = xcb_query_extension(
        connection, (uint16_t) strlen(name), name);
    xcb_query_extension_reply_t *reply = xcb_query_extension_reply(
        connection, cookie, NULL);
    int present = reply != NULL && reply->present;

    free(reply);
    return present;
}

static int
test_exact_extension_profile(xcb_connection_t *connection,
                             const char *const *expected,
                             size_t expected_count)
{
    xcb_generic_error_t *error = NULL;
    xcb_list_extensions_reply_t *reply = xcb_list_extensions_reply(
        connection, xcb_list_extensions(connection), &error);
    unsigned char *seen = calloc(expected_count, sizeof(*seen));
    xcb_str_iterator_t names;
    int result = 0;

    if (error != NULL || reply == NULL || seen == NULL ||
        (size_t) xcb_list_extensions_names_length(reply) != expected_count)
        goto cleanup;
    names = xcb_list_extensions_names_iterator(reply);
    while (names.rem != 0) {
        const char *name = xcb_str_name(names.data);
        size_t name_length = (size_t) xcb_str_name_length(names.data);
        size_t i;

        for (i = 0; i < expected_count; ++i) {
            if (!seen[i] && strlen(expected[i]) == name_length &&
                memcmp(expected[i], name, name_length) == 0) {
                seen[i] = 1;
                break;
            }
        }
        if (i == expected_count) {
            fprintf(stderr, "Xmin advertised unexpected extension: %.*s\n",
                    (int) name_length, name);
            goto cleanup;
        }
        xcb_str_next(&names);
    }
    for (size_t i = 0; i < expected_count; ++i) {
        if (!seen[i]) {
            fprintf(stderr, "Xmin omitted expected extension: %s\n",
                    expected[i]);
            goto cleanup;
        }
    }
    result = 1;

cleanup:
    if (!result && error != NULL)
        fprintf(stderr, "ListExtensions failed with X11 error %u\n",
                error->error_code);
    else if (!result && reply != NULL && seen != NULL &&
             (size_t) xcb_list_extensions_names_length(reply) !=
                 expected_count)
        fprintf(stderr, "Xmin advertised %d extensions; expected %zu\n",
                xcb_list_extensions_names_length(reply), expected_count);
    free(seen);
    free(error);
    free(reply);
    return result;
}

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
font_is_listed(xcb_connection_t *connection, const char *name)
{
    xcb_generic_error_t *error = NULL;
    xcb_list_fonts_reply_t *reply = xcb_list_fonts_reply(
        connection,
        xcb_list_fonts(connection, 8, (uint16_t) strlen(name), name),
        &error);
    xcb_str_iterator_t names;
    int found = 0;

    if (error == NULL && reply != NULL) {
        names = xcb_list_fonts_names_iterator(reply);
        while (names.rem != 0) {
            int length = xcb_str_name_length(names.data);

            if ((size_t) length == strlen(name) &&
                memcmp(xcb_str_name(names.data), name, (size_t) length) == 0) {
                found = 1;
                break;
            }
            xcb_str_next(&names);
        }
    }
    free(error);
    free(reply);
    return found;
}

static int
test_builtin_fonts(xcb_connection_t *connection, xcb_window_t window,
                   xcb_gcontext_t graphics)
{
    static const char fixed_name[] = "fixed";
    static const char cursor_name[] = "cursor";
    static const char text[] = "A";
    xcb_generic_error_t *error = NULL;
    xcb_query_font_reply_t *fixed_info = NULL;
    xcb_query_font_reply_t *cursor_info = NULL;
    xcb_get_image_reply_t *image = NULL;
    xcb_font_t fixed = xcb_generate_id(connection);
    xcb_font_t cursor = xcb_generate_id(connection);
    uint32_t values[3] = { 0x00ff0000U, 0, fixed };
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t red_pixels = 0;
    uint32_t black_pixels = 0;
    uint32_t blue_pixels = 0;
    uint8_t cursor_character = 0;
    const char *stage = "listing built-in fonts";
    int fixed_open = 0;
    int cursor_open = 0;
    int result = 0;
    int i;

    if (!font_is_listed(connection, fixed_name) ||
        !font_is_listed(connection, cursor_name))
        goto cleanup;
    stage = "opening built-in fonts";
    if (!checked(connection,
                 xcb_open_font_checked(connection, fixed,
                                       sizeof(fixed_name) - 1, fixed_name),
                 "OpenFont fixed"))
        goto cleanup;
    fixed_open = 1;
    if (!checked(connection,
                 xcb_open_font_checked(connection, cursor,
                                       sizeof(cursor_name) - 1, cursor_name),
                 "OpenFont cursor"))
        goto cleanup;
    cursor_open = 1;

    stage = "querying built-in font metrics";
    fixed_info = xcb_query_font_reply(
        connection, xcb_query_font(connection, fixed), &error);
    cursor_info = xcb_query_font_reply(
        connection, xcb_query_font(connection, cursor), &error);
    if (error != NULL || fixed_info == NULL || cursor_info == NULL ||
        fixed_info->font_ascent <= 0 || fixed_info->font_descent < 0 ||
        fixed_info->max_bounds.character_width <= 0 ||
        fixed_info->min_char_or_byte2 > 'A' ||
        fixed_info->max_char_or_byte2 < 'A' ||
        cursor_info->min_byte1 != 0 || cursor_info->max_byte1 != 0 ||
        cursor_info->max_char_or_byte2 < cursor_info->min_char_or_byte2 ||
        cursor_info->min_char_or_byte2 > UINT8_MAX)
        goto cleanup;
    width = (uint16_t) fixed_info->max_bounds.character_width;
    height = (uint16_t) (fixed_info->font_ascent + fixed_info->font_descent);
    if (width == 0 || height == 0 || width > 32 || height > 32)
        goto cleanup;

    stage = "rendering core text";
    if (!checked(connection,
                 xcb_change_gc_checked(
                     connection, graphics,
                     XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
                     values),
                 "ChangeGC core font") ||
        !checked(connection,
                 xcb_image_text_8_checked(
                     connection, sizeof(text) - 1, window, graphics, 4,
                     (int16_t) (4 + fixed_info->font_ascent), text),
                 "ImageText8"))
        goto cleanup;
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      4, 4, width, height, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < (int) width * height * 4)
        goto cleanup;
    for (i = 0; i < (int) width * height; ++i) {
        uint32_t pixel;

        memcpy(&pixel, xcb_get_image_data(image) + (size_t) i * 4,
               sizeof(pixel));
        pixel &= 0x00ffffffU;
        if (pixel == 0x00ff0000U)
            ++red_pixels;
        else if (pixel == 0)
            ++black_pixels;
    }
    if (red_pixels == 0 || black_pixels == 0 ||
        red_pixels + black_pixels != (uint32_t) width * height)
        goto cleanup;

    stage = "rendering a cursor-font glyph";
    black_pixels = 0;
    cursor_character = (uint8_t) cursor_info->min_char_or_byte2;
    values[0] = 0x000000ffU;
    values[2] = cursor;
    if (!checked(connection,
                 xcb_change_gc_checked(
                     connection, graphics,
                     XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT,
                     values),
                 "ChangeGC cursor font") ||
        !checked(connection,
                 xcb_image_text_8_checked(
                     connection, 1, window, graphics, 20,
                     (int16_t) (4 + cursor_info->font_ascent),
                     (const char *) &cursor_character),
                 "ImageText8 cursor"))
        goto cleanup;
    free(image);
    image = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window,
                      16, 0, 32, 32, UINT32_MAX),
        &error);
    if (error != NULL || image == NULL ||
        xcb_get_image_data_length(image) < 32 * 32 * 4)
        goto cleanup;
    for (i = 0; i < 32 * 32; ++i) {
        uint32_t pixel;

        memcpy(&pixel, xcb_get_image_data(image) + (size_t) i * 4,
               sizeof(pixel));
        pixel &= 0x00ffffffU;
        if (pixel == 0x000000ffU)
            ++blue_pixels;
        else if (pixel == 0)
            ++black_pixels;
    }
    if (blue_pixels == 0 || black_pixels == 0)
        goto cleanup;
    result = 1;

cleanup:
    if (cursor_open)
        xcb_close_font(connection, cursor);
    if (fixed_open)
        xcb_close_font(connection, fixed);
    if (!result)
        fprintf(stderr, "built-in font acceptance failed while %s\n", stage);
    free(error);
    free(image);
    free(cursor_info);
    free(fixed_info);
    return result;
}

int
main(void)
{
    static const char *const required_extensions[] = {
        "Generic Event Extension",
        "SHAPE",
        "XInputExtension",
        "XTEST",
        "BIG-REQUESTS",
        "SYNC",
        "XKEYBOARD",
        "XC-MISC",
        "XFIXES",
        "RENDER",
        "RANDR",
        "Composite",
        "DAMAGE",
        "MIT-SCREEN-SAVER",
        "DOUBLE-BUFFER",
        "Present",
        "XINERAMA",
#if XMIN_BUILD_GLX
        "GLX",
#endif
#if XMIN_HAVE_MITSHM
        "MIT-SHM",
#endif
    };
    xcb_connection_t *connection;
    const xcb_setup_t *setup;
    xcb_screen_iterator_t screens;
    xcb_screen_t *screen;
    xcb_window_t window = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    xcb_rectangle_t rectangle = { 0, 0, 64, 48 };
    xcb_get_image_reply_t *image = NULL;
    uint32_t window_values[2] = { 0, XCB_EVENT_MASK_EXPOSURE };
    uint32_t foreground = 0x0000ff00U;
    uint32_t pixel = 0;
    size_t i;
    int screen_number = 0;
    int result = 1;

    connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection)) {
        fprintf(stderr, "XCB could not open the authenticated Xmin display\n");
        return 2;
    }
    setup = xcb_get_setup(connection);
    screens = xcb_setup_roots_iterator(setup);
    for (i = 0; i < (size_t) screen_number && screens.rem != 0; ++i)
        xcb_screen_next(&screens);
    if (screens.rem == 0)
        goto cleanup;
    screen = screens.data;
    if (screen->width_in_pixels != 96 || screen->height_in_pixels != 80 ||
        screen->root_depth != 24)
        goto cleanup;

    for (i = 0; i < sizeof(required_extensions) /
                    sizeof(required_extensions[0]); ++i) {
        if (!query_extension(connection, required_extensions[i])) {
            fprintf(stderr, "XCB could not query required extension: %s\n",
                    required_extensions[i]);
            goto cleanup;
        }
    }
    if (!test_exact_extension_profile(
            connection, required_extensions,
            sizeof(required_extensions) / sizeof(required_extensions[0])))
        goto cleanup;

    window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 64, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, window_values);
    graphics = xcb_generate_id(connection);
    xcb_create_gc(connection, graphics, window, XCB_GC_FOREGROUND,
                  &foreground);
    xcb_map_window(connection, window);
    xcb_poly_fill_rectangle(connection, window, graphics, 1, &rectangle);
    {
        xcb_get_image_cookie_t cookie = xcb_get_image(
            connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 32, 24, 1, 1,
            UINT32_MAX);
        image = xcb_get_image_reply(connection, cookie, NULL);
    }
    if (image == NULL || xcb_get_image_data_length(image) < 4)
        goto cleanup;
    memcpy(&pixel, xcb_get_image_data(image), sizeof(pixel));
    if ((pixel & 0x00ffffffU) != 0x0000ff00U ||
        !test_builtin_fonts(connection, window, graphics))
        goto cleanup;
    result = 0;

cleanup:
    free(image);
    if (!xcb_connection_has_error(connection)) {
        if (graphics != XCB_NONE)
            xcb_free_gc(connection, graphics);
        if (window != XCB_NONE)
            xcb_destroy_window(connection, window);
        xcb_flush(connection);
    }
    xcb_disconnect(connection);
    return result;
}
