#ifndef XMIN_CLIENT_H
#define XMIN_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t xcb_window_t;
typedef uint32_t xcb_drawable_t;
typedef uint32_t xcb_pixmap_t;
typedef uint32_t xcb_atom_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_keysym_t;
typedef uint32_t xcb_damage_damage_t;
typedef uint8_t xcb_keycode_t;

typedef struct xcb_connection_t xcb_connection_t;
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef xcb_cookie_t xcb_void_cookie_t;
typedef xcb_cookie_t xcb_intern_atom_cookie_t;
typedef xcb_cookie_t xcb_get_property_cookie_t;
typedef xcb_cookie_t xcb_get_window_attributes_cookie_t;
typedef xcb_cookie_t xcb_query_tree_cookie_t;
typedef xcb_cookie_t xcb_get_geometry_cookie_t;
typedef xcb_cookie_t xcb_translate_coordinates_cookie_t;
typedef xcb_cookie_t xcb_get_input_focus_cookie_t;
typedef xcb_cookie_t xcb_get_keyboard_mapping_cookie_t;
typedef xcb_cookie_t xcb_query_pointer_cookie_t;
typedef xcb_cookie_t xcb_get_image_cookie_t;
typedef xcb_cookie_t xcb_composite_query_version_cookie_t;
typedef xcb_cookie_t xcb_damage_query_version_cookie_t;

typedef struct {
    uint8_t depth;
    uint8_t bits_per_pixel;
    uint8_t scanline_pad;
    uint8_t pad0[5];
} xcb_format_t;

typedef struct {
    xcb_visualid_t visual_id;
    uint8_t class_;
    uint8_t bits_per_rgb_value;
    uint16_t colormap_entries;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint8_t pad0[4];
} xcb_visualtype_t;

typedef struct {
    uint8_t depth;
    uint8_t pad0;
    uint16_t visuals_len;
    uint8_t pad1[4];
    xcb_visualtype_t xmin_visual;
} xcb_depth_t;

typedef struct {
    xcb_window_t root;
    uint32_t default_colormap;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t current_input_masks;
    uint16_t width_in_pixels;
    uint16_t height_in_pixels;
    uint16_t width_in_millimeters;
    uint16_t height_in_millimeters;
    uint16_t min_installed_maps;
    uint16_t max_installed_maps;
    xcb_visualid_t root_visual;
    uint8_t backing_stores;
    uint8_t save_unders;
    uint8_t root_depth;
    uint8_t allowed_depths_len;
    xcb_depth_t xmin_depth;
} xcb_screen_t;

typedef struct {
    uint8_t status;
    uint8_t pad0;
    uint16_t protocol_major_version;
    uint16_t protocol_minor_version;
    uint16_t length;
    uint32_t release_number;
    uint32_t resource_id_base;
    uint32_t resource_id_mask;
    uint32_t motion_buffer_size;
    uint16_t vendor_len;
    uint16_t maximum_request_length;
    uint8_t roots_len;
    uint8_t pixmap_formats_len;
    uint8_t image_byte_order;
    uint8_t bitmap_format_bit_order;
    uint8_t bitmap_format_scanline_unit;
    uint8_t bitmap_format_scanline_pad;
    xcb_keycode_t min_keycode;
    xcb_keycode_t max_keycode;
    xcb_format_t xmin_formats[8];
} xcb_setup_t;

typedef struct { xcb_depth_t *data; int rem; int index; } xcb_depth_iterator_t;
typedef struct { xcb_visualtype_t *data; int rem; int index; }
    xcb_visualtype_iterator_t;
typedef struct { xcb_format_t *data; int rem; int index; } xcb_format_iterator_t;

typedef struct {
    uint8_t response_type;
    uint8_t error_code;
    uint16_t sequence;
    uint32_t resource_id;
    uint16_t minor_code;
    uint8_t major_code;
    uint8_t pad0;
    uint32_t pad[5];
} xcb_generic_error_t;

typedef struct { uint8_t response_type; uint8_t pad[31]; }
    xcb_generic_event_t;

typedef struct {
    uint8_t response_type;
    uint8_t format;
    uint16_t sequence;
    xcb_window_t window;
    xcb_atom_t type;
    union {
        uint8_t data8[20];
        uint16_t data16[10];
        uint32_t data32[5];
    } data;
} xcb_client_message_event_t;

