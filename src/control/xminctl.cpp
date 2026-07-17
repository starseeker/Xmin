#include "xmin_client.h"
#include "xmin_keysyms.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "xmin/config.h"

typedef struct controller {
    xmin_xcb_session session;
    xcb_atom_t net_wm_name;
    xcb_atom_t utf8_string;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
} controller;

typedef struct window_geometry {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t border_width;
    uint8_t depth;
} window_geometry;

typedef struct keyboard_map {
    xcb_get_keyboard_mapping_reply_t *reply;
    xcb_keysym_t *symbols;
    xcb_keycode_t minimum;
    uint8_t symbols_per_keycode;
    uint8_t count;
} keyboard_map;

static void
usage(FILE *stream)
{
    fprintf(stream,
            "usage: xminctl [--display :N] COMMAND [ARGUMENTS]\n"
            "\n"
            "Window commands:\n"
            "  list-windows                         list named windows\n"
            "  wait-window [--timeout MS] NAME      wait for a mapped window\n"
            "  find-window NAME                     print a matching window ID\n"
            "  geometry WINDOW                      print root-relative geometry\n"
            "  focus|raise|activate WINDOW          control a window\n"
            "  move WINDOW X Y                      move a window\n"
            "  resize WINDOW WIDTH HEIGHT           resize a window\n"
            "  map|unmap|close WINDOW               change window state\n"
            "\n"
            "Input commands (XTEST):\n"
            "  mouse-move WINDOW X Y                move relative to a window\n"
            "  click [--delay MS] WINDOW X Y [BUTTON]\n"
            "                                       move and click\n"
            "  mouse-drag [OPTIONS] WINDOW X1 Y1 X2 Y2\n"
            "                                       drag with --button N,\n"
            "                                       --steps N, --delay MS\n"
            "  button BUTTON down|up                change a mouse button\n"
            "  scroll AMOUNT                        wheel up (+) or down (-)\n"
            "  pointer                              print root pointer position\n"
            "  key CHORD                            type e.g. ctrl+shift+a\n"
            "  key-down|key-up KEY                  hold or release one key\n"
            "  type [--delay MS] TEXT               type ASCII text\n"
            "\n"
            "Observation commands:\n"
            "  wait-stable [--quiet MS] [--timeout MS] WINDOW\n"
            "  capture-root FILE.ppm\n"
            "  capture-window WINDOW FILE.ppm\n"
            "\n"
            "WINDOW is root, a numeric ID, or a substring of WM_NAME.\n");
}

static int64_t
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void
sleep_milliseconds(long milliseconds)
{
    struct timespec delay = {
        milliseconds / 1000,
        (milliseconds % 1000) * 1000 * 1000
    };

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static int
parse_long(const char *text, long minimum, long maximum, long *value)
{
    char *end = NULL;
    long result;

    errno = 0;
    result = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        result < minimum || result > maximum)
        return -1;
    *value = result;
    return 0;
}

static xcb_atom_t
intern_atom(controller *ctl, const char *name)
{
    xcb_intern_atom_reply_t *reply;
    xcb_atom_t atom = XCB_ATOM_NONE;

    reply = xcb_intern_atom_reply(
        ctl->session.connection,
        xcb_intern_atom(ctl->session.connection, false,
                        (uint16_t) strlen(name), name),
        NULL);
    if (reply != NULL) {
        atom = reply->atom;
        free(reply);
    }
    return atom;
}

static int
initialize_atoms(controller *ctl)
{
    ctl->net_wm_name = intern_atom(ctl, "_NET_WM_NAME");
    ctl->utf8_string = intern_atom(ctl, "UTF8_STRING");
    ctl->wm_protocols = intern_atom(ctl, "WM_PROTOCOLS");
    ctl->wm_delete_window = intern_atom(ctl, "WM_DELETE_WINDOW");
    if (ctl->net_wm_name == XCB_ATOM_NONE ||
        ctl->utf8_string == XCB_ATOM_NONE ||
        ctl->wm_protocols == XCB_ATOM_NONE ||
        ctl->wm_delete_window == XCB_ATOM_NONE) {
        fprintf(stderr, "xminctl: cannot initialize X11 atoms\n");
        return -1;
    }
    return 0;
}

static char *
read_text_property(controller *ctl, xcb_window_t window, xcb_atom_t property,
                   xcb_atom_t type)
{
    xcb_get_property_reply_t *reply;
    int length;
    char *text;

    reply = xcb_get_property_reply(
        ctl->session.connection,
        xcb_get_property(ctl->session.connection, false, window, property,
                         type, 0, 4096),
        NULL);
    if (reply == NULL)
        return NULL;
    length = xcb_get_property_value_length(reply);
    if (reply->type == XCB_ATOM_NONE || length <= 0) {
        free(reply);
        return NULL;
    }
    text = static_cast<char *>(malloc((size_t) length + 1));
    if (text != NULL) {
        memcpy(text, xcb_get_property_value(reply), (size_t) length);
        text[length] = '\0';
    }
    free(reply);
    return text;
}

static char *
window_name(controller *ctl, xcb_window_t window)
{
    char *name = read_text_property(ctl, window, ctl->net_wm_name,
                                    ctl->utf8_string);

    if (name == NULL)
        name = read_text_property(ctl, window, XCB_ATOM_WM_NAME,
                                  XCB_GET_PROPERTY_TYPE_ANY);
    return name;
}

static int
window_is_mapped(controller *ctl, xcb_window_t window)
{
    xcb_get_window_attributes_reply_t *reply;
    int mapped = 0;

    reply = xcb_get_window_attributes_reply(
        ctl->session.connection,
        xcb_get_window_attributes(ctl->session.connection, window), NULL);
    if (reply != NULL) {
        mapped = reply->map_state == XCB_MAP_STATE_VIEWABLE;
        free(reply);
    }
    return mapped;
}

typedef int (*window_visitor)(controller *, xcb_window_t, unsigned int, void *);

