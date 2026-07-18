/*
 * Additional core protocol encoders used by the focused FLTK/Tk facade.
 * The layouts are the public xcb-proto request structures; no libxcb or
 * libX11 implementation code is incorporated here.
 */
#include "xcb_protocol.hpp"

#include <xcb/xproto.h>

#include <cstddef>
#include <cstdint>

namespace {

template <typename Request, typename Element>
xcb_void_cookie_t send_list_request(
    xcb_connection_t *connection, std::uint8_t opcode,
    const Request &request, std::uint32_t count, const Element *elements)
{
    return xmin::client::x11::send_request<xcb_void_cookie_t>(
        connection, nullptr, opcode, true, 0, request,
        {{elements, static_cast<std::size_t>(count) * sizeof(Element)}});
}

} // namespace

extern "C" {

xcb_void_cookie_t xcb_set_dashes(
    xcb_connection_t *connection, xcb_gcontext_t gc,
    std::uint16_t dash_offset, std::uint16_t dashes_len,
    const std::uint8_t *dashes)
{
    xcb_set_dashes_request_t request{};
    request.gc = gc;
    request.dash_offset = dash_offset;
    request.dashes_len = dashes_len;
    return send_list_request(
        connection, XCB_SET_DASHES, request, dashes_len, dashes);
}

xcb_void_cookie_t xcb_poly_point(
    xcb_connection_t *connection, std::uint8_t coordinate_mode,
    xcb_drawable_t drawable, xcb_gcontext_t gc,
    std::uint32_t points_len, const xcb_point_t *points)
{
    xcb_poly_point_request_t request{};
    request.coordinate_mode = coordinate_mode;
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(
        connection, XCB_POLY_POINT, request, points_len, points);
}

xcb_void_cookie_t xcb_poly_line(
    xcb_connection_t *connection, std::uint8_t coordinate_mode,
    xcb_drawable_t drawable, xcb_gcontext_t gc,
    std::uint32_t points_len, const xcb_point_t *points)
{
    xcb_poly_line_request_t request{};
    request.coordinate_mode = coordinate_mode;
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(
        connection, XCB_POLY_LINE, request, points_len, points);
}

xcb_void_cookie_t xcb_poly_rectangle(
    xcb_connection_t *connection, xcb_drawable_t drawable,
    xcb_gcontext_t gc, std::uint32_t rectangles_len,
    const xcb_rectangle_t *rectangles)
{
    xcb_poly_rectangle_request_t request{};
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(
        connection, XCB_POLY_RECTANGLE, request, rectangles_len, rectangles);
}

xcb_void_cookie_t xcb_poly_arc(
    xcb_connection_t *connection, xcb_drawable_t drawable,
    xcb_gcontext_t gc, std::uint32_t arcs_len, const xcb_arc_t *arcs)
{
    xcb_poly_arc_request_t request{};
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(connection, XCB_POLY_ARC, request, arcs_len, arcs);
}

xcb_void_cookie_t xcb_fill_poly(
    xcb_connection_t *connection, xcb_drawable_t drawable,
    xcb_gcontext_t gc, std::uint8_t shape, std::uint8_t coordinate_mode,
    std::uint32_t points_len, const xcb_point_t *points)
{
    xcb_fill_poly_request_t request{};
    request.drawable = drawable;
    request.gc = gc;
    request.shape = shape;
    request.coordinate_mode = coordinate_mode;
    return send_list_request(
        connection, XCB_FILL_POLY, request, points_len, points);
}

xcb_void_cookie_t xcb_poly_fill_rectangle(
    xcb_connection_t *connection, xcb_drawable_t drawable,
    xcb_gcontext_t gc, std::uint32_t rectangles_len,
    const xcb_rectangle_t *rectangles)
{
    xcb_poly_fill_rectangle_request_t request{};
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(
        connection, XCB_POLY_FILL_RECTANGLE, request, rectangles_len,
        rectangles);
}

xcb_void_cookie_t xcb_poly_fill_arc(
    xcb_connection_t *connection, xcb_drawable_t drawable,
    xcb_gcontext_t gc, std::uint32_t arcs_len, const xcb_arc_t *arcs)
{
    xcb_poly_fill_arc_request_t request{};
    request.drawable = drawable;
    request.gc = gc;
    return send_list_request(
        connection, XCB_POLY_FILL_ARC, request, arcs_len, arcs);
}

xcb_void_cookie_t xcb_copy_plane(
    xcb_connection_t *connection, xcb_drawable_t source,
    xcb_drawable_t destination, xcb_gcontext_t gc,
    std::int16_t source_x, std::int16_t source_y,
    std::int16_t destination_x, std::int16_t destination_y,
    std::uint16_t width, std::uint16_t height, std::uint32_t bit_plane)
{
    xcb_copy_plane_request_t request{};
    request.src_drawable = source;
    request.dst_drawable = destination;
    request.gc = gc;
    request.src_x = source_x;
    request.src_y = source_y;
    request.dst_x = destination_x;
    request.dst_y = destination_y;
    request.width = width;
    request.height = height;
    request.bit_plane = bit_plane;
    return xmin::client::x11::send_request<xcb_void_cookie_t>(
        connection, nullptr, XCB_COPY_PLANE, true, 0, request);
}

xcb_void_cookie_t xcb_no_operation(xcb_connection_t *connection)
{
    xcb_no_operation_request_t request{};
    return xmin::client::x11::send_request<xcb_void_cookie_t>(
        connection, nullptr, XCB_NO_OPERATION, true, 0, request);
}

xcb_query_keymap_cookie_t xcb_query_keymap(xcb_connection_t *connection)
{
    xcb_query_keymap_request_t request{};
    return xmin::client::x11::send_request<xcb_query_keymap_cookie_t>(
        connection, nullptr, XCB_QUERY_KEYMAP, false,
        XCB_REQUEST_CHECKED, request);
}

xcb_query_keymap_cookie_t xcb_query_keymap_unchecked(
    xcb_connection_t *connection)
{
    xcb_query_keymap_request_t request{};
    return xmin::client::x11::send_request<xcb_query_keymap_cookie_t>(
        connection, nullptr, XCB_QUERY_KEYMAP, false, 0, request);
}

xcb_query_keymap_reply_t *xcb_query_keymap_reply(
    xcb_connection_t *connection, xcb_query_keymap_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return xmin::client::x11::wait_for_reply<xcb_query_keymap_reply_t>(
        connection, cookie, error);
}

} // extern "C"