typedef struct {
    uint8_t response_type;
    uint8_t level;
    uint16_t sequence;
    xcb_drawable_t drawable;
    xcb_damage_damage_t damage;
    uint32_t timestamp;
    int16_t area_x;
    int16_t area_y;
    uint16_t area_width;
    uint16_t area_height;
    int16_t geometry_x;
    int16_t geometry_y;
    uint16_t geometry_width;
    uint16_t geometry_height;
} xcb_damage_notify_event_t;

typedef struct { uint8_t response_type, pad0; uint16_t sequence; uint32_t length;
                 xcb_atom_t atom; uint8_t pad1[20]; }
    xcb_intern_atom_reply_t;
typedef struct { uint8_t response_type, format; uint16_t sequence;
                 uint32_t length; xcb_atom_t type; uint32_t bytes_after;
                 uint32_t value_len; uint8_t pad[12]; }
    xcb_get_property_reply_t;
typedef struct { uint8_t response_type, backing_store; uint16_t sequence;
                 uint32_t length; xcb_visualid_t visual; uint16_t class_;
                 uint8_t bit_gravity, win_gravity; uint32_t backing_planes;
                 uint32_t backing_pixel; uint8_t save_under, map_is_installed;
                 uint8_t map_state, override_redirect; uint32_t colormap;
                 uint32_t all_event_masks; uint32_t your_event_mask;
                 uint16_t do_not_propagate_mask; uint8_t pad[2]; }
    xcb_get_window_attributes_reply_t;
typedef struct { uint8_t response_type, pad0; uint16_t sequence; uint32_t length;
                 xcb_window_t root, parent; uint16_t children_len;
                 uint8_t pad[14]; }
    xcb_query_tree_reply_t;
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length;
                 xcb_window_t root; int16_t x, y; uint16_t width, height;
                 uint16_t border_width; uint8_t pad[10]; }
    xcb_get_geometry_reply_t;
typedef struct { uint8_t response_type, same_screen; uint16_t sequence;
                 uint32_t length; xcb_window_t child; int16_t dst_x, dst_y;
                 uint8_t pad[16]; }
    xcb_translate_coordinates_reply_t;
typedef struct { uint8_t response_type, revert_to; uint16_t sequence;
                 uint32_t length; xcb_window_t focus; uint8_t pad[20]; }
    xcb_get_input_focus_reply_t;
typedef struct { uint8_t response_type, keysyms_per_keycode; uint16_t sequence;
                 uint32_t length; uint8_t pad[24]; }
    xcb_get_keyboard_mapping_reply_t;
typedef struct { uint8_t response_type, same_screen; uint16_t sequence;
                 uint32_t length; xcb_window_t root, child;
                 int16_t root_x, root_y, win_x, win_y; uint16_t mask;
                 uint8_t pad[6]; }
    xcb_query_pointer_reply_t;
typedef struct { uint8_t response_type, depth; uint16_t sequence;
                 uint32_t length; xcb_visualid_t visual; uint8_t pad[20]; }
    xcb_get_image_reply_t;
typedef struct { uint8_t response_type, pad0; uint16_t sequence; uint32_t length;
                 uint32_t major_version, minor_version; uint8_t pad[16]; }
    xcb_composite_query_version_reply_t;
typedef xcb_composite_query_version_reply_t xcb_damage_query_version_reply_t;

typedef struct { const char *name; int global_id; } xcb_extension_t;
typedef struct { uint8_t response_type, pad0; uint16_t sequence; uint32_t length;
                 uint8_t present, major_opcode, first_event, first_error;
                 uint8_t pad[20]; }
    xcb_query_extension_reply_t;

extern xcb_extension_t xcb_test_id;
extern xcb_extension_t xcb_composite_id;
extern xcb_extension_t xcb_damage_id;

typedef struct xmin_xcb_session {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    int display_number;
    int screen_number;
} xmin_xcb_session;

int xmin_xcb_connect(xmin_xcb_session *, const char *, char *, size_t);
void xmin_xcb_disconnect(xmin_xcb_session *);