static int
walk_windows(controller *ctl, xcb_window_t parent, unsigned int depth,
             window_visitor visitor, void *context)
{
    xcb_query_tree_reply_t *reply;
    xcb_window_t *children;
    int child_count;
    int index;

    reply = xcb_query_tree_reply(
        ctl->session.connection,
        xcb_query_tree(ctl->session.connection, parent), NULL);
    if (reply == NULL)
        return 0;
    children = xcb_query_tree_children(reply);
    child_count = xcb_query_tree_children_length(reply);
    for (index = 0; index < child_count; ++index) {
        if (visitor(ctl, children[index], depth, context) != 0) {
            free(reply);
            return 1;
        }
        if (walk_windows(ctl, children[index], depth + 1,
                         visitor, context) != 0) {
            free(reply);
            return 1;
        }
    }
    free(reply);
    return 0;
}

static int
absolute_geometry(controller *ctl, xcb_window_t window,
                  window_geometry *geometry)
{
    xcb_get_geometry_reply_t *geometry_reply;
    xcb_translate_coordinates_reply_t *translate_reply = NULL;

    geometry_reply = xcb_get_geometry_reply(
        ctl->session.connection,
        xcb_get_geometry(ctl->session.connection, window), NULL);
    if (geometry_reply == NULL)
        return -1;
    geometry->width = geometry_reply->width;
    geometry->height = geometry_reply->height;
    geometry->border_width = geometry_reply->border_width;
    geometry->depth = geometry_reply->depth;
    geometry->x = geometry_reply->x;
    geometry->y = geometry_reply->y;
    free(geometry_reply);

    if (window != ctl->session.screen->root) {
        translate_reply = xcb_translate_coordinates_reply(
            ctl->session.connection,
            xcb_translate_coordinates(ctl->session.connection, window,
                                      ctl->session.screen->root, 0, 0),
            NULL);
        if (translate_reply == NULL)
            return -1;
        geometry->x = translate_reply->dst_x;
        geometry->y = translate_reply->dst_y;
        free(translate_reply);
    }
    return 0;
}

typedef struct find_context {
    const char *pattern;
    xcb_window_t substring;
    xcb_window_t exact;
    bool mapped_only;
} find_context;

static int
find_visitor(controller *ctl, xcb_window_t window, unsigned int depth,
             void *opaque)
{
    find_context *context = static_cast<find_context *>(opaque);
    char *name;

    (void) depth;
    if (context->mapped_only && !window_is_mapped(ctl, window))
        return 0;
    name = window_name(ctl, window);
    if (name == NULL)
        return 0;
    if (strcmp(name, context->pattern) == 0)
        context->exact = window;
    else if (context->substring == XCB_WINDOW_NONE &&
             strstr(name, context->pattern) != NULL)
        context->substring = window;
    free(name);
    return context->exact != XCB_WINDOW_NONE;
}

static xcb_window_t
find_named_window(controller *ctl, const char *pattern, bool mapped_only)
{
    find_context context;

    memset(&context, 0, sizeof(context));
    context.pattern = pattern;
    context.mapped_only = mapped_only;
    (void) walk_windows(ctl, ctl->session.screen->root, 0,
                        find_visitor, &context);
    return context.exact != XCB_WINDOW_NONE ? context.exact : context.substring;
}

static xcb_window_t
resolve_window(controller *ctl, const char *selector, bool mapped_only)
{
    char *end = NULL;
    unsigned long value;

    if (strcasecmp(selector, "root") == 0)
        return ctl->session.screen->root;
    errno = 0;
    value = strtoul(selector, &end, 0);
    if (errno == 0 && end != selector && *end == '\0' && value <= UINT32_MAX) {
        xcb_window_t window = (xcb_window_t) value;
        xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(
            ctl->session.connection,
            xcb_get_geometry(ctl->session.connection, window), NULL);

        if (reply == NULL || (mapped_only && !window_is_mapped(ctl, window))) {
            free(reply);
            return XCB_WINDOW_NONE;
        }
        free(reply);
        return window;
    }
    return find_named_window(ctl, selector, mapped_only);
}

static void
print_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *) name;

    while (*cursor != '\0') {
        if (*cursor < 0x20 || *cursor == 0x7f)
            putchar(' ');
        else
            putchar(*cursor);
        ++cursor;
    }
}

static int
list_visitor(controller *ctl, xcb_window_t window, unsigned int depth,
             void *opaque)
{
    window_geometry geometry;
    char *name;

    (void) depth;
    (void) opaque;
    name = window_name(ctl, window);
    if (name == NULL)
        return 0;
    if (absolute_geometry(ctl, window, &geometry) == 0) {
        printf("0x%08" PRIx32 "\t%d\t%d\t%u\t%u\t%s\t",
               window, geometry.x, geometry.y, geometry.width,
               geometry.height,
               window_is_mapped(ctl, window) ? "mapped" : "unmapped");
        print_name(name);
        putchar('\n');
    }
    free(name);
    return 0;
}

static int
check_request(controller *ctl, xcb_void_cookie_t cookie, const char *action)
{
    xcb_generic_error_t *error =
        xcb_request_check(ctl->session.connection, cookie);

    if (error == NULL)
        return 0;
    fprintf(stderr,
            "xminctl: %s failed (X11 error %u, request %u, minor %u)\n",
            action, error->error_code, error->major_code, error->minor_code);
    free(error);
    return -1;
}

static int
sync_connection(controller *ctl)
{
    xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(
        ctl->session.connection,
        xcb_get_input_focus(ctl->session.connection), NULL);

    if (reply == NULL) {
        fprintf(stderr, "xminctl: X11 connection was lost\n");
        return -1;
    }
    free(reply);
    return 0;
}

static int
require_xtest(controller *ctl)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(ctl->session.connection, &xcb_test_id);

    if (extension == NULL || !extension->present) {
        fprintf(stderr, "xminctl: the display does not provide XTEST\n");
        return -1;
    }
    return 0;
}

static int
fake_input(controller *ctl, uint8_t type, uint8_t detail,
           int16_t root_x, int16_t root_y)
{
    return check_request(
        ctl,
        xcb_test_fake_input_checked(ctl->session.connection, type, detail,
                                    XCB_CURRENT_TIME,
                                    ctl->session.screen->root,
                                    root_x, root_y, 0),
        "XTEST input injection");
}

