/*
 * Minimal XCB wire surface required by Qt's qxcb platform plugin.
 *
 * These functions are stateless protocol encoders and reply accessors.  The
 * connection, authentication, sequencing, descriptor, reply, and event
 * implementation is Xmin's C++17 client core.  The declarations and wire
 * layouts come from the pinned xcb-proto interface material.
 */
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/sync.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>
#include <xcb/xkb.h>
#include <xcb/xproto.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>

#define ALIGNOF(type) alignof(type)

extern "C" {
xcb_extension_t xcb_randr_id = {"RANDR", 0};
xcb_extension_t xcb_render_id = {"RENDER", 0};
xcb_extension_t xcb_shape_id = {"SHAPE", 0};
xcb_extension_t xcb_shm_id = {"MIT-SHM", 0};
xcb_extension_t xcb_sync_id = {"SYNC", 0};
xcb_extension_t xcb_xfixes_id = {"XFIXES", 0};
xcb_extension_t xcb_xkb_id = {"XKEYBOARD", 0};
}

/* xproto.c: xcb_bell */
xcb_void_cookie_t
xcb_bell (xcb_connection_t *c,
          int8_t            percent)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_BELL, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_bell_request_t xcb_out;

    xcb_out.percent = percent;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_change_gc */
xcb_void_cookie_t
xcb_change_gc (xcb_connection_t *c,
               xcb_gcontext_t    gc,
               uint32_t          value_mask,
               const void       *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, 0, XCB_CHANGE_GC, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_change_gc_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.gc = gc;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_change_gc_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_change_gc_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_change_property */
xcb_void_cookie_t
xcb_change_property (xcb_connection_t *c,
                     uint8_t           mode,
                     xcb_window_t      window,
                     xcb_atom_t        property,
                     xcb_atom_t        type,
                     uint8_t           format,
                     uint32_t          data_len,
                     const void       *data)
{
    static const xcb_protocol_request_t xcb_req{
        4, 0, XCB_CHANGE_PROPERTY, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_change_property_request_t xcb_out;

    xcb_out.mode = mode;
    xcb_out.window = window;
    xcb_out.property = property;
    xcb_out.type = type;
    xcb_out.format = format;
    memset(xcb_out.pad0, 0, 3);
    xcb_out.data_len = data_len;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* void data */
    xcb_parts[4].iov_base = (char *) data;
    xcb_parts[4].iov_len = ((data_len * format) / 8) * sizeof(char);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_change_window_attributes */
xcb_void_cookie_t
xcb_change_window_attributes (xcb_connection_t *c,
                              xcb_window_t      window,
                              uint32_t          value_mask,
                              const void       *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, 0, XCB_CHANGE_WINDOW_ATTRIBUTES, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_change_window_attributes_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_change_window_attributes_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_change_window_attributes_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_clear_area */
xcb_void_cookie_t
xcb_clear_area (xcb_connection_t *c,
                uint8_t           exposures,
                xcb_window_t      window,
                int16_t           x,
                int16_t           y,
                uint16_t          width,
                uint16_t          height)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CLEAR_AREA, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_clear_area_request_t xcb_out;

    xcb_out.exposures = exposures;
    xcb_out.window = window;
    xcb_out.x = x;
    xcb_out.y = y;
    xcb_out.width = width;
    xcb_out.height = height;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_close_font */
xcb_void_cookie_t
xcb_close_font (xcb_connection_t *c,
                xcb_font_t        font)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CLOSE_FONT, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_close_font_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.font = font;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_configure_window */
xcb_void_cookie_t
xcb_configure_window (xcb_connection_t *c,
                      xcb_window_t      window,
                      uint16_t          value_mask,
                      const void       *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, 0, XCB_CONFIGURE_WINDOW, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_configure_window_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;
    xcb_out.value_mask = value_mask;
    memset(xcb_out.pad1, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_configure_window_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_configure_window_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_convert_selection */
xcb_void_cookie_t
xcb_convert_selection (xcb_connection_t *c,
                       xcb_window_t      requestor,
                       xcb_atom_t        selection,
                       xcb_atom_t        target,
                       xcb_atom_t        property,
                       xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CONVERT_SELECTION, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_convert_selection_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.requestor = requestor;
    xcb_out.selection = selection;
    xcb_out.target = target;
    xcb_out.property = property;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_copy_area */
xcb_void_cookie_t
xcb_copy_area (xcb_connection_t *c,
               xcb_drawable_t    src_drawable,
               xcb_drawable_t    dst_drawable,
               xcb_gcontext_t    gc,
               int16_t           src_x,
               int16_t           src_y,
               int16_t           dst_x,
               int16_t           dst_y,
               uint16_t          width,
               uint16_t          height)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_COPY_AREA, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_copy_area_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.src_drawable = src_drawable;
    xcb_out.dst_drawable = dst_drawable;
    xcb_out.gc = gc;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;
    xcb_out.dst_x = dst_x;
    xcb_out.dst_y = dst_y;
    xcb_out.width = width;
    xcb_out.height = height;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_colormap */
xcb_void_cookie_t
xcb_create_colormap (xcb_connection_t *c,
                     uint8_t           alloc,
                     xcb_colormap_t    mid,
                     xcb_window_t      window,
                     xcb_visualid_t    visual)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CREATE_COLORMAP, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_create_colormap_request_t xcb_out;

    xcb_out.alloc = alloc;
    xcb_out.mid = mid;
    xcb_out.window = window;
    xcb_out.visual = visual;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_cursor */
xcb_void_cookie_t
xcb_create_cursor (xcb_connection_t *c,
                   xcb_cursor_t      cid,
                   xcb_pixmap_t      source,
                   xcb_pixmap_t      mask,
                   uint16_t          fore_red,
                   uint16_t          fore_green,
                   uint16_t          fore_blue,
                   uint16_t          back_red,
                   uint16_t          back_green,
                   uint16_t          back_blue,
                   uint16_t          x,
                   uint16_t          y)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CREATE_CURSOR, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_create_cursor_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.cid = cid;
    xcb_out.source = source;
    xcb_out.mask = mask;
    xcb_out.fore_red = fore_red;
    xcb_out.fore_green = fore_green;
    xcb_out.fore_blue = fore_blue;
    xcb_out.back_red = back_red;
    xcb_out.back_green = back_green;
    xcb_out.back_blue = back_blue;
    xcb_out.x = x;
    xcb_out.y = y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_gc */
xcb_void_cookie_t
xcb_create_gc (xcb_connection_t *c,
               xcb_gcontext_t    cid,
               xcb_drawable_t    drawable,
               uint32_t          value_mask,
               const void       *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, 0, XCB_CREATE_GC, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_create_gc_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.cid = cid;
    xcb_out.drawable = drawable;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_create_gc_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_create_gc_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_glyph_cursor */
xcb_void_cookie_t
xcb_create_glyph_cursor (xcb_connection_t *c,
                         xcb_cursor_t      cid,
                         xcb_font_t        source_font,
                         xcb_font_t        mask_font,
                         uint16_t          source_char,
                         uint16_t          mask_char,
                         uint16_t          fore_red,
                         uint16_t          fore_green,
                         uint16_t          fore_blue,
                         uint16_t          back_red,
                         uint16_t          back_green,
                         uint16_t          back_blue)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CREATE_GLYPH_CURSOR, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_create_glyph_cursor_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.cid = cid;
    xcb_out.source_font = source_font;
    xcb_out.mask_font = mask_font;
    xcb_out.source_char = source_char;
    xcb_out.mask_char = mask_char;
    xcb_out.fore_red = fore_red;
    xcb_out.fore_green = fore_green;
    xcb_out.fore_blue = fore_blue;
    xcb_out.back_red = back_red;
    xcb_out.back_green = back_green;
    xcb_out.back_blue = back_blue;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_pixmap */
xcb_void_cookie_t
xcb_create_pixmap (xcb_connection_t *c,
                   uint8_t           depth,
                   xcb_pixmap_t      pid,
                   xcb_drawable_t    drawable,
                   uint16_t          width,
                   uint16_t          height)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_CREATE_PIXMAP, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_create_pixmap_request_t xcb_out;

    xcb_out.depth = depth;
    xcb_out.pid = pid;
    xcb_out.drawable = drawable;
    xcb_out.width = width;
    xcb_out.height = height;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_create_window */
xcb_void_cookie_t
xcb_create_window (xcb_connection_t *c,
                   uint8_t           depth,
                   xcb_window_t      wid,
                   xcb_window_t      parent,
                   int16_t           x,
                   int16_t           y,
                   uint16_t          width,
                   uint16_t          height,
                   uint16_t          border_width,
                   uint16_t          _class,
                   xcb_visualid_t    visual,
                   uint32_t          value_mask,
                   const void       *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, 0, XCB_CREATE_WINDOW, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_create_window_request_t xcb_out;

    xcb_out.depth = depth;
    xcb_out.wid = wid;
    xcb_out.parent = parent;
    xcb_out.x = x;
    xcb_out.y = y;
    xcb_out.width = width;
    xcb_out.height = height;
    xcb_out.border_width = border_width;
    xcb_out._class = _class;
    xcb_out.visual = visual;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_create_window_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_create_window_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_delete_property */
xcb_void_cookie_t
xcb_delete_property (xcb_connection_t *c,
                     xcb_window_t      window,
                     xcb_atom_t        property)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_DELETE_PROPERTY, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_delete_property_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;
    xcb_out.property = property;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_destroy_window */
xcb_void_cookie_t
xcb_destroy_window (xcb_connection_t *c,
                    xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_DESTROY_WINDOW, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_destroy_window_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_free_colormap */
xcb_void_cookie_t
xcb_free_colormap (xcb_connection_t *c,
                   xcb_colormap_t    cmap)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_FREE_COLORMAP, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_free_colormap_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.cmap = cmap;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_free_cursor */
xcb_void_cookie_t
xcb_free_cursor (xcb_connection_t *c,
                 xcb_cursor_t      cursor)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_FREE_CURSOR, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_free_cursor_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.cursor = cursor;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_free_gc */
xcb_void_cookie_t
xcb_free_gc (xcb_connection_t *c,
             xcb_gcontext_t    gc)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_FREE_GC, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_free_gc_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.gc = gc;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_free_pixmap */
xcb_void_cookie_t
xcb_free_pixmap (xcb_connection_t *c,
                 xcb_pixmap_t      pixmap)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_FREE_PIXMAP, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_free_pixmap_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.pixmap = pixmap;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_atom_name */
xcb_get_atom_name_cookie_t
xcb_get_atom_name (xcb_connection_t *c,
                   xcb_atom_t        atom)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_ATOM_NAME, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_atom_name_cookie_t xcb_ret;
    xcb_get_atom_name_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.atom = atom;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_atom_name_name */
char *
xcb_get_atom_name_name (const xcb_get_atom_name_reply_t *R)
{
    return (char *) (R + 1);
}

/* xproto.c: xcb_get_atom_name_name_length */
int
xcb_get_atom_name_name_length (const xcb_get_atom_name_reply_t *R)
{
    return R->name_len;
}

/* xproto.c: xcb_get_atom_name_reply */
xcb_get_atom_name_reply_t *
xcb_get_atom_name_reply (xcb_connection_t            *c,
                         xcb_get_atom_name_cookie_t   cookie  /**< */,
                         xcb_generic_error_t        **e)
{
    return (xcb_get_atom_name_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_geometry */
xcb_get_geometry_cookie_t
xcb_get_geometry (xcb_connection_t *c,
                  xcb_drawable_t    drawable)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_GEOMETRY, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_geometry_cookie_t xcb_ret;
    xcb_get_geometry_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.drawable = drawable;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_geometry_reply */
xcb_get_geometry_reply_t *
xcb_get_geometry_reply (xcb_connection_t           *c,
                        xcb_get_geometry_cookie_t   cookie  /**< */,
                        xcb_generic_error_t       **e)
{
    return (xcb_get_geometry_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_geometry_unchecked */
xcb_get_geometry_cookie_t
xcb_get_geometry_unchecked (xcb_connection_t *c,
                            xcb_drawable_t    drawable)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_GEOMETRY, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_geometry_cookie_t xcb_ret;
    xcb_get_geometry_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.drawable = drawable;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_image_data */
uint8_t *
xcb_get_image_data (const xcb_get_image_reply_t *R)
{
    return (uint8_t *) (R + 1);
}

/* xproto.c: xcb_get_image_data_length */
int
xcb_get_image_data_length (const xcb_get_image_reply_t *R)
{
    return (R->length * 4);
}

/* xproto.c: xcb_get_image_reply */
xcb_get_image_reply_t *
xcb_get_image_reply (xcb_connection_t        *c,
                     xcb_get_image_cookie_t   cookie  /**< */,
                     xcb_generic_error_t    **e)
{
    return (xcb_get_image_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_image_unchecked */
xcb_get_image_cookie_t
xcb_get_image_unchecked (xcb_connection_t *c,
                         uint8_t           format,
                         xcb_drawable_t    drawable,
                         int16_t           x,
                         int16_t           y,
                         uint16_t          width,
                         uint16_t          height,
                         uint32_t          plane_mask)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_IMAGE, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_image_cookie_t xcb_ret;
    xcb_get_image_request_t xcb_out;

    xcb_out.format = format;
    xcb_out.drawable = drawable;
    xcb_out.x = x;
    xcb_out.y = y;
    xcb_out.width = width;
    xcb_out.height = height;
    xcb_out.plane_mask = plane_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_input_focus */
xcb_get_input_focus_cookie_t
xcb_get_input_focus (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_INPUT_FOCUS, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_input_focus_cookie_t xcb_ret;
    xcb_get_input_focus_request_t xcb_out;

    xcb_out.pad0 = 0;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_input_focus_reply */
xcb_get_input_focus_reply_t *
xcb_get_input_focus_reply (xcb_connection_t              *c,
                           xcb_get_input_focus_cookie_t   cookie  /**< */,
                           xcb_generic_error_t          **e)
{
    return (xcb_get_input_focus_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_keyboard_mapping */
xcb_get_keyboard_mapping_cookie_t
xcb_get_keyboard_mapping (xcb_connection_t *c,
                          xcb_keycode_t     first_keycode,
                          uint8_t           count)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_KEYBOARD_MAPPING, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_keyboard_mapping_cookie_t xcb_ret;
    xcb_get_keyboard_mapping_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.first_keycode = first_keycode;
    xcb_out.count = count;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_keyboard_mapping_keysyms */
xcb_keysym_t *
xcb_get_keyboard_mapping_keysyms (const xcb_get_keyboard_mapping_reply_t *R)
{
    return (xcb_keysym_t *) (R + 1);
}

/* xproto.c: xcb_get_keyboard_mapping_reply */
xcb_get_keyboard_mapping_reply_t *
xcb_get_keyboard_mapping_reply (xcb_connection_t                   *c,
                                xcb_get_keyboard_mapping_cookie_t   cookie  /**< */,
                                xcb_generic_error_t               **e)
{
    return (xcb_get_keyboard_mapping_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_modifier_mapping */
xcb_get_modifier_mapping_cookie_t
xcb_get_modifier_mapping (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_MODIFIER_MAPPING, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_modifier_mapping_cookie_t xcb_ret;
    xcb_get_modifier_mapping_request_t xcb_out;

    xcb_out.pad0 = 0;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_modifier_mapping_keycodes */
xcb_keycode_t *
xcb_get_modifier_mapping_keycodes (const xcb_get_modifier_mapping_reply_t *R)
{
    return (xcb_keycode_t *) (R + 1);
}

/* xproto.c: xcb_get_modifier_mapping_keycodes_length */
int
xcb_get_modifier_mapping_keycodes_length (const xcb_get_modifier_mapping_reply_t *R)
{
    return (R->keycodes_per_modifier * 8);
}

/* xproto.c: xcb_get_modifier_mapping_reply */
xcb_get_modifier_mapping_reply_t *
xcb_get_modifier_mapping_reply (xcb_connection_t                   *c,
                                xcb_get_modifier_mapping_cookie_t   cookie  /**< */,
                                xcb_generic_error_t               **e)
{
    return (xcb_get_modifier_mapping_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_property */
xcb_get_property_cookie_t
xcb_get_property (xcb_connection_t *c,
                  uint8_t           _delete,
                  xcb_window_t      window,
                  xcb_atom_t        property,
                  xcb_atom_t        type,
                  uint32_t          long_offset,
                  uint32_t          long_length)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_PROPERTY, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_property_cookie_t xcb_ret;
    xcb_get_property_request_t xcb_out;

    xcb_out._delete = _delete;
    xcb_out.window = window;
    xcb_out.property = property;
    xcb_out.type = type;
    xcb_out.long_offset = long_offset;
    xcb_out.long_length = long_length;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_property_reply */
xcb_get_property_reply_t *
xcb_get_property_reply (xcb_connection_t           *c,
                        xcb_get_property_cookie_t   cookie  /**< */,
                        xcb_generic_error_t       **e)
{
    return (xcb_get_property_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_property_unchecked */
xcb_get_property_cookie_t
xcb_get_property_unchecked (xcb_connection_t *c,
                            uint8_t           _delete,
                            xcb_window_t      window,
                            xcb_atom_t        property,
                            xcb_atom_t        type,
                            uint32_t          long_offset,
                            uint32_t          long_length)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_PROPERTY, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_property_cookie_t xcb_ret;
    xcb_get_property_request_t xcb_out;

    xcb_out._delete = _delete;
    xcb_out.window = window;
    xcb_out.property = property;
    xcb_out.type = type;
    xcb_out.long_offset = long_offset;
    xcb_out.long_length = long_length;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_property_value */
void *
xcb_get_property_value (const xcb_get_property_reply_t *R)
{
    return (void *) (R + 1);
}

/* xproto.c: xcb_get_property_value_length */
int
xcb_get_property_value_length (const xcb_get_property_reply_t *R)
{
    return (R->value_len * (R->format / 8));
}

/* xproto.c: xcb_get_selection_owner */
xcb_get_selection_owner_cookie_t
xcb_get_selection_owner (xcb_connection_t *c,
                         xcb_atom_t        selection)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_SELECTION_OWNER, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_selection_owner_cookie_t xcb_ret;
    xcb_get_selection_owner_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.selection = selection;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_selection_owner_reply */
xcb_get_selection_owner_reply_t *
xcb_get_selection_owner_reply (xcb_connection_t                  *c,
                               xcb_get_selection_owner_cookie_t   cookie  /**< */,
                               xcb_generic_error_t              **e)
{
    return (xcb_get_selection_owner_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_window_attributes */
xcb_get_window_attributes_cookie_t
xcb_get_window_attributes (xcb_connection_t *c,
                           xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_WINDOW_ATTRIBUTES, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_window_attributes_cookie_t xcb_ret;
    xcb_get_window_attributes_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_get_window_attributes_reply */
xcb_get_window_attributes_reply_t *
xcb_get_window_attributes_reply (xcb_connection_t                    *c,
                                 xcb_get_window_attributes_cookie_t   cookie  /**< */,
                                 xcb_generic_error_t                **e)
{
    return (xcb_get_window_attributes_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_get_window_attributes_unchecked */
xcb_get_window_attributes_cookie_t
xcb_get_window_attributes_unchecked (xcb_connection_t *c,
                                     xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GET_WINDOW_ATTRIBUTES, 0
    };

    struct iovec xcb_parts[4];
    xcb_get_window_attributes_cookie_t xcb_ret;
    xcb_get_window_attributes_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_grab_keyboard */
xcb_grab_keyboard_cookie_t
xcb_grab_keyboard (xcb_connection_t *c,
                   uint8_t           owner_events,
                   xcb_window_t      grab_window,
                   xcb_timestamp_t   time,
                   uint8_t           pointer_mode,
                   uint8_t           keyboard_mode)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GRAB_KEYBOARD, 0
    };

    struct iovec xcb_parts[4];
    xcb_grab_keyboard_cookie_t xcb_ret;
    xcb_grab_keyboard_request_t xcb_out;

    xcb_out.owner_events = owner_events;
    xcb_out.grab_window = grab_window;
    xcb_out.time = time;
    xcb_out.pointer_mode = pointer_mode;
    xcb_out.keyboard_mode = keyboard_mode;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_grab_keyboard_reply */
xcb_grab_keyboard_reply_t *
xcb_grab_keyboard_reply (xcb_connection_t            *c,
                         xcb_grab_keyboard_cookie_t   cookie  /**< */,
                         xcb_generic_error_t        **e)
{
    return (xcb_grab_keyboard_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_grab_pointer */
xcb_grab_pointer_cookie_t
xcb_grab_pointer (xcb_connection_t *c,
                  uint8_t           owner_events,
                  xcb_window_t      grab_window,
                  uint16_t          event_mask,
                  uint8_t           pointer_mode,
                  uint8_t           keyboard_mode,
                  xcb_window_t      confine_to,
                  xcb_cursor_t      cursor,
                  xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GRAB_POINTER, 0
    };

    struct iovec xcb_parts[4];
    xcb_grab_pointer_cookie_t xcb_ret;
    xcb_grab_pointer_request_t xcb_out;

    xcb_out.owner_events = owner_events;
    xcb_out.grab_window = grab_window;
    xcb_out.event_mask = event_mask;
    xcb_out.pointer_mode = pointer_mode;
    xcb_out.keyboard_mode = keyboard_mode;
    xcb_out.confine_to = confine_to;
    xcb_out.cursor = cursor;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_grab_pointer_reply */
xcb_grab_pointer_reply_t *
xcb_grab_pointer_reply (xcb_connection_t           *c,
                        xcb_grab_pointer_cookie_t   cookie  /**< */,
                        xcb_generic_error_t       **e)
{
    return (xcb_grab_pointer_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_grab_server */
xcb_void_cookie_t
xcb_grab_server (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_GRAB_SERVER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_grab_server_request_t xcb_out;

    xcb_out.pad0 = 0;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_intern_atom */
xcb_intern_atom_cookie_t
xcb_intern_atom (xcb_connection_t *c,
                 uint8_t           only_if_exists,
                 uint16_t          name_len,
                 const char       *name)
{
    static const xcb_protocol_request_t xcb_req{
        4, 0, XCB_INTERN_ATOM, 0
    };

    struct iovec xcb_parts[6];
    xcb_intern_atom_cookie_t xcb_ret;
    xcb_intern_atom_request_t xcb_out;

    xcb_out.only_if_exists = only_if_exists;
    xcb_out.name_len = name_len;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* char name */
    xcb_parts[4].iov_base = (char *) name;
    xcb_parts[4].iov_len = name_len * sizeof(char);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_intern_atom_reply */
xcb_intern_atom_reply_t *
xcb_intern_atom_reply (xcb_connection_t          *c,
                       xcb_intern_atom_cookie_t   cookie  /**< */,
                       xcb_generic_error_t      **e)
{
    return (xcb_intern_atom_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_map_window */
xcb_void_cookie_t
xcb_map_window (xcb_connection_t *c,
                xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_MAP_WINDOW, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_map_window_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_open_font */
xcb_void_cookie_t
xcb_open_font (xcb_connection_t *c,
               xcb_font_t        fid,
               uint16_t          name_len,
               const char       *name)
{
    static const xcb_protocol_request_t xcb_req{
        4, 0, XCB_OPEN_FONT, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_open_font_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.fid = fid;
    xcb_out.name_len = name_len;
    memset(xcb_out.pad1, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* char name */
    xcb_parts[4].iov_base = (char *) name;
    xcb_parts[4].iov_len = name_len * sizeof(char);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_query_pointer */
xcb_query_pointer_cookie_t
xcb_query_pointer (xcb_connection_t *c,
                   xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_QUERY_POINTER, 0
    };

    struct iovec xcb_parts[4];
    xcb_query_pointer_cookie_t xcb_ret;
    xcb_query_pointer_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_query_pointer_reply */
xcb_query_pointer_reply_t *
xcb_query_pointer_reply (xcb_connection_t            *c,
                         xcb_query_pointer_cookie_t   cookie  /**< */,
                         xcb_generic_error_t        **e)
{
    return (xcb_query_pointer_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_query_tree */
xcb_query_tree_cookie_t
xcb_query_tree (xcb_connection_t *c,
                xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_QUERY_TREE, 0
    };

    struct iovec xcb_parts[4];
    xcb_query_tree_cookie_t xcb_ret;
    xcb_query_tree_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_query_tree_children */
xcb_window_t *
xcb_query_tree_children (const xcb_query_tree_reply_t *R)
{
    return (xcb_window_t *) (R + 1);
}

/* xproto.c: xcb_query_tree_children_length */
int
xcb_query_tree_children_length (const xcb_query_tree_reply_t *R)
{
    return R->children_len;
}

/* xproto.c: xcb_query_tree_reply */
xcb_query_tree_reply_t *
xcb_query_tree_reply (xcb_connection_t         *c,
                      xcb_query_tree_cookie_t   cookie  /**< */,
                      xcb_generic_error_t     **e)
{
    return (xcb_query_tree_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_query_tree_unchecked */
xcb_query_tree_cookie_t
xcb_query_tree_unchecked (xcb_connection_t *c,
                          xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_QUERY_TREE, 0
    };

    struct iovec xcb_parts[4];
    xcb_query_tree_cookie_t xcb_ret;
    xcb_query_tree_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_crtc_info */
xcb_randr_get_crtc_info_cookie_t
xcb_randr_get_crtc_info (xcb_connection_t *c,
                         xcb_randr_crtc_t  crtc,
                         xcb_timestamp_t   config_timestamp)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_CRTC_INFO, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_crtc_info_cookie_t xcb_ret;
    xcb_randr_get_crtc_info_request_t xcb_out;

    xcb_out.crtc = crtc;
    xcb_out.config_timestamp = config_timestamp;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_crtc_info_reply */
xcb_randr_get_crtc_info_reply_t *
xcb_randr_get_crtc_info_reply (xcb_connection_t                  *c,
                               xcb_randr_get_crtc_info_cookie_t   cookie  /**< */,
                               xcb_generic_error_t              **e)
{
    return (xcb_randr_get_crtc_info_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_crtc_info_unchecked */
xcb_randr_get_crtc_info_cookie_t
xcb_randr_get_crtc_info_unchecked (xcb_connection_t *c,
                                   xcb_randr_crtc_t  crtc,
                                   xcb_timestamp_t   config_timestamp)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_CRTC_INFO, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_crtc_info_cookie_t xcb_ret;
    xcb_randr_get_crtc_info_request_t xcb_out;

    xcb_out.crtc = crtc;
    xcb_out.config_timestamp = config_timestamp;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_monitors */
xcb_randr_get_monitors_cookie_t
xcb_randr_get_monitors (xcb_connection_t *c,
                        xcb_window_t      window,
                        uint8_t           get_active)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_MONITORS, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_monitors_cookie_t xcb_ret;
    xcb_randr_get_monitors_request_t xcb_out;

    xcb_out.window = window;
    xcb_out.get_active = get_active;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_monitors_monitors_iterator */
xcb_randr_monitor_info_iterator_t
xcb_randr_get_monitors_monitors_iterator (const xcb_randr_get_monitors_reply_t *R)
{
    xcb_randr_monitor_info_iterator_t i;
    i.data = (xcb_randr_monitor_info_t *) (R + 1);
    i.rem = R->nMonitors;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_get_monitors_reply */
xcb_randr_get_monitors_reply_t *
xcb_randr_get_monitors_reply (xcb_connection_t                 *c,
                              xcb_randr_get_monitors_cookie_t   cookie  /**< */,
                              xcb_generic_error_t             **e)
{
    return (xcb_randr_get_monitors_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_output_info */
xcb_randr_get_output_info_cookie_t
xcb_randr_get_output_info (xcb_connection_t   *c,
                           xcb_randr_output_t  output,
                           xcb_timestamp_t     config_timestamp)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_OUTPUT_INFO, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_output_info_cookie_t xcb_ret;
    xcb_randr_get_output_info_request_t xcb_out;

    xcb_out.output = output;
    xcb_out.config_timestamp = config_timestamp;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_output_info_name */
uint8_t *
xcb_randr_get_output_info_name (const xcb_randr_get_output_info_reply_t *R)
{
    xcb_generic_iterator_t prev = xcb_randr_get_output_info_clones_end(R);
    return (uint8_t *) ((char *) prev.data + XCB_TYPE_PAD(uint8_t, prev.index) + 0);
}

/* randr.c: xcb_randr_get_output_info_name_length */
int
xcb_randr_get_output_info_name_length (const xcb_randr_get_output_info_reply_t *R)
{
    return R->name_len;
}

/* randr.c: xcb_randr_get_output_info_reply */
xcb_randr_get_output_info_reply_t *
xcb_randr_get_output_info_reply (xcb_connection_t                    *c,
                                 xcb_randr_get_output_info_cookie_t   cookie  /**< */,
                                 xcb_generic_error_t                **e)
{
    return (xcb_randr_get_output_info_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_output_info_unchecked */
xcb_randr_get_output_info_cookie_t
xcb_randr_get_output_info_unchecked (xcb_connection_t   *c,
                                     xcb_randr_output_t  output,
                                     xcb_timestamp_t     config_timestamp)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_OUTPUT_INFO, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_output_info_cookie_t xcb_ret;
    xcb_randr_get_output_info_request_t xcb_out;

    xcb_out.output = output;
    xcb_out.config_timestamp = config_timestamp;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_output_primary */
xcb_randr_get_output_primary_cookie_t
xcb_randr_get_output_primary (xcb_connection_t *c,
                              xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_OUTPUT_PRIMARY, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_output_primary_cookie_t xcb_ret;
    xcb_randr_get_output_primary_request_t xcb_out;

    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_output_primary_reply */
xcb_randr_get_output_primary_reply_t *
xcb_randr_get_output_primary_reply (xcb_connection_t                       *c,
                                    xcb_randr_get_output_primary_cookie_t   cookie  /**< */,
                                    xcb_generic_error_t                   **e)
{
    return (xcb_randr_get_output_primary_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_output_property */
xcb_randr_get_output_property_cookie_t
xcb_randr_get_output_property (xcb_connection_t   *c,
                               xcb_randr_output_t  output,
                               xcb_atom_t          property,
                               xcb_atom_t          type,
                               uint32_t            long_offset,
                               uint32_t            long_length,
                               uint8_t             _delete,
                               uint8_t             pending)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_OUTPUT_PROPERTY, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_output_property_cookie_t xcb_ret;
    xcb_randr_get_output_property_request_t xcb_out;

    xcb_out.output = output;
    xcb_out.property = property;
    xcb_out.type = type;
    xcb_out.long_offset = long_offset;
    xcb_out.long_length = long_length;
    xcb_out._delete = _delete;
    xcb_out.pending = pending;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_output_property_data */
uint8_t *
xcb_randr_get_output_property_data (const xcb_randr_get_output_property_reply_t *R)
{
    return (uint8_t *) (R + 1);
}

/* randr.c: xcb_randr_get_output_property_reply */
xcb_randr_get_output_property_reply_t *
xcb_randr_get_output_property_reply (xcb_connection_t                        *c,
                                     xcb_randr_get_output_property_cookie_t   cookie  /**< */,
                                     xcb_generic_error_t                    **e)
{
    return (xcb_randr_get_output_property_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_screen_resources */
xcb_randr_get_screen_resources_cookie_t
xcb_randr_get_screen_resources (xcb_connection_t *c,
                                xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_SCREEN_RESOURCES, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_screen_resources_cookie_t xcb_ret;
    xcb_randr_get_screen_resources_request_t xcb_out;

    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_screen_resources_current */
xcb_randr_get_screen_resources_current_cookie_t
xcb_randr_get_screen_resources_current (xcb_connection_t *c,
                                        xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_SCREEN_RESOURCES_CURRENT, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_screen_resources_current_cookie_t xcb_ret;
    xcb_randr_get_screen_resources_current_request_t xcb_out;

    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_screen_resources_current_modes_iterator */
xcb_randr_mode_info_iterator_t
xcb_randr_get_screen_resources_current_modes_iterator (const xcb_randr_get_screen_resources_current_reply_t *R)
{
    xcb_randr_mode_info_iterator_t i;
    xcb_generic_iterator_t prev = xcb_randr_get_screen_resources_current_outputs_end(R);
    i.data = (xcb_randr_mode_info_t *) ((char *) prev.data + XCB_TYPE_PAD(xcb_randr_mode_info_t, prev.index));
    i.rem = R->num_modes;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_get_screen_resources_current_outputs */
xcb_randr_output_t *
xcb_randr_get_screen_resources_current_outputs (const xcb_randr_get_screen_resources_current_reply_t *R)
{
    xcb_generic_iterator_t prev = xcb_randr_get_screen_resources_current_crtcs_end(R);
    return (xcb_randr_output_t *) ((char *) prev.data + XCB_TYPE_PAD(xcb_randr_output_t, prev.index) + 0);
}

/* randr.c: xcb_randr_get_screen_resources_current_outputs_length */
int
xcb_randr_get_screen_resources_current_outputs_length (const xcb_randr_get_screen_resources_current_reply_t *R)
{
    return R->num_outputs;
}

/* randr.c: xcb_randr_get_screen_resources_current_reply */
xcb_randr_get_screen_resources_current_reply_t *
xcb_randr_get_screen_resources_current_reply (xcb_connection_t                                 *c,
                                              xcb_randr_get_screen_resources_current_cookie_t   cookie  /**< */,
                                              xcb_generic_error_t                             **e)
{
    return (xcb_randr_get_screen_resources_current_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_get_screen_resources_current_unchecked */
xcb_randr_get_screen_resources_current_cookie_t
xcb_randr_get_screen_resources_current_unchecked (xcb_connection_t *c,
                                                  xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_GET_SCREEN_RESOURCES_CURRENT, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_get_screen_resources_current_cookie_t xcb_ret;
    xcb_randr_get_screen_resources_current_request_t xcb_out;

    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_get_screen_resources_outputs */
xcb_randr_output_t *
xcb_randr_get_screen_resources_outputs (const xcb_randr_get_screen_resources_reply_t *R)
{
    xcb_generic_iterator_t prev = xcb_randr_get_screen_resources_crtcs_end(R);
    return (xcb_randr_output_t *) ((char *) prev.data + XCB_TYPE_PAD(xcb_randr_output_t, prev.index) + 0);
}

/* randr.c: xcb_randr_get_screen_resources_outputs_length */
int
xcb_randr_get_screen_resources_outputs_length (const xcb_randr_get_screen_resources_reply_t *R)
{
    return R->num_outputs;
}

/* randr.c: xcb_randr_get_screen_resources_reply */
xcb_randr_get_screen_resources_reply_t *
xcb_randr_get_screen_resources_reply (xcb_connection_t                         *c,
                                      xcb_randr_get_screen_resources_cookie_t   cookie  /**< */,
                                      xcb_generic_error_t                     **e)
{
    return (xcb_randr_get_screen_resources_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_mode_info_next */
void
xcb_randr_mode_info_next (xcb_randr_mode_info_iterator_t *i)
{
    --i->rem;
    ++i->data;
    i->index += sizeof(xcb_randr_mode_info_t);
}

/* randr.c: xcb_randr_monitor_info_next */
void
xcb_randr_monitor_info_next (xcb_randr_monitor_info_iterator_t *i)
{
    xcb_randr_monitor_info_t *R = i->data;
    xcb_generic_iterator_t child;
    child.data = (xcb_randr_monitor_info_t *)(((char *)R) + xcb_randr_monitor_info_sizeof(R));
    i->index = (char *) child.data - (char *) i->data;
    --i->rem;
    i->data = (xcb_randr_monitor_info_t *) child.data;
}

/* randr.c: xcb_randr_monitor_info_outputs */
xcb_randr_output_t *
xcb_randr_monitor_info_outputs (const xcb_randr_monitor_info_t *R)
{
    return (xcb_randr_output_t *) (R + 1);
}

/* randr.c: xcb_randr_monitor_info_outputs_length */
int
xcb_randr_monitor_info_outputs_length (const xcb_randr_monitor_info_t *R)
{
    return R->nOutput;
}

/* randr.c: xcb_randr_query_version */
xcb_randr_query_version_cookie_t
xcb_randr_query_version (xcb_connection_t *c,
                         uint32_t          major_version,
                         uint32_t          minor_version)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_QUERY_VERSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_randr_query_version_cookie_t xcb_ret;
    xcb_randr_query_version_request_t xcb_out;

    xcb_out.major_version = major_version;
    xcb_out.minor_version = minor_version;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* randr.c: xcb_randr_query_version_reply */
xcb_randr_query_version_reply_t *
xcb_randr_query_version_reply (xcb_connection_t                  *c,
                               xcb_randr_query_version_cookie_t   cookie  /**< */,
                               xcb_generic_error_t              **e)
{
    return (xcb_randr_query_version_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* randr.c: xcb_randr_select_input */
xcb_void_cookie_t
xcb_randr_select_input (xcb_connection_t *c,
                        xcb_window_t      window,
                        uint16_t          enable)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_randr_id, XCB_RANDR_SELECT_INPUT, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_randr_select_input_request_t xcb_out;

    xcb_out.window = window;
    xcb_out.enable = enable;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_composite */
xcb_void_cookie_t
xcb_render_composite (xcb_connection_t     *c,
                      uint8_t               op,
                      xcb_render_picture_t  src,
                      xcb_render_picture_t  mask,
                      xcb_render_picture_t  dst,
                      int16_t               src_x,
                      int16_t               src_y,
                      int16_t               mask_x,
                      int16_t               mask_y,
                      int16_t               dst_x,
                      int16_t               dst_y,
                      uint16_t              width,
                      uint16_t              height)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_render_id, XCB_RENDER_COMPOSITE, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_render_composite_request_t xcb_out;

    xcb_out.op = op;
    memset(xcb_out.pad0, 0, 3);
    xcb_out.src = src;
    xcb_out.mask = mask;
    xcb_out.dst = dst;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;
    xcb_out.mask_x = mask_x;
    xcb_out.mask_y = mask_y;
    xcb_out.dst_x = dst_x;
    xcb_out.dst_y = dst_y;
    xcb_out.width = width;
    xcb_out.height = height;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_create_cursor */
xcb_void_cookie_t
xcb_render_create_cursor (xcb_connection_t     *c,
                          xcb_cursor_t          cid,
                          xcb_render_picture_t  source,
                          uint16_t              x,
                          uint16_t              y)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_render_id, XCB_RENDER_CREATE_CURSOR, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_render_create_cursor_request_t xcb_out;

    xcb_out.cid = cid;
    xcb_out.source = source;
    xcb_out.x = x;
    xcb_out.y = y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_create_picture */
xcb_void_cookie_t
xcb_render_create_picture (xcb_connection_t        *c,
                           xcb_render_picture_t     pid,
                           xcb_drawable_t           drawable,
                           xcb_render_pictformat_t  format,
                           uint32_t                 value_mask,
                           const void              *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, &xcb_render_id, XCB_RENDER_CREATE_PICTURE, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_render_create_picture_request_t xcb_out;

    xcb_out.pid = pid;
    xcb_out.drawable = drawable;
    xcb_out.format = format;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_render_create_picture_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_render_create_picture_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_create_picture_checked */
xcb_void_cookie_t
xcb_render_create_picture_checked (xcb_connection_t        *c,
                                   xcb_render_picture_t     pid,
                                   xcb_drawable_t           drawable,
                                   xcb_render_pictformat_t  format,
                                   uint32_t                 value_mask,
                                   const void              *value_list)
{
    static const xcb_protocol_request_t xcb_req{
        3, &xcb_render_id, XCB_RENDER_CREATE_PICTURE, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_render_create_picture_request_t xcb_out;

    xcb_out.pid = pid;
    xcb_out.drawable = drawable;
    xcb_out.format = format;
    xcb_out.value_mask = value_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_render_create_picture_value_list_t value_list */
    xcb_parts[4].iov_base = (char *) value_list;
    xcb_parts[4].iov_len =
      xcb_render_create_picture_value_list_sizeof (value_list, value_mask);

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_free_picture */
xcb_void_cookie_t
xcb_render_free_picture (xcb_connection_t     *c,
                         xcb_render_picture_t  picture)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_render_id, XCB_RENDER_FREE_PICTURE, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_render_free_picture_request_t xcb_out;

    xcb_out.picture = picture;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_query_pict_formats */
xcb_render_query_pict_formats_cookie_t
xcb_render_query_pict_formats (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_render_id, XCB_RENDER_QUERY_PICT_FORMATS, 0
    };

    struct iovec xcb_parts[4];
    xcb_render_query_pict_formats_cookie_t xcb_ret;
    xcb_render_query_pict_formats_request_t xcb_out;


    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_query_pict_formats_reply */
xcb_render_query_pict_formats_reply_t *
xcb_render_query_pict_formats_reply (xcb_connection_t                        *c,
                                     xcb_render_query_pict_formats_cookie_t   cookie  /**< */,
                                     xcb_generic_error_t                    **e)
{
    return (xcb_render_query_pict_formats_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* render.c: xcb_render_query_version */
xcb_render_query_version_cookie_t
xcb_render_query_version (xcb_connection_t *c,
                          uint32_t          client_major_version,
                          uint32_t          client_minor_version)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_render_id, XCB_RENDER_QUERY_VERSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_render_query_version_cookie_t xcb_ret;
    xcb_render_query_version_request_t xcb_out;

    xcb_out.client_major_version = client_major_version;
    xcb_out.client_minor_version = client_minor_version;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* render.c: xcb_render_query_version_reply */
xcb_render_query_version_reply_t *
xcb_render_query_version_reply (xcb_connection_t                   *c,
                                xcb_render_query_version_cookie_t   cookie  /**< */,
                                xcb_generic_error_t               **e)
{
    return (xcb_render_query_version_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_reparent_window */
xcb_void_cookie_t
xcb_reparent_window (xcb_connection_t *c,
                     xcb_window_t      window,
                     xcb_window_t      parent,
                     int16_t           x,
                     int16_t           y)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_REPARENT_WINDOW, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_reparent_window_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;
    xcb_out.parent = parent;
    xcb_out.x = x;
    xcb_out.y = y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_send_event */
xcb_void_cookie_t
xcb_send_event (xcb_connection_t *c,
                uint8_t           propagate,
                xcb_window_t      destination,
                uint32_t          event_mask,
                const char       *event)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_SEND_EVENT, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_send_event_request_t xcb_out;

    xcb_out.propagate = propagate;
    xcb_out.destination = destination;
    xcb_out.event_mask = event_mask;
    memcpy(xcb_out.event, event, 32);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_set_clip_rectangles */
xcb_void_cookie_t
xcb_set_clip_rectangles (xcb_connection_t      *c,
                         uint8_t                ordering,
                         xcb_gcontext_t         gc,
                         int16_t                clip_x_origin,
                         int16_t                clip_y_origin,
                         uint32_t               rectangles_len,
                         const xcb_rectangle_t *rectangles)
{
    static const xcb_protocol_request_t xcb_req{
        4, 0, XCB_SET_CLIP_RECTANGLES, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_set_clip_rectangles_request_t xcb_out;

    xcb_out.ordering = ordering;
    xcb_out.gc = gc;
    xcb_out.clip_x_origin = clip_x_origin;
    xcb_out.clip_y_origin = clip_y_origin;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_rectangle_t rectangles */
    xcb_parts[4].iov_base = (char *) rectangles;
    xcb_parts[4].iov_len = rectangles_len * sizeof(xcb_rectangle_t);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_set_input_focus */
xcb_void_cookie_t
xcb_set_input_focus (xcb_connection_t *c,
                     uint8_t           revert_to,
                     xcb_window_t      focus,
                     xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_SET_INPUT_FOCUS, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_set_input_focus_request_t xcb_out;

    xcb_out.revert_to = revert_to;
    xcb_out.focus = focus;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_set_selection_owner */
xcb_void_cookie_t
xcb_set_selection_owner (xcb_connection_t *c,
                         xcb_window_t      owner,
                         xcb_atom_t        selection,
                         xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_SET_SELECTION_OWNER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_set_selection_owner_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.owner = owner;
    xcb_out.selection = selection;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shape.c: xcb_shape_get_rectangles */
xcb_shape_get_rectangles_cookie_t
xcb_shape_get_rectangles (xcb_connection_t *c,
                          xcb_window_t      window,
                          xcb_shape_kind_t  source_kind)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shape_id, XCB_SHAPE_GET_RECTANGLES, 0
    };

    struct iovec xcb_parts[4];
    xcb_shape_get_rectangles_cookie_t xcb_ret;
    xcb_shape_get_rectangles_request_t xcb_out;

    xcb_out.window = window;
    xcb_out.source_kind = source_kind;
    memset(xcb_out.pad0, 0, 3);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shape.c: xcb_shape_get_rectangles_rectangles */
xcb_rectangle_t *
xcb_shape_get_rectangles_rectangles (const xcb_shape_get_rectangles_reply_t *R)
{
    return (xcb_rectangle_t *) (R + 1);
}

/* shape.c: xcb_shape_get_rectangles_rectangles_length */
int
xcb_shape_get_rectangles_rectangles_length (const xcb_shape_get_rectangles_reply_t *R)
{
    return R->rectangles_len;
}

/* shape.c: xcb_shape_get_rectangles_reply */
xcb_shape_get_rectangles_reply_t *
xcb_shape_get_rectangles_reply (xcb_connection_t                   *c,
                                xcb_shape_get_rectangles_cookie_t   cookie  /**< */,
                                xcb_generic_error_t               **e)
{
    return (xcb_shape_get_rectangles_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* shape.c: xcb_shape_mask */
xcb_void_cookie_t
xcb_shape_mask (xcb_connection_t *c,
                xcb_shape_op_t    operation,
                xcb_shape_kind_t  destination_kind,
                xcb_window_t      destination_window,
                int16_t           x_offset,
                int16_t           y_offset,
                xcb_pixmap_t      source_bitmap)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shape_id, XCB_SHAPE_MASK, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_shape_mask_request_t xcb_out;

    xcb_out.operation = operation;
    xcb_out.destination_kind = destination_kind;
    memset(xcb_out.pad0, 0, 2);
    xcb_out.destination_window = destination_window;
    xcb_out.x_offset = x_offset;
    xcb_out.y_offset = y_offset;
    xcb_out.source_bitmap = source_bitmap;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shape.c: xcb_shape_query_version */
xcb_shape_query_version_cookie_t
xcb_shape_query_version (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shape_id, XCB_SHAPE_QUERY_VERSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_shape_query_version_cookie_t xcb_ret;
    xcb_shape_query_version_request_t xcb_out;


    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shape.c: xcb_shape_query_version_reply */
xcb_shape_query_version_reply_t *
xcb_shape_query_version_reply (xcb_connection_t                  *c,
                               xcb_shape_query_version_cookie_t   cookie  /**< */,
                               xcb_generic_error_t              **e)
{
    return (xcb_shape_query_version_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* shape.c: xcb_shape_rectangles */
xcb_void_cookie_t
xcb_shape_rectangles (xcb_connection_t      *c,
                      xcb_shape_op_t         operation,
                      xcb_shape_kind_t       destination_kind,
                      uint8_t                ordering,
                      xcb_window_t           destination_window,
                      int16_t                x_offset,
                      int16_t                y_offset,
                      uint32_t               rectangles_len,
                      const xcb_rectangle_t *rectangles)
{
    static const xcb_protocol_request_t xcb_req{
        4, &xcb_shape_id, XCB_SHAPE_RECTANGLES, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_shape_rectangles_request_t xcb_out;

    xcb_out.operation = operation;
    xcb_out.destination_kind = destination_kind;
    xcb_out.ordering = ordering;
    xcb_out.pad0 = 0;
    xcb_out.destination_window = destination_window;
    xcb_out.x_offset = x_offset;
    xcb_out.y_offset = y_offset;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_rectangle_t rectangles */
    xcb_parts[4].iov_base = (char *) rectangles;
    xcb_parts[4].iov_len = rectangles_len * sizeof(xcb_rectangle_t);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_attach_checked */
xcb_void_cookie_t
xcb_shm_attach_checked (xcb_connection_t *c,
                        xcb_shm_seg_t     shmseg,
                        uint32_t          shmid,
                        uint8_t           read_only)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_ATTACH, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_shm_attach_request_t xcb_out;

    xcb_out.shmseg = shmseg;
    xcb_out.shmid = shmid;
    xcb_out.read_only = read_only;
    memset(xcb_out.pad0, 0, 3);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_create_segment */
xcb_shm_create_segment_cookie_t
xcb_shm_create_segment (xcb_connection_t *c,
                        xcb_shm_seg_t     shmseg,
                        uint32_t          size,
                        uint8_t           read_only)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_CREATE_SEGMENT, 0
    };

    struct iovec xcb_parts[4];
    xcb_shm_create_segment_cookie_t xcb_ret;
    xcb_shm_create_segment_request_t xcb_out;

    xcb_out.shmseg = shmseg;
    xcb_out.size = size;
    xcb_out.read_only = read_only;
    memset(xcb_out.pad0, 0, 3);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED|XCB_REQUEST_REPLY_FDS, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_create_segment_reply */
xcb_shm_create_segment_reply_t *
xcb_shm_create_segment_reply (xcb_connection_t                 *c,
                              xcb_shm_create_segment_cookie_t   cookie  /**< */,
                              xcb_generic_error_t             **e)
{
    return (xcb_shm_create_segment_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* shm.c: xcb_shm_create_segment_reply_fds */
int *
xcb_shm_create_segment_reply_fds (xcb_connection_t                *c  /**< */,
                                  xcb_shm_create_segment_reply_t  *reply)
{
    return xcb_get_reply_fds(c, reply, sizeof(xcb_shm_create_segment_reply_t) + 4 * reply->length);
}

/* shm.c: xcb_shm_detach */
xcb_void_cookie_t
xcb_shm_detach (xcb_connection_t *c,
                xcb_shm_seg_t     shmseg)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_DETACH, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_shm_detach_request_t xcb_out;

    xcb_out.shmseg = shmseg;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_detach_checked */
xcb_void_cookie_t
xcb_shm_detach_checked (xcb_connection_t *c,
                        xcb_shm_seg_t     shmseg)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_DETACH, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_shm_detach_request_t xcb_out;

    xcb_out.shmseg = shmseg;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_put_image */
xcb_void_cookie_t
xcb_shm_put_image (xcb_connection_t *c,
                   xcb_drawable_t    drawable,
                   xcb_gcontext_t    gc,
                   uint16_t          total_width,
                   uint16_t          total_height,
                   uint16_t          src_x,
                   uint16_t          src_y,
                   uint16_t          src_width,
                   uint16_t          src_height,
                   int16_t           dst_x,
                   int16_t           dst_y,
                   uint8_t           depth,
                   uint8_t           format,
                   uint8_t           send_event,
                   xcb_shm_seg_t     shmseg,
                   uint32_t          offset)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_PUT_IMAGE, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_shm_put_image_request_t xcb_out;

    xcb_out.drawable = drawable;
    xcb_out.gc = gc;
    xcb_out.total_width = total_width;
    xcb_out.total_height = total_height;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;
    xcb_out.src_width = src_width;
    xcb_out.src_height = src_height;
    xcb_out.dst_x = dst_x;
    xcb_out.dst_y = dst_y;
    xcb_out.depth = depth;
    xcb_out.format = format;
    xcb_out.send_event = send_event;
    xcb_out.pad0 = 0;
    xcb_out.shmseg = shmseg;
    xcb_out.offset = offset;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_query_version */
xcb_shm_query_version_cookie_t
xcb_shm_query_version (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_shm_id, XCB_SHM_QUERY_VERSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_shm_query_version_cookie_t xcb_ret;
    xcb_shm_query_version_request_t xcb_out;


    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* shm.c: xcb_shm_query_version_reply */
xcb_shm_query_version_reply_t *
xcb_shm_query_version_reply (xcb_connection_t                *c,
                             xcb_shm_query_version_cookie_t   cookie  /**< */,
                             xcb_generic_error_t            **e)
{
    return (xcb_shm_query_version_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* sync.c: xcb_sync_create_counter */
xcb_void_cookie_t
xcb_sync_create_counter (xcb_connection_t   *c,
                         xcb_sync_counter_t  id,
                         xcb_sync_int64_t    initial_value)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_sync_id, XCB_SYNC_CREATE_COUNTER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_sync_create_counter_request_t xcb_out;

    xcb_out.id = id;
    xcb_out.initial_value = initial_value;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* sync.c: xcb_sync_destroy_counter */
xcb_void_cookie_t
xcb_sync_destroy_counter (xcb_connection_t   *c,
                          xcb_sync_counter_t  counter)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_sync_id, XCB_SYNC_DESTROY_COUNTER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_sync_destroy_counter_request_t xcb_out;

    xcb_out.counter = counter;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* sync.c: xcb_sync_set_counter */
xcb_void_cookie_t
xcb_sync_set_counter (xcb_connection_t   *c,
                      xcb_sync_counter_t  counter,
                      xcb_sync_int64_t    value)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_sync_id, XCB_SYNC_SET_COUNTER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_sync_set_counter_request_t xcb_out;

    xcb_out.counter = counter;
    xcb_out.value = value;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_translate_coordinates */
xcb_translate_coordinates_cookie_t
xcb_translate_coordinates (xcb_connection_t *c,
                           xcb_window_t      src_window,
                           xcb_window_t      dst_window,
                           int16_t           src_x,
                           int16_t           src_y)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_TRANSLATE_COORDINATES, 0
    };

    struct iovec xcb_parts[4];
    xcb_translate_coordinates_cookie_t xcb_ret;
    xcb_translate_coordinates_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.src_window = src_window;
    xcb_out.dst_window = dst_window;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_translate_coordinates_reply */
xcb_translate_coordinates_reply_t *
xcb_translate_coordinates_reply (xcb_connection_t                    *c,
                                 xcb_translate_coordinates_cookie_t   cookie  /**< */,
                                 xcb_generic_error_t                **e)
{
    return (xcb_translate_coordinates_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xproto.c: xcb_translate_coordinates_unchecked */
xcb_translate_coordinates_cookie_t
xcb_translate_coordinates_unchecked (xcb_connection_t *c,
                                     xcb_window_t      src_window,
                                     xcb_window_t      dst_window,
                                     int16_t           src_x,
                                     int16_t           src_y)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_TRANSLATE_COORDINATES, 0
    };

    struct iovec xcb_parts[4];
    xcb_translate_coordinates_cookie_t xcb_ret;
    xcb_translate_coordinates_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.src_window = src_window;
    xcb_out.dst_window = dst_window;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_ungrab_keyboard */
xcb_void_cookie_t
xcb_ungrab_keyboard (xcb_connection_t *c,
                     xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_UNGRAB_KEYBOARD, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_ungrab_keyboard_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_ungrab_pointer */
xcb_void_cookie_t
xcb_ungrab_pointer (xcb_connection_t *c,
                    xcb_timestamp_t   time)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_UNGRAB_POINTER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_ungrab_pointer_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.time = time;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_ungrab_server */
xcb_void_cookie_t
xcb_ungrab_server (xcb_connection_t *c)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_UNGRAB_SERVER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_ungrab_server_request_t xcb_out;

    xcb_out.pad0 = 0;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_unmap_window */
xcb_void_cookie_t
xcb_unmap_window (xcb_connection_t *c,
                  xcb_window_t      window)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_UNMAP_WINDOW, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_unmap_window_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.window = window;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xproto.c: xcb_warp_pointer */
xcb_void_cookie_t
xcb_warp_pointer (xcb_connection_t *c,
                  xcb_window_t      src_window,
                  xcb_window_t      dst_window,
                  int16_t           src_x,
                  int16_t           src_y,
                  uint16_t          src_width,
                  uint16_t          src_height,
                  int16_t           dst_x,
                  int16_t           dst_y)
{
    static const xcb_protocol_request_t xcb_req{
        2, 0, XCB_WARP_POINTER, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_warp_pointer_request_t xcb_out;

    xcb_out.pad0 = 0;
    xcb_out.src_window = src_window;
    xcb_out.dst_window = dst_window;
    xcb_out.src_x = src_x;
    xcb_out.src_y = src_y;
    xcb_out.src_width = src_width;
    xcb_out.src_height = src_height;
    xcb_out.dst_x = dst_x;
    xcb_out.dst_y = dst_y;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_create_region */
xcb_void_cookie_t
xcb_xfixes_create_region (xcb_connection_t      *c,
                          xcb_xfixes_region_t    region,
                          uint32_t               rectangles_len,
                          const xcb_rectangle_t *rectangles)
{
    static const xcb_protocol_request_t xcb_req{
        4, &xcb_xfixes_id, XCB_XFIXES_CREATE_REGION, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_xfixes_create_region_request_t xcb_out;

    xcb_out.region = region;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_rectangle_t rectangles */
    xcb_parts[4].iov_base = (char *) rectangles;
    xcb_parts[4].iov_len = rectangles_len * sizeof(xcb_rectangle_t);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_destroy_region */
xcb_void_cookie_t
xcb_xfixes_destroy_region (xcb_connection_t    *c,
                           xcb_xfixes_region_t  region)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xfixes_id, XCB_XFIXES_DESTROY_REGION, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_xfixes_destroy_region_request_t xcb_out;

    xcb_out.region = region;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_query_version */
xcb_xfixes_query_version_cookie_t
xcb_xfixes_query_version (xcb_connection_t *c,
                          uint32_t          client_major_version,
                          uint32_t          client_minor_version)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xfixes_id, XCB_XFIXES_QUERY_VERSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_xfixes_query_version_cookie_t xcb_ret;
    xcb_xfixes_query_version_request_t xcb_out;

    xcb_out.client_major_version = client_major_version;
    xcb_out.client_minor_version = client_minor_version;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_query_version_reply */
xcb_xfixes_query_version_reply_t *
xcb_xfixes_query_version_reply (xcb_connection_t                   *c,
                                xcb_xfixes_query_version_cookie_t   cookie  /**< */,
                                xcb_generic_error_t               **e)
{
    return (xcb_xfixes_query_version_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xfixes.c: xcb_xfixes_select_selection_input_checked */
xcb_void_cookie_t
xcb_xfixes_select_selection_input_checked (xcb_connection_t *c,
                                           xcb_window_t      window,
                                           xcb_atom_t        selection,
                                           uint32_t          event_mask)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xfixes_id, XCB_XFIXES_SELECT_SELECTION_INPUT, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_xfixes_select_selection_input_request_t xcb_out;

    xcb_out.window = window;
    xcb_out.selection = selection;
    xcb_out.event_mask = event_mask;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_set_cursor_name */
xcb_void_cookie_t
xcb_xfixes_set_cursor_name (xcb_connection_t *c,
                            xcb_cursor_t      cursor,
                            uint16_t          nbytes,
                            const char       *name)
{
    static const xcb_protocol_request_t xcb_req{
        4, &xcb_xfixes_id, XCB_XFIXES_SET_CURSOR_NAME, 1
    };

    struct iovec xcb_parts[6];
    xcb_void_cookie_t xcb_ret;
    xcb_xfixes_set_cursor_name_request_t xcb_out;

    xcb_out.cursor = cursor;
    xcb_out.nbytes = nbytes;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* char name */
    xcb_parts[4].iov_base = (char *) name;
    xcb_parts[4].iov_len = nbytes * sizeof(char);
    xcb_parts[5].iov_base = 0;
    xcb_parts[5].iov_len = -xcb_parts[4].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, 0, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xfixes.c: xcb_xfixes_set_window_shape_region_checked */
xcb_void_cookie_t
xcb_xfixes_set_window_shape_region_checked (xcb_connection_t    *c,
                                            xcb_window_t         dest,
                                            xcb_shape_kind_t     dest_kind,
                                            int16_t              x_offset,
                                            int16_t              y_offset,
                                            xcb_xfixes_region_t  region)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xfixes_id, XCB_XFIXES_SET_WINDOW_SHAPE_REGION, 1
    };

    struct iovec xcb_parts[4];
    xcb_void_cookie_t xcb_ret;
    xcb_xfixes_set_window_shape_region_request_t xcb_out;

    xcb_out.dest = dest;
    xcb_out.dest_kind = dest_kind;
    memset(xcb_out.pad0, 0, 3);
    xcb_out.x_offset = x_offset;
    xcb_out.y_offset = y_offset;
    xcb_out.region = region;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xkb.c: xcb_xkb_get_map */
xcb_xkb_get_map_cookie_t
xcb_xkb_get_map (xcb_connection_t      *c,
                 xcb_xkb_device_spec_t  deviceSpec,
                 uint16_t               full,
                 uint16_t               partial,
                 uint8_t                firstType,
                 uint8_t                nTypes,
                 xcb_keycode_t          firstKeySym,
                 uint8_t                nKeySyms,
                 xcb_keycode_t          firstKeyAction,
                 uint8_t                nKeyActions,
                 xcb_keycode_t          firstKeyBehavior,
                 uint8_t                nKeyBehaviors,
                 uint16_t               virtualMods,
                 xcb_keycode_t          firstKeyExplicit,
                 uint8_t                nKeyExplicit,
                 xcb_keycode_t          firstModMapKey,
                 uint8_t                nModMapKeys,
                 xcb_keycode_t          firstVModMapKey,
                 uint8_t                nVModMapKeys)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xkb_id, XCB_XKB_GET_MAP, 0
    };

    struct iovec xcb_parts[4];
    xcb_xkb_get_map_cookie_t xcb_ret;
    xcb_xkb_get_map_request_t xcb_out;

    xcb_out.deviceSpec = deviceSpec;
    xcb_out.full = full;
    xcb_out.partial = partial;
    xcb_out.firstType = firstType;
    xcb_out.nTypes = nTypes;
    xcb_out.firstKeySym = firstKeySym;
    xcb_out.nKeySyms = nKeySyms;
    xcb_out.firstKeyAction = firstKeyAction;
    xcb_out.nKeyActions = nKeyActions;
    xcb_out.firstKeyBehavior = firstKeyBehavior;
    xcb_out.nKeyBehaviors = nKeyBehaviors;
    xcb_out.virtualMods = virtualMods;
    xcb_out.firstKeyExplicit = firstKeyExplicit;
    xcb_out.nKeyExplicit = nKeyExplicit;
    xcb_out.firstModMapKey = firstModMapKey;
    xcb_out.nModMapKeys = nModMapKeys;
    xcb_out.firstVModMapKey = firstVModMapKey;
    xcb_out.nVModMapKeys = nVModMapKeys;
    memset(xcb_out.pad0, 0, 2);

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xkb.c: xcb_xkb_get_map_map */
void *
xcb_xkb_get_map_map (const xcb_xkb_get_map_reply_t *R)
{
    return (void *) (R + 1);
}

/* xkb.c: xcb_xkb_get_map_map_unpack */
int
xcb_xkb_get_map_map_unpack (const void             *_buffer,
                            uint8_t                 nTypes,
                            uint8_t                 nKeySyms,
                            uint8_t                 nKeyActions,
                            uint16_t                totalActions,
                            uint8_t                 totalKeyBehaviors,
                            uint16_t                virtualMods,
                            uint8_t                 totalKeyExplicit,
                            uint8_t                 totalModMapKeys,
                            uint8_t                 totalVModMapKeys,
                            uint16_t                present,
                            xcb_xkb_get_map_map_t  *_aux)
{
    char *xcb_tmp = (char *)_buffer;
    unsigned int xcb_buffer_len = 0;
    unsigned int xcb_block_len = 0;
    unsigned int xcb_pad = 0;
    unsigned int xcb_align_to = 0;
    unsigned int xcb_padding_offset = 0;

    unsigned int i;
    unsigned int xcb_tmp_len;

    if(present & XCB_XKB_MAP_PART_KEY_TYPES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* types_rtrn */
        _aux->types_rtrn = (xcb_xkb_key_type_t *)xcb_tmp;
        for(i=0; i<nTypes; i++) {
            xcb_tmp_len = xcb_xkb_key_type_sizeof(xcb_tmp);
            xcb_block_len += xcb_tmp_len;
            xcb_tmp += xcb_tmp_len;
        }
        xcb_align_to = ALIGNOF(xcb_xkb_key_type_t);
    }
    if(present & XCB_XKB_MAP_PART_KEY_SYMS) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* syms_rtrn */
        _aux->syms_rtrn = (xcb_xkb_key_sym_map_t *)xcb_tmp;
        for(i=0; i<nKeySyms; i++) {
            xcb_tmp_len = xcb_xkb_key_sym_map_sizeof(xcb_tmp);
            xcb_block_len += xcb_tmp_len;
            xcb_tmp += xcb_tmp_len;
        }
        xcb_align_to = ALIGNOF(xcb_xkb_key_sym_map_t);
    }
    if(present & XCB_XKB_MAP_PART_KEY_ACTIONS) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* acts_rtrn_count */
        _aux->acts_rtrn_count = (uint8_t *)xcb_tmp;
        xcb_block_len += nKeyActions * sizeof(xcb_keycode_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(uint8_t);
        xcb_align_to = 4;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* acts_rtrn_acts */
        _aux->acts_rtrn_acts = (xcb_xkb_action_t *)xcb_tmp;
        xcb_block_len += totalActions * sizeof(xcb_xkb_action_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_action_t);
    }
    if(present & XCB_XKB_MAP_PART_KEY_BEHAVIORS) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* behaviors_rtrn */
        _aux->behaviors_rtrn = (xcb_xkb_set_behavior_t *)xcb_tmp;
        xcb_block_len += totalKeyBehaviors * sizeof(xcb_xkb_set_behavior_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_set_behavior_t);
    }
    if(present & XCB_XKB_MAP_PART_VIRTUAL_MODS) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* vmods_rtrn */
        _aux->vmods_rtrn = (uint8_t *)xcb_tmp;
        xcb_block_len += xcb_popcount(virtualMods) * sizeof(xcb_keycode_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(uint8_t);
        xcb_align_to = 4;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
    }
    if(present & XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* explicit_rtrn */
        _aux->explicit_rtrn = (xcb_xkb_set_explicit_t *)xcb_tmp;
        xcb_block_len += totalKeyExplicit * sizeof(xcb_xkb_set_explicit_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_set_explicit_t);
        xcb_align_to = 4;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
    }
    if(present & XCB_XKB_MAP_PART_MODIFIER_MAP) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* modmap_rtrn */
        _aux->modmap_rtrn = (xcb_xkb_key_mod_map_t *)xcb_tmp;
        xcb_block_len += totalModMapKeys * sizeof(xcb_xkb_key_mod_map_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_key_mod_map_t);
        xcb_align_to = 4;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
    }
    if(present & XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* vmodmap_rtrn */
        _aux->vmodmap_rtrn = (xcb_xkb_key_v_mod_map_t *)xcb_tmp;
        xcb_block_len += totalVModMapKeys * sizeof(xcb_xkb_key_v_mod_map_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_key_v_mod_map_t);
    }
    /* insert padding */
    xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;
    xcb_padding_offset = 0;

    return xcb_buffer_len;
}

/* xkb.c: xcb_xkb_get_map_reply */
xcb_xkb_get_map_reply_t *
xcb_xkb_get_map_reply (xcb_connection_t          *c,
                       xcb_xkb_get_map_cookie_t   cookie  /**< */,
                       xcb_generic_error_t      **e)
{
    return (xcb_xkb_get_map_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xkb.c: xcb_xkb_get_names */
xcb_xkb_get_names_cookie_t
xcb_xkb_get_names (xcb_connection_t      *c,
                   xcb_xkb_device_spec_t  deviceSpec,
                   uint32_t               which)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xkb_id, XCB_XKB_GET_NAMES, 0
    };

    struct iovec xcb_parts[4];
    xcb_xkb_get_names_cookie_t xcb_ret;
    xcb_xkb_get_names_request_t xcb_out;

    xcb_out.deviceSpec = deviceSpec;
    memset(xcb_out.pad0, 0, 2);
    xcb_out.which = which;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xkb.c: xcb_xkb_get_names_reply */
xcb_xkb_get_names_reply_t *
xcb_xkb_get_names_reply (xcb_connection_t            *c,
                         xcb_xkb_get_names_cookie_t   cookie  /**< */,
                         xcb_generic_error_t        **e)
{
    return (xcb_xkb_get_names_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}

/* xkb.c: xcb_xkb_get_names_value_list */
void *
xcb_xkb_get_names_value_list (const xcb_xkb_get_names_reply_t *R)
{
    return (void *) (R + 1);
}

/* xkb.c: xcb_xkb_get_names_value_list_unpack */
int
xcb_xkb_get_names_value_list_unpack (const void                      *_buffer,
                                     uint8_t                          nTypes,
                                     uint32_t                         indicators,
                                     uint16_t                         virtualMods,
                                     uint8_t                          groupNames,
                                     uint8_t                          nKeys,
                                     uint8_t                          nKeyAliases,
                                     uint8_t                          nRadioGroups,
                                     uint32_t                         which,
                                     xcb_xkb_get_names_value_list_t  *_aux)
{
    char *xcb_tmp = (char *)_buffer;
    unsigned int xcb_buffer_len = 0;
    unsigned int xcb_block_len = 0;
    unsigned int xcb_pad = 0;
    unsigned int xcb_align_to = 0;
    unsigned int xcb_padding_offset = 0;

    int xcb_pre_tmp_1; /* sumof length */
    int xcb_pre_tmp_2; /* sumof loop counter */
    int64_t xcb_pre_tmp_3; /* sumof sum */
    const uint8_t* xcb_pre_tmp_4; /* sumof list ptr */

    if(which & XCB_XKB_NAME_DETAIL_KEYCODES) {
        /* xcb_xkb_get_names_value_list_t.keycodesName */
        _aux->keycodesName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_GEOMETRY) {
        /* xcb_xkb_get_names_value_list_t.geometryName */
        _aux->geometryName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_SYMBOLS) {
        /* xcb_xkb_get_names_value_list_t.symbolsName */
        _aux->symbolsName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_PHYS_SYMBOLS) {
        /* xcb_xkb_get_names_value_list_t.physSymbolsName */
        _aux->physSymbolsName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_TYPES) {
        /* xcb_xkb_get_names_value_list_t.typesName */
        _aux->typesName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_COMPAT) {
        /* xcb_xkb_get_names_value_list_t.compatName */
        _aux->compatName = *(xcb_atom_t *)xcb_tmp;
        xcb_block_len += sizeof(xcb_atom_t);
        xcb_tmp += sizeof(xcb_atom_t);
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_KEY_TYPE_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* typeNames */
        _aux->typeNames = (xcb_atom_t *)xcb_tmp;
        xcb_block_len += nTypes * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_KT_LEVEL_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* nLevelsPerType */
        _aux->nLevelsPerType = (uint8_t *)xcb_tmp;
        xcb_block_len += nTypes * sizeof(uint8_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(uint8_t);
        xcb_align_to = 4;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* ktLevelNames */
        _aux->ktLevelNames = (xcb_atom_t *)xcb_tmp;
        /* sumof start */
        xcb_pre_tmp_1 = nTypes;
        xcb_pre_tmp_3 = 0;
        xcb_pre_tmp_4 = _aux->nLevelsPerType;
        for (xcb_pre_tmp_2 = 0; xcb_pre_tmp_2 < xcb_pre_tmp_1; xcb_pre_tmp_2++) {
            xcb_pre_tmp_3 += *xcb_pre_tmp_4;
            xcb_pre_tmp_4++;
        }
        /* sumof end. Result is in xcb_pre_tmp_3 */
        xcb_block_len += xcb_pre_tmp_3 * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_INDICATOR_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* indicatorNames */
        _aux->indicatorNames = (xcb_atom_t *)xcb_tmp;
        xcb_block_len += xcb_popcount(indicators) * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_VIRTUAL_MOD_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* virtualModNames */
        _aux->virtualModNames = (xcb_atom_t *)xcb_tmp;
        xcb_block_len += xcb_popcount(virtualMods) * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_GROUP_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* groups */
        _aux->groups = (xcb_atom_t *)xcb_tmp;
        xcb_block_len += xcb_popcount(groupNames) * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_KEY_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* keyNames */
        _aux->keyNames = (xcb_xkb_key_name_t *)xcb_tmp;
        xcb_block_len += nKeys * sizeof(xcb_xkb_key_name_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_key_name_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_KEY_ALIASES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* keyAliases */
        _aux->keyAliases = (xcb_xkb_key_alias_t *)xcb_tmp;
        xcb_block_len += nKeyAliases * sizeof(xcb_xkb_key_alias_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_xkb_key_alias_t);
    }
    if(which & XCB_XKB_NAME_DETAIL_RG_NAMES) {
        /* insert padding */
        xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
        xcb_buffer_len += xcb_block_len + xcb_pad;
        if (0 != xcb_pad) {
            xcb_tmp += xcb_pad;
            xcb_pad = 0;
        }
        xcb_block_len = 0;
        xcb_padding_offset = 0;
        /* radioGroupNames */
        _aux->radioGroupNames = (xcb_atom_t *)xcb_tmp;
        xcb_block_len += nRadioGroups * sizeof(xcb_atom_t);
        xcb_tmp += xcb_block_len;
        xcb_align_to = ALIGNOF(xcb_atom_t);
    }
    /* insert padding */
    xcb_pad = -(xcb_block_len + xcb_padding_offset) & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;
    xcb_padding_offset = 0;

    return xcb_buffer_len;
}

/* xkb.c: xcb_xkb_select_events_checked */
xcb_void_cookie_t
xcb_xkb_select_events_checked (xcb_connection_t      *c,
                               xcb_xkb_device_spec_t  deviceSpec,
                               uint16_t               affectWhich,
                               uint16_t               clear,
                               uint16_t               selectAll,
                               uint16_t               affectMap,
                               uint16_t               map,
                               const void            *details)
{
    static const xcb_protocol_request_t xcb_req{
        3, &xcb_xkb_id, XCB_XKB_SELECT_EVENTS, 1
    };

    struct iovec xcb_parts[5];
    xcb_void_cookie_t xcb_ret;
    xcb_xkb_select_events_request_t xcb_out;

    xcb_out.deviceSpec = deviceSpec;
    xcb_out.affectWhich = affectWhich;
    xcb_out.clear = clear;
    xcb_out.selectAll = selectAll;
    xcb_out.affectMap = affectMap;
    xcb_out.map = map;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;
    /* xcb_xkb_select_events_details_t details */
    xcb_parts[4].iov_base = (char *) details;
    xcb_parts[4].iov_len =
      xcb_xkb_select_events_details_sizeof (details, affectWhich, clear, selectAll);

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xkb.c: xcb_xkb_use_extension */
xcb_xkb_use_extension_cookie_t
xcb_xkb_use_extension (xcb_connection_t *c,
                       uint16_t          wantedMajor,
                       uint16_t          wantedMinor)
{
    static const xcb_protocol_request_t xcb_req{
        2, &xcb_xkb_id, XCB_XKB_USE_EXTENSION, 0
    };

    struct iovec xcb_parts[4];
    xcb_xkb_use_extension_cookie_t xcb_ret;
    xcb_xkb_use_extension_request_t xcb_out;

    xcb_out.wantedMajor = wantedMajor;
    xcb_out.wantedMinor = wantedMinor;

    xcb_parts[2].iov_base = (char *) &xcb_out;
    xcb_parts[2].iov_len = sizeof(xcb_out);
    xcb_parts[3].iov_base = 0;
    xcb_parts[3].iov_len = -xcb_parts[2].iov_len & 3;

    xcb_ret.sequence = xcb_send_request(c, XCB_REQUEST_CHECKED, xcb_parts + 2, &xcb_req);
    return xcb_ret;
}

/* xkb.c: xcb_xkb_use_extension_reply */
xcb_xkb_use_extension_reply_t *
xcb_xkb_use_extension_reply (xcb_connection_t                *c,
                             xcb_xkb_use_extension_cookie_t   cookie  /**< */,
                             xcb_generic_error_t            **e)
{
    return (xcb_xkb_use_extension_reply_t *) xcb_wait_for_reply(c, cookie.sequence, e);
}
/* xproto.c: xcb_change_gc_value_list_sizeof */
int
xcb_change_gc_value_list_sizeof (const void  *_buffer,
                                 uint32_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

/* xproto.c: xcb_change_window_attributes_value_list_sizeof */
int
xcb_change_window_attributes_value_list_sizeof (const void  *_buffer,
                                                uint32_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

/* xproto.c: xcb_configure_window_value_list_sizeof */
int
xcb_configure_window_value_list_sizeof (const void  *_buffer,
                                        uint16_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

/* xproto.c: xcb_create_gc_value_list_sizeof */
int
xcb_create_gc_value_list_sizeof (const void  *_buffer,
                                 uint32_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

/* xproto.c: xcb_create_window_value_list_sizeof */
int
xcb_create_window_value_list_sizeof (const void  *_buffer,
                                     uint32_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

static xcb_generic_iterator_t
xmin_randr_get_output_info_modes_end(
    const xcb_randr_get_output_info_reply_t *reply)
{
    xcb_generic_iterator_t crtcs{};
    crtcs.data = const_cast<xcb_randr_crtc_t *>(
        reinterpret_cast<const xcb_randr_crtc_t *>(reply + 1)) +
        reply->num_crtcs;
    crtcs.index = static_cast<int>(
        static_cast<char *>(crtcs.data) -
        reinterpret_cast<const char *>(reply));
    xcb_generic_iterator_t result{};
    result.data = reinterpret_cast<xcb_randr_mode_t *>(
        static_cast<char *>(crtcs.data) +
        XCB_TYPE_PAD(xcb_randr_mode_t, crtcs.index)) + reply->num_modes;
    result.index = static_cast<int>(
        static_cast<char *>(result.data) -
        reinterpret_cast<const char *>(reply));
    return result;
}

/* randr.c: xcb_randr_get_output_info_clones_end */
xcb_generic_iterator_t
xcb_randr_get_output_info_clones_end (const xcb_randr_get_output_info_reply_t *R)
{
    xcb_generic_iterator_t i;
    xcb_generic_iterator_t prev = xmin_randr_get_output_info_modes_end(R);
    i.data = ((xcb_randr_output_t *) ((char*) prev.data + XCB_TYPE_PAD(xcb_randr_output_t, prev.index))) + (R->num_clones);
    i.rem = 0;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_get_screen_resources_crtcs_end */
xcb_generic_iterator_t
xcb_randr_get_screen_resources_crtcs_end (const xcb_randr_get_screen_resources_reply_t *R)
{
    xcb_generic_iterator_t i;
    i.data = ((xcb_randr_crtc_t *) (R + 1)) + (R->num_crtcs);
    i.rem = 0;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_get_screen_resources_current_crtcs_end */
xcb_generic_iterator_t
xcb_randr_get_screen_resources_current_crtcs_end (const xcb_randr_get_screen_resources_current_reply_t *R)
{
    xcb_generic_iterator_t i;
    i.data = ((xcb_randr_crtc_t *) (R + 1)) + (R->num_crtcs);
    i.rem = 0;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_get_screen_resources_current_outputs_end */
xcb_generic_iterator_t
xcb_randr_get_screen_resources_current_outputs_end (const xcb_randr_get_screen_resources_current_reply_t *R)
{
    xcb_generic_iterator_t i;
    xcb_generic_iterator_t prev = xcb_randr_get_screen_resources_current_crtcs_end(R);
    i.data = ((xcb_randr_output_t *) ((char*) prev.data + XCB_TYPE_PAD(xcb_randr_output_t, prev.index))) + (R->num_outputs);
    i.rem = 0;
    i.index = (char *) i.data - (char *) R;
    return i;
}

/* randr.c: xcb_randr_monitor_info_sizeof */
int
xcb_randr_monitor_info_sizeof (const void  *_buffer)
{
    char *xcb_tmp = (char *)_buffer;
    const xcb_randr_monitor_info_t *_aux = (xcb_randr_monitor_info_t *)_buffer;
    unsigned int xcb_buffer_len = 0;
    unsigned int xcb_block_len = 0;
    unsigned int xcb_pad = 0;
    unsigned int xcb_align_to = 0;


    xcb_block_len += sizeof(xcb_randr_monitor_info_t);
    xcb_tmp += xcb_block_len;
    xcb_buffer_len += xcb_block_len;
    xcb_block_len = 0;
    /* outputs */
    xcb_block_len += _aux->nOutput * sizeof(xcb_randr_output_t);
    xcb_tmp += xcb_block_len;
    xcb_align_to = ALIGNOF(xcb_randr_output_t);
    /* insert padding */
    xcb_pad = -xcb_block_len & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;

    return xcb_buffer_len;
}

/* render.c: xcb_render_create_picture_value_list_sizeof */
int
xcb_render_create_picture_value_list_sizeof (const void  *_buffer,
                                             uint32_t     value_mask)
{
    (void) _buffer;
    return xcb_popcount(value_mask) * (int) sizeof(uint32_t);
}

/* xkb.c: xcb_xkb_key_sym_map_sizeof */
int
xcb_xkb_key_sym_map_sizeof (const void  *_buffer)
{
    char *xcb_tmp = (char *)_buffer;
    const xcb_xkb_key_sym_map_t *_aux = (xcb_xkb_key_sym_map_t *)_buffer;
    unsigned int xcb_buffer_len = 0;
    unsigned int xcb_block_len = 0;
    unsigned int xcb_pad = 0;
    unsigned int xcb_align_to = 0;


    xcb_block_len += sizeof(xcb_xkb_key_sym_map_t);
    xcb_tmp += xcb_block_len;
    xcb_buffer_len += xcb_block_len;
    xcb_block_len = 0;
    /* syms */
    xcb_block_len += _aux->nSyms * sizeof(xcb_keysym_t);
    xcb_tmp += xcb_block_len;
    xcb_align_to = ALIGNOF(xcb_keysym_t);
    /* insert padding */
    xcb_pad = -xcb_block_len & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;

    return xcb_buffer_len;
}

/* xkb.c: xcb_xkb_key_type_sizeof */
int
xcb_xkb_key_type_sizeof (const void  *_buffer)
{
    char *xcb_tmp = (char *)_buffer;
    const xcb_xkb_key_type_t *_aux = (xcb_xkb_key_type_t *)_buffer;
    unsigned int xcb_buffer_len = 0;
    unsigned int xcb_block_len = 0;
    unsigned int xcb_pad = 0;
    unsigned int xcb_align_to = 0;


    xcb_block_len += sizeof(xcb_xkb_key_type_t);
    xcb_tmp += xcb_block_len;
    xcb_buffer_len += xcb_block_len;
    xcb_block_len = 0;
    /* map */
    xcb_block_len += _aux->nMapEntries * sizeof(xcb_xkb_kt_map_entry_t);
    xcb_tmp += xcb_block_len;
    xcb_align_to = ALIGNOF(xcb_xkb_kt_map_entry_t);
    /* insert padding */
    xcb_pad = -xcb_block_len & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;
    /* preserve */
    xcb_block_len += (_aux->hasPreserve * _aux->nMapEntries) * sizeof(xcb_xkb_mod_def_t);
    xcb_tmp += xcb_block_len;
    xcb_align_to = ALIGNOF(xcb_xkb_mod_def_t);
    /* insert padding */
    xcb_pad = -xcb_block_len & (xcb_align_to - 1);
    xcb_buffer_len += xcb_block_len + xcb_pad;
    if (0 != xcb_pad) {
        xcb_tmp += xcb_pad;
        xcb_pad = 0;
    }
    xcb_block_len = 0;

    return xcb_buffer_len;
}

/* xkb.c: xcb_xkb_select_events_details_sizeof */
int
xcb_xkb_select_events_details_sizeof (const void  *_buffer,
                                      uint16_t     affectWhich,
                                      uint16_t     clear,
                                      uint16_t     selectAll)
{
    (void) _buffer;
    const uint16_t selected = affectWhich &
        static_cast<uint16_t>(~clear) & static_cast<uint16_t>(~selectAll);
    int size = 0;
    if (selected & XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY) size += 4;
    if (selected & XCB_XKB_EVENT_TYPE_STATE_NOTIFY) size += 4;
    if (selected & XCB_XKB_EVENT_TYPE_CONTROLS_NOTIFY) size += 8;
    if (selected & XCB_XKB_EVENT_TYPE_INDICATOR_STATE_NOTIFY) size += 8;
    if (selected & XCB_XKB_EVENT_TYPE_INDICATOR_MAP_NOTIFY) size += 8;
    if (selected & XCB_XKB_EVENT_TYPE_NAMES_NOTIFY) size += 4;
    if (selected & XCB_XKB_EVENT_TYPE_COMPAT_MAP_NOTIFY) size += 2;
    if (selected & XCB_XKB_EVENT_TYPE_BELL_NOTIFY) size += 2;
    if (selected & XCB_XKB_EVENT_TYPE_ACTION_MESSAGE) size += 2;
    if (selected & XCB_XKB_EVENT_TYPE_ACCESS_X_NOTIFY) size += 4;
    if (selected & XCB_XKB_EVENT_TYPE_EXTENSION_DEVICE_NOTIFY) size += 4;
    return size;
}