const xcb_setup_t *xcb_get_setup(xcb_connection_t *);
xcb_depth_iterator_t xcb_screen_allowed_depths_iterator(xcb_screen_t *);
void xcb_depth_next(xcb_depth_iterator_t *);
xcb_visualtype_iterator_t xcb_depth_visuals_iterator(xcb_depth_t *);
void xcb_visualtype_next(xcb_visualtype_iterator_t *);
xcb_format_iterator_t xcb_setup_pixmap_formats_iterator(const xcb_setup_t *);
void xcb_format_next(xcb_format_iterator_t *);

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *, uint8_t,
                                         uint16_t, const char *);
xcb_intern_atom_reply_t *xcb_intern_atom_reply(
    xcb_connection_t *, xcb_intern_atom_cookie_t, xcb_generic_error_t **);
xcb_get_property_cookie_t xcb_get_property(
    xcb_connection_t *, uint8_t, xcb_window_t, xcb_atom_t, xcb_atom_t,
    uint32_t, uint32_t);
xcb_get_property_reply_t *xcb_get_property_reply(
    xcb_connection_t *, xcb_get_property_cookie_t, xcb_generic_error_t **);
int xcb_get_property_value_length(const xcb_get_property_reply_t *);
void *xcb_get_property_value(const xcb_get_property_reply_t *);
xcb_get_window_attributes_cookie_t xcb_get_window_attributes(
    xcb_connection_t *, xcb_window_t);
xcb_get_window_attributes_reply_t *xcb_get_window_attributes_reply(
    xcb_connection_t *, xcb_get_window_attributes_cookie_t,
    xcb_generic_error_t **);
xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t *, xcb_window_t);
xcb_query_tree_reply_t *xcb_query_tree_reply(
    xcb_connection_t *, xcb_query_tree_cookie_t, xcb_generic_error_t **);
int xcb_query_tree_children_length(const xcb_query_tree_reply_t *);
xcb_window_t *xcb_query_tree_children(const xcb_query_tree_reply_t *);
xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *, xcb_drawable_t);
xcb_get_geometry_reply_t *xcb_get_geometry_reply(
    xcb_connection_t *, xcb_get_geometry_cookie_t, xcb_generic_error_t **);
xcb_translate_coordinates_cookie_t xcb_translate_coordinates(
    xcb_connection_t *, xcb_window_t, xcb_window_t, int16_t, int16_t);
xcb_translate_coordinates_reply_t *xcb_translate_coordinates_reply(
    xcb_connection_t *, xcb_translate_coordinates_cookie_t,
    xcb_generic_error_t **);
xcb_get_input_focus_cookie_t xcb_get_input_focus(xcb_connection_t *);
xcb_get_input_focus_reply_t *xcb_get_input_focus_reply(
    xcb_connection_t *, xcb_get_input_focus_cookie_t, xcb_generic_error_t **);
xcb_get_keyboard_mapping_cookie_t xcb_get_keyboard_mapping(
    xcb_connection_t *, xcb_keycode_t, uint8_t);
xcb_get_keyboard_mapping_reply_t *xcb_get_keyboard_mapping_reply(
    xcb_connection_t *, xcb_get_keyboard_mapping_cookie_t,
    xcb_generic_error_t **);
xcb_keysym_t *xcb_get_keyboard_mapping_keysyms(
    const xcb_get_keyboard_mapping_reply_t *);
xcb_query_pointer_cookie_t xcb_query_pointer(xcb_connection_t *, xcb_window_t);
xcb_query_pointer_reply_t *xcb_query_pointer_reply(
    xcb_connection_t *, xcb_query_pointer_cookie_t, xcb_generic_error_t **);
xcb_get_image_cookie_t xcb_get_image(
    xcb_connection_t *, uint8_t, xcb_drawable_t, int16_t, int16_t,
    uint16_t, uint16_t, uint32_t);
xcb_get_image_reply_t *xcb_get_image_reply(
    xcb_connection_t *, xcb_get_image_cookie_t, xcb_generic_error_t **);
int xcb_get_image_data_length(const xcb_get_image_reply_t *);
uint8_t *xcb_get_image_data(const xcb_get_image_reply_t *);

const xcb_query_extension_reply_t *xcb_get_extension_data(
    xcb_connection_t *, xcb_extension_t *);
uint32_t xcb_generate_id(xcb_connection_t *);
xcb_generic_error_t *xcb_request_check(xcb_connection_t *, xcb_void_cookie_t);
int xcb_flush(xcb_connection_t *);
int xcb_connection_has_error(xcb_connection_t *);
int xcb_get_file_descriptor(xcb_connection_t *);
xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *);