static int
move_pointer(controller *ctl, xcb_window_t relative_to, int16_t x, int16_t y)
{
    xcb_translate_coordinates_reply_t *reply;
    int result;

    if (relative_to == ctl->session.screen->root)
        return fake_input(ctl, XCB_MOTION_NOTIFY, 0, x, y);
    reply = xcb_translate_coordinates_reply(
        ctl->session.connection,
        xcb_translate_coordinates(ctl->session.connection, relative_to,
                                  ctl->session.screen->root, x, y), NULL);
    if (reply == NULL) {
        fprintf(stderr, "xminctl: cannot translate window coordinates\n");
        return -1;
    }
    result = fake_input(ctl, XCB_MOTION_NOTIFY, 0,
                        reply->dst_x, reply->dst_y);
    free(reply);
    return result;
}

static int
load_keyboard_map(controller *ctl, keyboard_map *map)
{
    const xcb_setup_t *setup = xcb_get_setup(ctl->session.connection);
    uint16_t count = (uint16_t) setup->max_keycode - setup->min_keycode + 1;

    memset(map, 0, sizeof(*map));
    if (count > UINT8_MAX) {
        fprintf(stderr, "xminctl: invalid keyboard keycode range\n");
        return -1;
    }
    map->reply = xcb_get_keyboard_mapping_reply(
        ctl->session.connection,
        xcb_get_keyboard_mapping(ctl->session.connection,
                                 setup->min_keycode, (uint8_t) count),
        NULL);
    if (map->reply == NULL) {
        fprintf(stderr, "xminctl: cannot read the keyboard map\n");
        return -1;
    }
    map->symbols = xcb_get_keyboard_mapping_keysyms(map->reply);
    map->minimum = setup->min_keycode;
    map->count = (uint8_t) count;
    map->symbols_per_keycode = map->reply->keysyms_per_keycode;
    return 0;
}

static int
find_keycode(const keyboard_map *map, xcb_keysym_t symbol,
             xcb_keycode_t *keycode, bool *shift)
{
    unsigned int code;
    unsigned int level;
    unsigned int maximum_level = map->symbols_per_keycode < 2 ?
                                 map->symbols_per_keycode : 2;

    for (level = 0; level < maximum_level; ++level) {
        for (code = 0; code < map->count; ++code) {
            size_t offset = (size_t) code * map->symbols_per_keycode + level;

            if (map->symbols[offset] == symbol) {
                *keycode = (xcb_keycode_t) (map->minimum + code);
                *shift = level == 1;
                return 0;
            }
        }
    }
    return -1;
}

static xcb_keysym_t
named_keysym(const char *name)
{
    static const struct {
        const char *name;
        xcb_keysym_t symbol;
    } names[] = {
        { "backspace", XK_BackSpace }, { "tab", XK_Tab },
        { "return", XK_Return }, { "enter", XK_Return },
        { "escape", XK_Escape }, { "esc", XK_Escape },
        { "space", XK_space }, { "delete", XK_Delete },
        { "insert", XK_Insert }, { "home", XK_Home },
        { "end", XK_End }, { "left", XK_Left }, { "right", XK_Right },
        { "up", XK_Up }, { "down", XK_Down },
        { "pageup", XK_Page_Up }, { "pagedown", XK_Page_Down },
        { "f1", XK_F1 }, { "f2", XK_F2 }, { "f3", XK_F3 },
        { "f4", XK_F4 }, { "f5", XK_F5 }, { "f6", XK_F6 },
        { "f7", XK_F7 }, { "f8", XK_F8 }, { "f9", XK_F9 },
        { "f10", XK_F10 }, { "f11", XK_F11 }, { "f12", XK_F12 },
        { "shift", XK_Shift_L }, { "ctrl", XK_Control_L },
        { "control", XK_Control_L }, { "alt", XK_Alt_L },
        { "super", XK_Super_L }, { "meta", XK_Super_L },
    };
    size_t index;

    if (name[0] != '\0' && name[1] == '\0')
        return (unsigned char) name[0];
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strcasecmp(name, names[index].name) == 0)
            return names[index].symbol;
    }
    return XCB_NO_SYMBOL;
}

static int
send_keycode(controller *ctl, xcb_keycode_t keycode, bool press)
{
    return fake_input(ctl, press ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                      keycode, 0, 0);
}

static int
send_named_key_state(controller *ctl, const char *name, bool press)
{
    keyboard_map map;
    xcb_keysym_t symbol = named_keysym(name);
    xcb_keycode_t keycode;
    bool shift;
    int result = -1;

    if (symbol == XCB_NO_SYMBOL) {
        fprintf(stderr, "xminctl: unknown key name: %s\n", name);
        return -1;
    }
    if (load_keyboard_map(ctl, &map) != 0)
        return -1;
    if (find_keycode(&map, symbol, &keycode, &shift) != 0) {
        fprintf(stderr, "xminctl: keyboard map has no key for %s\n", name);
        goto cleanup;
    }
    if (shift) {
        fprintf(stderr,
                "xminctl: key-down/key-up requires an unshifted key name\n");
        goto cleanup;
    }
    if (send_keycode(ctl, keycode, press) == 0)
        result = sync_connection(ctl);

cleanup:
    free(map.reply);
    return result;
}

static int
type_text(controller *ctl, const char *text, long delay_milliseconds)
{
    keyboard_map map;
    xcb_keycode_t shift_key;
    bool ignored_shift;
    const unsigned char *cursor;
    int result = -1;

    if (load_keyboard_map(ctl, &map) != 0)
        return -1;
    if (find_keycode(&map, XK_Shift_L, &shift_key, &ignored_shift) != 0) {
        fprintf(stderr, "xminctl: keyboard map has no left Shift key\n");
        goto cleanup;
    }
    for (cursor = (const unsigned char *) text; *cursor != '\0'; ++cursor) {
        xcb_keysym_t symbol = *cursor;
        xcb_keycode_t keycode;
        bool shift;

        if (*cursor >= 0x80) {
            fprintf(stderr,
                    "xminctl: type currently accepts ASCII text only\n");
            goto cleanup;
        }
        if (*cursor == '\n' || *cursor == '\r')
            symbol = XK_Return;
        else if (*cursor == '\t')
            symbol = XK_Tab;
        else if (*cursor == '\b')
            symbol = XK_BackSpace;
        else if (*cursor < 0x20 || *cursor == 0x7f) {
            fprintf(stderr, "xminctl: unsupported control character 0x%02x\n",
                    *cursor);
            goto cleanup;
        }
        if (find_keycode(&map, symbol, &keycode, &shift) != 0) {
            fprintf(stderr, "xminctl: no keycode for character 0x%02x\n",
                    *cursor);
            goto cleanup;
        }
        if ((shift && send_keycode(ctl, shift_key, true) != 0) ||
            send_keycode(ctl, keycode, true) != 0 ||
            send_keycode(ctl, keycode, false) != 0 ||
            (shift && send_keycode(ctl, shift_key, false) != 0))
            goto cleanup;
        if (delay_milliseconds != 0 && cursor[1] != '\0')
            sleep_milliseconds(delay_milliseconds);
    }
    result = sync_connection(ctl);

cleanup:
    free(map.reply);
    return result;
}

static int
send_chord(controller *ctl, const char *chord)
{
    enum { MOD_SHIFT = 1, MOD_CONTROL = 2, MOD_ALT = 4, MOD_SUPER = 8 };
    keyboard_map map;
    char *copy;
    char *save = NULL;
    char *token;
    char *key_name = NULL;
    unsigned int modifiers = 0;
    xcb_keycode_t modifier_keys[4];
    size_t modifier_count = 0;
    xcb_keycode_t keycode;
    xcb_keysym_t symbol;
    bool automatic_shift;
    int result = -1;
    size_t index;

    copy = strdup(chord);
    if (copy == NULL)
        return -1;
    for (token = strtok_r(copy, "+", &save); token != NULL;
         token = strtok_r(NULL, "+", &save)) {
        if (strcasecmp(token, "shift") == 0)
            modifiers |= MOD_SHIFT;
        else if (strcasecmp(token, "ctrl") == 0 ||
                 strcasecmp(token, "control") == 0)
            modifiers |= MOD_CONTROL;
        else if (strcasecmp(token, "alt") == 0)
            modifiers |= MOD_ALT;
        else if (strcasecmp(token, "super") == 0 ||
                 strcasecmp(token, "meta") == 0)
            modifiers |= MOD_SUPER;
        else if (key_name == NULL)
            key_name = token;
        else {
            fprintf(stderr, "xminctl: chord has more than one non-modifier\n");
            goto cleanup_copy;
        }
    }
    if (key_name == NULL) {
        fprintf(stderr, "xminctl: chord has no key\n");
        goto cleanup_copy;
    }
    symbol = named_keysym(key_name);
    if (symbol == XCB_NO_SYMBOL) {
        fprintf(stderr, "xminctl: unknown key name: %s\n", key_name);
        goto cleanup_copy;
    }
    if (load_keyboard_map(ctl, &map) != 0)
        goto cleanup_copy;
    if (find_keycode(&map, symbol, &keycode, &automatic_shift) != 0) {
        fprintf(stderr, "xminctl: keyboard map has no key for %s\n", key_name);
        goto cleanup_map;
    }
    if (automatic_shift)
        modifiers |= MOD_SHIFT;

#define ADD_MODIFIER(flag, keysym) do {                                      \
        if ((modifiers & (flag)) != 0) {                                     \
            bool modifier_shift;                                             \
            if (find_keycode(&map, (keysym),                                 \
                             &modifier_keys[modifier_count],                  \
                             &modifier_shift) != 0) {                         \
                fprintf(stderr, "xminctl: keyboard map lacks a modifier\n"); \
                goto cleanup_map;                                            \
            }                                                                \
            ++modifier_count;                                                \
        }                                                                    \
    } while (0)
    ADD_MODIFIER(MOD_SHIFT, XK_Shift_L);
    ADD_MODIFIER(MOD_CONTROL, XK_Control_L);
    ADD_MODIFIER(MOD_ALT, XK_Alt_L);
    ADD_MODIFIER(MOD_SUPER, XK_Super_L);
#undef ADD_MODIFIER

    for (index = 0; index < modifier_count; ++index) {
        if (send_keycode(ctl, modifier_keys[index], true) != 0)
            goto cleanup_map;
    }
    if (send_keycode(ctl, keycode, true) != 0 ||
        send_keycode(ctl, keycode, false) != 0)
        goto cleanup_map;
    for (index = modifier_count; index != 0; --index) {
        if (send_keycode(ctl, modifier_keys[index - 1], false) != 0)
            goto cleanup_map;
    }
    result = sync_connection(ctl);

cleanup_map:
    free(map.reply);
cleanup_copy:
    free(copy);
    return result;
}

static const xcb_visualtype_t *
find_visual(controller *ctl, xcb_visualid_t visual_id)
{
    xcb_depth_iterator_t depths =
        xcb_screen_allowed_depths_iterator(ctl->session.screen);

    for (; depths.rem != 0; xcb_depth_next(&depths)) {
        xcb_visualtype_iterator_t visuals =
            xcb_depth_visuals_iterator(depths.data);

        for (; visuals.rem != 0; xcb_visualtype_next(&visuals)) {
            if (visuals.data->visual_id == visual_id)
                return visuals.data;
        }
    }
    return NULL;
}

static const xcb_format_t *
find_format(controller *ctl, uint8_t depth)
{
    xcb_format_iterator_t formats =
        xcb_setup_pixmap_formats_iterator(
            xcb_get_setup(ctl->session.connection));

    for (; formats.rem != 0; xcb_format_next(&formats)) {
        if (formats.data->depth == depth)
            return formats.data;
    }
    return NULL;
}