xcb_void_cookie_t xcb_configure_window_checked(
    xcb_connection_t *, xcb_window_t, uint16_t, const uint32_t *);
xcb_void_cookie_t xcb_map_window_checked(xcb_connection_t *, xcb_window_t);
xcb_void_cookie_t xcb_unmap_window_checked(xcb_connection_t *, xcb_window_t);
xcb_void_cookie_t xcb_set_input_focus_checked(
    xcb_connection_t *, uint8_t, xcb_window_t, uint32_t);
xcb_void_cookie_t xcb_send_event_checked(
    xcb_connection_t *, uint8_t, xcb_window_t, uint32_t, const char *);
xcb_void_cookie_t xcb_free_pixmap(xcb_connection_t *, xcb_pixmap_t);

xcb_void_cookie_t xcb_test_fake_input_checked(
    xcb_connection_t *, uint8_t, uint8_t, uint32_t, xcb_window_t,
    int16_t, int16_t, uint8_t);
xcb_composite_query_version_cookie_t xcb_composite_query_version(
    xcb_connection_t *, uint32_t, uint32_t);
xcb_composite_query_version_reply_t *xcb_composite_query_version_reply(
    xcb_connection_t *, xcb_composite_query_version_cookie_t,
    xcb_generic_error_t **);
xcb_void_cookie_t xcb_composite_redirect_window_checked(
    xcb_connection_t *, xcb_window_t, uint8_t);
xcb_void_cookie_t xcb_composite_name_window_pixmap_checked(
    xcb_connection_t *, xcb_window_t, xcb_pixmap_t);
xcb_void_cookie_t xcb_composite_unredirect_window(
    xcb_connection_t *, xcb_window_t, uint8_t);
xcb_damage_query_version_cookie_t xcb_damage_query_version(
    xcb_connection_t *, uint32_t, uint32_t);
xcb_damage_query_version_reply_t *xcb_damage_query_version_reply(
    xcb_connection_t *, xcb_damage_query_version_cookie_t,
    xcb_generic_error_t **);
xcb_void_cookie_t xcb_damage_create_checked(
    xcb_connection_t *, xcb_damage_damage_t, xcb_drawable_t, uint8_t);
xcb_void_cookie_t xcb_damage_subtract(
    xcb_connection_t *, xcb_damage_damage_t, uint32_t, uint32_t);
xcb_void_cookie_t xcb_damage_destroy(
    xcb_connection_t *, xcb_damage_damage_t);

#define XCB_ATOM_NONE 0U
#define XCB_ATOM_WM_NAME 39U
#define XCB_GET_PROPERTY_TYPE_ANY 0U
#define XCB_WINDOW_NONE 0U
#define XCB_PIXMAP_NONE 0U
#define XCB_NO_SYMBOL 0U
#define XCB_CURRENT_TIME 0U
#define XCB_XFIXES_REGION_NONE 0U
#define XCB_EVENT_MASK_NO_EVENT 0U
#define XCB_IMAGE_ORDER_LSB_FIRST 0U
#define XCB_MAP_STATE_VIEWABLE 2U
#define XCB_INPUT_FOCUS_PARENT 2U
#define XCB_STACK_MODE_ABOVE 0U
#define XCB_COMPOSITE_REDIRECT_AUTOMATIC 0U
#define XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY 3U
#define XCB_DAMAGE_NOTIFY 0U
#define XCB_IMAGE_FORMAT_Z_PIXMAP 2U
#define XCB_KEY_PRESS 2U
#define XCB_KEY_RELEASE 3U
#define XCB_BUTTON_PRESS 4U
#define XCB_BUTTON_RELEASE 5U
#define XCB_MOTION_NOTIFY 6U
#define XCB_CLIENT_MESSAGE 33U
#define XCB_CONFIG_WINDOW_X (1U << 0)
#define XCB_CONFIG_WINDOW_Y (1U << 1)
#define XCB_CONFIG_WINDOW_WIDTH (1U << 2)
#define XCB_CONFIG_WINDOW_HEIGHT (1U << 3)
#define XCB_CONFIG_WINDOW_STACK_MODE (1U << 6)

#ifdef __cplusplus
}
#endif

#endif