static unsigned int
mask_shift(uint32_t mask)
{
    unsigned int shift = 0;

    while (mask != 0 && (mask & 1U) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

static uint8_t
pixel_channel(uint32_t pixel, uint32_t mask)
{
    unsigned int shift;
    uint32_t maximum;
    uint32_t value;

    if (mask == 0)
        return 0;
    shift = mask_shift(mask);
    maximum = mask >> shift;
    value = (pixel & mask) >> shift;
    return (uint8_t) ((value * 255U + maximum / 2U) / maximum);
}

static uint32_t
read_pixel(const uint8_t *bytes, unsigned int byte_count, uint8_t byte_order)
{
    uint32_t pixel = 0;
    unsigned int index;

    if (byte_order == XCB_IMAGE_ORDER_LSB_FIRST) {
        for (index = 0; index < byte_count; ++index)
            pixel |= (uint32_t) bytes[index] << (index * 8U);
    }
    else {
        for (index = 0; index < byte_count; ++index)
            pixel = (pixel << 8U) | bytes[index];
    }
    return pixel;
}

static int
write_ppm(controller *ctl, const char *path, uint16_t width, uint16_t height,
          uint8_t depth, xcb_visualid_t visual_id, const uint8_t *data,
          size_t data_size)
{
    const xcb_setup_t *setup = xcb_get_setup(ctl->session.connection);
    const xcb_visualtype_t *visual = find_visual(ctl, visual_id);
    const xcb_format_t *format = find_format(ctl, depth);
    size_t stride;
    unsigned int bytes_per_pixel;
    FILE *file;
    uint16_t y;
    int result = -1;

    if (visual == NULL || format == NULL || visual->red_mask == 0 ||
        visual->green_mask == 0 || visual->blue_mask == 0) {
        fprintf(stderr,
                "xminctl: capture requires a TrueColor/DirectColor visual\n");
        return -1;
    }
    bytes_per_pixel = (format->bits_per_pixel + 7U) / 8U;
    if (bytes_per_pixel == 0 || bytes_per_pixel > 4) {
        fprintf(stderr, "xminctl: unsupported %u-bit pixel format\n",
                format->bits_per_pixel);
        return -1;
    }
    stride = ((size_t) width * format->bits_per_pixel +
              format->scanline_pad - 1) / format->scanline_pad;
    stride *= format->scanline_pad / 8U;
    if (stride * height > data_size) {
        fprintf(stderr, "xminctl: truncated X11 image reply\n");
        return -1;
    }
    file = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "xminctl: cannot write %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0)
        goto cleanup;
    for (y = 0; y < height; ++y) {
        const uint8_t *row = data + (size_t) y * stride;
        uint16_t x;

        for (x = 0; x < width; ++x) {
            uint32_t pixel = read_pixel(row + (size_t) x * bytes_per_pixel,
                                        bytes_per_pixel,
                                        setup->image_byte_order);
            uint8_t rgb[3] = {
                pixel_channel(pixel, visual->red_mask),
                pixel_channel(pixel, visual->green_mask),
                pixel_channel(pixel, visual->blue_mask),
            };

            if (fwrite(rgb, sizeof(rgb), 1, file) != 1)
                goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (file != stdout && fclose(file) != 0)
        result = -1;
    else if (file == stdout && fflush(file) != 0)
        result = -1;
    if (result != 0)
        fprintf(stderr, "xminctl: failed while writing %s\n", path);
    return result;
}

static int
capture_window(controller *ctl, xcb_window_t window, const char *path)
{
    xcb_get_geometry_reply_t *geometry;
    xcb_get_window_attributes_reply_t *attributes;
    xcb_get_image_reply_t *image = NULL;
    xcb_drawable_t drawable = window;
    xcb_pixmap_t pixmap = XCB_PIXMAP_NONE;
    bool redirected = false;
    const xcb_query_extension_reply_t *composite;
    int result = -1;

    geometry = xcb_get_geometry_reply(
        ctl->session.connection,
        xcb_get_geometry(ctl->session.connection, window), NULL);
    attributes = xcb_get_window_attributes_reply(
        ctl->session.connection,
        xcb_get_window_attributes(ctl->session.connection, window), NULL);
    if (geometry == NULL || attributes == NULL) {
        fprintf(stderr, "xminctl: cannot inspect capture window\n");
        goto cleanup;
    }

    composite = xcb_get_extension_data(ctl->session.connection,
                                       &xcb_composite_id);
    if (window != ctl->session.screen->root && composite != NULL &&
        composite->present) {
        xcb_composite_query_version_reply_t *version =
            xcb_composite_query_version_reply(
                ctl->session.connection,
                xcb_composite_query_version(ctl->session.connection, 0, 4),
                NULL);

        if (version != NULL) {
            free(version);
            if (check_request(
                    ctl,
                    xcb_composite_redirect_window_checked(
                        ctl->session.connection, window,
                        XCB_COMPOSITE_REDIRECT_AUTOMATIC),
                    "Composite window redirect") == 0) {
                redirected = true;
                pixmap = xcb_generate_id(ctl->session.connection);
                if (check_request(
                        ctl,
                        xcb_composite_name_window_pixmap_checked(
                            ctl->session.connection, window, pixmap),
                        "Composite window snapshot") == 0)
                    drawable = pixmap;
                else {
                    pixmap = XCB_PIXMAP_NONE;
                    drawable = window;
                }
            }
        }
    }

    image = xcb_get_image_reply(
        ctl->session.connection,
        xcb_get_image(ctl->session.connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
                      drawable, 0, 0, geometry->width, geometry->height,
                      UINT32_MAX),
        NULL);
    if (image == NULL) {
        fprintf(stderr, "xminctl: X11 GetImage failed\n");
        goto cleanup;
    }
    result = write_ppm(ctl, path, geometry->width, geometry->height,
                       geometry->depth, attributes->visual,
                       xcb_get_image_data(image),
                       (size_t) xcb_get_image_data_length(image));

cleanup:
    free(image);
    if (pixmap != XCB_PIXMAP_NONE)
        xcb_free_pixmap(ctl->session.connection, pixmap);
    if (redirected)
        xcb_composite_unredirect_window(
            ctl->session.connection, window,
            XCB_COMPOSITE_REDIRECT_AUTOMATIC);
    xcb_flush(ctl->session.connection);
    free(attributes);
    free(geometry);
    return result;
}

static int
wait_stable(controller *ctl, xcb_window_t window,
            long quiet_milliseconds, long timeout_milliseconds)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(ctl->session.connection, &xcb_damage_id);
    xcb_damage_damage_t damage;
    int64_t start;
    int64_t last_damage;
    int result = -1;
    xcb_generic_error_t *version_error = NULL;
    xcb_damage_query_version_reply_t *version;

    if (extension == NULL || !extension->present) {
        fprintf(stderr, "xminctl: the display does not provide DAMAGE\n");
        return -1;
    }
    version = xcb_damage_query_version_reply(
        ctl->session.connection,
        xcb_damage_query_version(ctl->session.connection, 1, 1),
        &version_error);
    if (version == NULL || version_error != NULL) {
        fprintf(stderr, "xminctl: cannot negotiate the DAMAGE extension\n");
        free(version_error);
        free(version);
        return -1;
    }
    free(version);
    damage = xcb_generate_id(ctl->session.connection);
    if (check_request(
            ctl,
            xcb_damage_create_checked(
                ctl->session.connection, damage, window,
                XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY),
            "DAMAGE subscription") != 0)
        return -1;
    xcb_damage_subtract(ctl->session.connection, damage,
                        XCB_XFIXES_REGION_NONE, XCB_XFIXES_REGION_NONE);
    xcb_flush(ctl->session.connection);
    start = monotonic_milliseconds();
    last_damage = start;
    if (start < 0)
        goto cleanup;

    for (;;) {
        xcb_generic_event_t *event;
        int64_t now;
        long wait_for;
        struct pollfd descriptor;
        int poll_result;

        while ((event = xcb_poll_for_event(ctl->session.connection)) != NULL) {
            uint8_t type = event->response_type & 0x7f;

            if (type == extension->first_event + XCB_DAMAGE_NOTIFY) {
                xcb_damage_notify_event_t *notify =
                    (xcb_damage_notify_event_t *) event;

                if (notify->damage == damage) {
                    last_damage = monotonic_milliseconds();
                    xcb_damage_subtract(ctl->session.connection, damage,
                                        XCB_XFIXES_REGION_NONE,
                                        XCB_XFIXES_REGION_NONE);
                    xcb_flush(ctl->session.connection);
                }
            }
            free(event);
        }
        if (xcb_connection_has_error(ctl->session.connection) != 0) {
            fprintf(stderr, "xminctl: X11 connection lost while waiting\n");
            goto cleanup;
        }
        now = monotonic_milliseconds();
        if (now < 0)
            goto cleanup;
        if (now - last_damage >= quiet_milliseconds) {
            result = 0;
            break;
        }
        if (now - start >= timeout_milliseconds) {
            fprintf(stderr,
                    "xminctl: window did not become stable within %ld ms\n",
                    timeout_milliseconds);
            break;
        }
        wait_for = quiet_milliseconds - (long) (now - last_damage);
        if (timeout_milliseconds - (long) (now - start) < wait_for)
            wait_for = timeout_milliseconds - (long) (now - start);
        descriptor.fd = xcb_get_file_descriptor(ctl->session.connection);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            poll_result = poll(&descriptor, 1, (int) wait_for);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            fprintf(stderr, "xminctl: poll failed: %s\n", strerror(errno));
            break;
        }
    }

cleanup:
    xcb_damage_destroy(ctl->session.connection, damage);
    xcb_flush(ctl->session.connection);
    return result;
}

static int
wait_for_window(controller *ctl, const char *name, long timeout_milliseconds)
{
    int64_t start = monotonic_milliseconds();

    if (start < 0)
        return -1;
    for (;;) {
        xcb_window_t window = resolve_window(ctl, name, true);
        int64_t now;
        struct timespec delay = { 0, 50 * 1000 * 1000 };

        if (window != XCB_WINDOW_NONE) {
            printf("0x%08" PRIx32 "\n", window);
            return 0;
        }
        now = monotonic_milliseconds();
        if (now < 0 || now - start >= timeout_milliseconds) {
            fprintf(stderr,
                    "xminctl: no mapped window matching '%s' within %ld ms\n",
                    name, timeout_milliseconds);
            return -1;
        }
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            ;
    }
}

static int
configure_window(controller *ctl, xcb_window_t window, uint16_t mask,
                 const uint32_t *values, const char *action)
{
    if (check_request(
            ctl,
            xcb_configure_window_checked(ctl->session.connection, window,
                                         mask, values),
            action) != 0)
        return -1;
    return sync_connection(ctl);
}

static int
close_window(controller *ctl, xcb_window_t window)
{
    xcb_client_message_event_t event;

    memset(&event, 0, sizeof(event));
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = ctl->wm_protocols;
    event.data.data32[0] = ctl->wm_delete_window;
    event.data.data32[1] = XCB_CURRENT_TIME;
    if (check_request(
            ctl,
            xcb_send_event_checked(ctl->session.connection, false, window,
                                   XCB_EVENT_MASK_NO_EVENT,
                                   (const char *) &event),
            "WM_DELETE_WINDOW") != 0)
        return -1;
    return sync_connection(ctl);
}

static int
dispatch(controller *ctl, int argc, char **argv)
{
    const char *command = argv[0];
    xcb_window_t window;
    long first;
    long second;

    if (strcmp(command, "list-windows") == 0) {
        if (argc != 1)
            return 2;
        printf("window\tx\ty\twidth\theight\tstate\tname\n");
        (void) walk_windows(ctl, ctl->session.screen->root, 0,
                            list_visitor, NULL);
        return 0;
    }
    if (strcmp(command, "wait-window") == 0) {
        long timeout = 5000;
        const char *name = NULL;
        int index;

        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &timeout) != 0)
                    return 2;
            }
            else if (name == NULL)
                name = argv[index];
            else
                return 2;
        }
        return name == NULL ? 2 : wait_for_window(ctl, name, timeout);
    }
    if (strcmp(command, "find-window") == 0) {
        if (argc != 2)
            return 2;
        window = resolve_window(ctl, argv[1], false);
        if (window == XCB_WINDOW_NONE) {
            fprintf(stderr, "xminctl: no window matching '%s'\n", argv[1]);
            return 1;
        }
        printf("0x%08" PRIx32 "\n", window);
        return 0;
    }
    if (strcmp(command, "geometry") == 0) {
        window_geometry geometry;

        if (argc != 2)
            return 2;
        window = resolve_window(ctl, argv[1], false);
        if (window == XCB_WINDOW_NONE ||
            absolute_geometry(ctl, window, &geometry) != 0) {
            fprintf(stderr, "xminctl: cannot find window '%s'\n", argv[1]);
            return 1;
        }
        printf("%d %d %u %u %u %u\n", geometry.x, geometry.y,
               geometry.width, geometry.height, geometry.depth,
               geometry.border_width);
        return 0;
    }
    if (argc < 2 && strcmp(command, "capture-root") != 0 &&
        strcmp(command, "pointer") != 0 && strcmp(command, "scroll") != 0)
        return 2;

    if (strcmp(command, "focus") == 0 || strcmp(command, "raise") == 0 ||
        strcmp(command, "activate") == 0 || strcmp(command, "map") == 0 ||
        strcmp(command, "unmap") == 0 || strcmp(command, "close") == 0) {
        uint32_t above = XCB_STACK_MODE_ABOVE;

        if (argc != 2)
            return 2;
        window = resolve_window(ctl, argv[1], false);
        if (window == XCB_WINDOW_NONE) {
            fprintf(stderr, "xminctl: cannot find window '%s'\n", argv[1]);
            return 1;
        }
        if (strcmp(command, "focus") == 0 ||
            strcmp(command, "activate") == 0) {
            if (check_request(
                    ctl,
                    xcb_set_input_focus_checked(
                        ctl->session.connection, XCB_INPUT_FOCUS_PARENT,
                        window, XCB_CURRENT_TIME),
                    "focus") != 0)
                return 1;
        }
        if (strcmp(command, "raise") == 0 ||
            strcmp(command, "activate") == 0) {
            if (configure_window(ctl, window, XCB_CONFIG_WINDOW_STACK_MODE,
                                 &above, "raise") != 0)
                return 1;
        }
        else if (strcmp(command, "map") == 0) {
            if (check_request(ctl,
                              xcb_map_window_checked(ctl->session.connection,
                                                     window),
                              "map") != 0)
                return 1;
        }
        else if (strcmp(command, "unmap") == 0) {
            if (check_request(ctl,
                              xcb_unmap_window_checked(ctl->session.connection,
                                                       window),
                              "unmap") != 0)
                return 1;
        }
        else if (strcmp(command, "close") == 0)
            return close_window(ctl, window) == 0 ? 0 : 1;
        return sync_connection(ctl) == 0 ? 0 : 1;
    }
    if (strcmp(command, "move") == 0 || strcmp(command, "resize") == 0) {
        uint32_t values[2];
        uint16_t mask;

        if (argc != 4 || parse_long(argv[2], INT16_MIN, UINT16_MAX, &first) != 0 ||
            parse_long(argv[3], INT16_MIN, UINT16_MAX, &second) != 0)
            return 2;
        window = resolve_window(ctl, argv[1], false);
        if (window == XCB_WINDOW_NONE)
            return 1;
        values[0] = (uint32_t) first;
        values[1] = (uint32_t) second;
        if (strcmp(command, "move") == 0)
            mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        else {
            if (first < 1 || second < 1)
                return 2;
            mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        }
        return configure_window(ctl, window, mask, values, command) == 0 ? 0 : 1;
    }
    if (strcmp(command, "mouse-move") == 0) {
        if (argc != 4 ||
            parse_long(argv[2], INT16_MIN, INT16_MAX, &first) != 0 ||
            parse_long(argv[3], INT16_MIN, INT16_MAX, &second) != 0)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        window = resolve_window(ctl, argv[1], true);
        if (window == XCB_WINDOW_NONE)
            return 1;
        if (move_pointer(ctl, window, (int16_t) first, (int16_t) second) != 0)
            return 1;
        return sync_connection(ctl) == 0 ? 0 : 1;
    }
    if (strcmp(command, "click") == 0) {
        const char *positionals[4];
        int positional_count = 0;
        long button = 1;
        long delay = 0;
        int index;

        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--delay") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &delay) != 0)
                    return 2;
            }
            else if (positional_count < 4)
                positionals[positional_count++] = argv[index];
            else
                return 2;
        }
        if ((positional_count != 3 && positional_count != 4) ||
            parse_long(positionals[1], INT16_MIN, INT16_MAX, &first) != 0 ||
            parse_long(positionals[2], INT16_MIN, INT16_MAX, &second) != 0 ||
            (positional_count == 4 &&
             parse_long(positionals[3], 1, UINT8_MAX, &button) != 0))
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        window = resolve_window(ctl, positionals[0], true);
        if (window == XCB_WINDOW_NONE)
            return 1;
        if (move_pointer(ctl, window, (int16_t) first, (int16_t) second) != 0 ||
            fake_input(ctl, XCB_BUTTON_PRESS, (uint8_t) button, 0, 0) != 0)
            return 1;
        if (delay != 0)
            sleep_milliseconds(delay);
        if (fake_input(ctl, XCB_BUTTON_RELEASE, (uint8_t) button, 0, 0) != 0)
            return 1;
        return sync_connection(ctl) == 0 ? 0 : 1;
    }
    if (strcmp(command, "mouse-drag") == 0) {
        const char *positionals[5];
        int positional_count = 0;
        long button = 1;
        long steps = 10;
        long delay = 10;
        long start_x;
        long start_y;
        long end_x;
        long end_y;
        long step;
        int index;
        int result = 1;

        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--button") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 1, UINT8_MAX, &button) != 0)
                    return 2;
            }
            else if (strcmp(argv[index], "--steps") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 1, 10000, &steps) != 0)
                    return 2;
            }
            else if (strcmp(argv[index], "--delay") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &delay) != 0)
                    return 2;
            }
            else if (positional_count < 5)
                positionals[positional_count++] = argv[index];
            else
                return 2;
        }
        if (positional_count != 5 ||
            parse_long(positionals[1], INT16_MIN, INT16_MAX, &start_x) != 0 ||
            parse_long(positionals[2], INT16_MIN, INT16_MAX, &start_y) != 0 ||
            parse_long(positionals[3], INT16_MIN, INT16_MAX, &end_x) != 0 ||
            parse_long(positionals[4], INT16_MIN, INT16_MAX, &end_y) != 0)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        window = resolve_window(ctl, positionals[0], true);
        if (window == XCB_WINDOW_NONE)
            return 1;
        if (move_pointer(ctl, window, (int16_t) start_x, (int16_t) start_y) != 0 ||
            fake_input(ctl, XCB_BUTTON_PRESS, (uint8_t) button, 0, 0) != 0)
            return 1;
        for (step = 1; step <= steps; ++step) {
            long x;
            long y;

            if (delay != 0)
                sleep_milliseconds(delay);
            x = start_x + (end_x - start_x) * step / steps;
            y = start_y + (end_y - start_y) * step / steps;
            if (move_pointer(ctl, window, (int16_t) x, (int16_t) y) != 0)
                goto release_drag;
        }
        result = 0;

release_drag:
        if (fake_input(ctl, XCB_BUTTON_RELEASE, (uint8_t) button, 0, 0) != 0)
            result = 1;
        if (sync_connection(ctl) != 0)
            result = 1;
        return result;
    }
    if (strcmp(command, "button") == 0) {
        if (argc != 3 || parse_long(argv[1], 1, UINT8_MAX, &first) != 0 ||
            (strcmp(argv[2], "down") != 0 && strcmp(argv[2], "up") != 0))
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        if (fake_input(ctl, strcmp(argv[2], "down") == 0 ?
                       XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                       (uint8_t) first, 0, 0) != 0)
            return 1;
        return sync_connection(ctl) == 0 ? 0 : 1;
    }
    if (strcmp(command, "scroll") == 0) {
        long count;
        long index;
        uint8_t button;

        if (argc != 2 || parse_long(argv[1], INT16_MIN, INT16_MAX, &count) != 0)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        button = count >= 0 ? 4 : 5;
        if (count < 0)
            count = -count;
        for (index = 0; index < count; ++index) {
            if (fake_input(ctl, XCB_BUTTON_PRESS, button, 0, 0) != 0 ||
                fake_input(ctl, XCB_BUTTON_RELEASE, button, 0, 0) != 0)
                return 1;
        }
        return sync_connection(ctl) == 0 ? 0 : 1;
    }
    if (strcmp(command, "pointer") == 0) {
        xcb_query_pointer_reply_t *reply;

        if (argc != 1)
            return 2;
        reply = xcb_query_pointer_reply(
            ctl->session.connection,
            xcb_query_pointer(ctl->session.connection,
                              ctl->session.screen->root), NULL);
        if (reply == NULL)
            return 1;
        printf("%d %d 0x%08" PRIx32 "\n",
               reply->root_x, reply->root_y, reply->child);
        free(reply);
        return 0;
    }
    if (strcmp(command, "key") == 0) {
        if (argc != 2)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        return send_chord(ctl, argv[1]) == 0 ? 0 : 1;
    }
    if (strcmp(command, "type") == 0) {
        long delay = 0;
        const char *text = NULL;
        int index;

        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--delay") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &delay) != 0)
                    return 2;
            }
            else if (text == NULL)
                text = argv[index];
            else
                return 2;
        }
        if (text == NULL)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        return type_text(ctl, text, delay) == 0 ? 0 : 1;
    }
    if (strcmp(command, "key-down") == 0 ||
        strcmp(command, "keydown") == 0 ||
        strcmp(command, "key-up") == 0 ||
        strcmp(command, "keyup") == 0) {
        bool press = strcmp(command, "key-down") == 0 ||
                     strcmp(command, "keydown") == 0;

        if (argc != 2)
            return 2;
        if (require_xtest(ctl) != 0)
            return 1;
        return send_named_key_state(ctl, argv[1], press) == 0 ? 0 : 1;
    }
    if (strcmp(command, "wait-stable") == 0) {
        long quiet = 150;
        long timeout = 5000;
        const char *selector = NULL;
        int index;

        for (index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--quiet") == 0 && index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &quiet) != 0)
                    return 2;
            }
            else if (strcmp(argv[index], "--timeout") == 0 &&
                     index + 1 < argc) {
                if (parse_long(argv[++index], 0, INT32_MAX, &timeout) != 0)
                    return 2;
            }
            else if (selector == NULL)
                selector = argv[index];
            else
                return 2;
        }
        if (selector == NULL)
            return 2;
        window = resolve_window(ctl, selector, true);
        if (window == XCB_WINDOW_NONE)
            return 1;
        return wait_stable(ctl, window, quiet, timeout) == 0 ? 0 : 1;
    }
    if (strcmp(command, "capture-root") == 0) {
        if (argc != 2)
            return 2;
        return capture_window(ctl, ctl->session.screen->root, argv[1]) == 0 ? 0 : 1;
    }
    if (strcmp(command, "capture-window") == 0) {
        if (argc != 3)
            return 2;
        window = resolve_window(ctl, argv[1], true);
        if (window == XCB_WINDOW_NONE) {
            fprintf(stderr, "xminctl: cannot find mapped window '%s'\n", argv[1]);
            return 1;
        }
        return capture_window(ctl, window, argv[2]) == 0 ? 0 : 1;
    }

    fprintf(stderr, "xminctl: unknown command: %s\n", command);
    return 2;
}

int
main(int argc, char **argv)
{
    const char *display = NULL;
    controller ctl;
    char error[256];
    int index = 1;
    int result;

    while (index < argc) {
        if (strcmp(argv[index], "--display") == 0 && index + 1 < argc) {
            display = argv[index + 1];
            index += 2;
        }
        else if (strcmp(argv[index], "--help") == 0 ||
                 strcmp(argv[index], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        else if (strcmp(argv[index], "--version") == 0) {
            printf("xminctl %s\n", XMIN_VERSION);
            return 0;
        }
        else
            break;
    }
    if (index == argc) {
        usage(stderr);
        return 2;
    }
    memset(&ctl, 0, sizeof(ctl));
    if (xmin_xcb_connect(&ctl.session, display, error, sizeof(error)) != 0) {
        fprintf(stderr, "xminctl: %s\n", error);
        return 1;
    }
    if (initialize_atoms(&ctl) != 0) {
        xmin_xcb_disconnect(&ctl.session);
        return 1;
    }
    result = dispatch(&ctl, argc - index, argv + index);
    if (result == 2)
        usage(stderr);
    xmin_xcb_disconnect(&ctl.session);
    return result;
}
