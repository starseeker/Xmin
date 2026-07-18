/*
 * Focused C++17 RENDER encoders used by Xmin's embedded-font client.
 * Public request layouts come from the pinned xcb-proto header; no generated
 * libxcb implementation source is incorporated here.
 */
#include "xcb_protocol.hpp"

#include <xcb/render.h>
#include <xcb/xcbext.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

template <typename Request>
xcb_void_cookie_t send_render_request(
    xcb_connection_t *connection, std::uint8_t opcode, int flags,
    const Request &request,
    const std::vector<xmin::client::x11::RequestPart> &additional = {})
{
    return xmin::client::x11::send_request<xcb_void_cookie_t>(
        connection, &xcb_render_id, opcode, true, flags, request, additional);
}

template <typename Request>
xcb_void_cookie_t composite_glyphs(
    xcb_connection_t *connection, std::uint8_t opcode, int flags,
    std::uint8_t operation, xcb_render_picture_t source,
    xcb_render_picture_t destination, xcb_render_pictformat_t mask_format,
    xcb_render_glyphset_t glyphset, std::int16_t source_x,
    std::int16_t source_y, std::uint32_t commands_length,
    const std::uint8_t *commands)
{
    Request request{};
    request.op = operation;
    request.src = source;
    request.dst = destination;
    request.mask_format = mask_format;
    request.glyphset = glyphset;
    request.src_x = source_x;
    request.src_y = source_y;
    return send_render_request(
        connection, opcode, flags, request,
        {{commands, static_cast<std::size_t>(commands_length)}});
}

std::size_t value_count(std::uint32_t mask) noexcept
{
    std::size_t count = 0;
    while (mask != 0) {
        count += mask & 1U;
        mask >>= 1U;
    }
    return count;
}

} // namespace

extern "C" {

xcb_void_cookie_t xcb_render_change_picture_checked(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    std::uint32_t value_mask, const void *value_list)
{
    xcb_render_change_picture_request_t request{};
    request.picture = picture;
    request.value_mask = value_mask;
    const std::size_t count = value_count(value_mask);
    return send_render_request(
        connection, XCB_RENDER_CHANGE_PICTURE, XCB_REQUEST_CHECKED, request,
        {{value_list, count * sizeof(std::uint32_t)}});
}

xcb_void_cookie_t xcb_render_change_picture(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    std::uint32_t value_mask, const void *value_list)
{
    xcb_render_change_picture_request_t request{};
    request.picture = picture;
    request.value_mask = value_mask;
    const std::size_t count = value_count(value_mask);
    return send_render_request(
        connection, XCB_RENDER_CHANGE_PICTURE, 0, request,
        {{value_list, count * sizeof(std::uint32_t)}});
}

xcb_void_cookie_t xcb_render_set_picture_clip_rectangles_checked(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    std::int16_t origin_x, std::int16_t origin_y,
    std::uint32_t rectangles_length, const xcb_rectangle_t *rectangles)
{
    xcb_render_set_picture_clip_rectangles_request_t request{};
    request.picture = picture;
    request.clip_x_origin = origin_x;
    request.clip_y_origin = origin_y;
    return send_render_request(
        connection, XCB_RENDER_SET_PICTURE_CLIP_RECTANGLES,
        XCB_REQUEST_CHECKED, request,
        {{rectangles, static_cast<std::size_t>(rectangles_length) *
                          sizeof(*rectangles)}});
}

xcb_void_cookie_t xcb_render_set_picture_clip_rectangles(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    std::int16_t origin_x, std::int16_t origin_y,
    std::uint32_t rectangles_length, const xcb_rectangle_t *rectangles)
{
    xcb_render_set_picture_clip_rectangles_request_t request{};
    request.picture = picture;
    request.clip_x_origin = origin_x;
    request.clip_y_origin = origin_y;
    return send_render_request(
        connection, XCB_RENDER_SET_PICTURE_CLIP_RECTANGLES, 0, request,
        {{rectangles, static_cast<std::size_t>(rectangles_length) *
                          sizeof(*rectangles)}});
}

xcb_void_cookie_t xcb_render_create_glyph_set_checked(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    xcb_render_pictformat_t format)
{
    xcb_render_create_glyph_set_request_t request{};
    request.gsid = glyphset;
    request.format = format;
    return send_render_request(
        connection, XCB_RENDER_CREATE_GLYPH_SET, XCB_REQUEST_CHECKED, request);
}

xcb_void_cookie_t xcb_render_create_glyph_set(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    xcb_render_pictformat_t format)
{
    xcb_render_create_glyph_set_request_t request{};
    request.gsid = glyphset;
    request.format = format;
    return send_render_request(
        connection, XCB_RENDER_CREATE_GLYPH_SET, 0, request);
}

xcb_void_cookie_t xcb_render_free_glyph_set_checked(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset)
{
    xcb_render_free_glyph_set_request_t request{};
    request.glyphset = glyphset;
    return send_render_request(
        connection, XCB_RENDER_FREE_GLYPH_SET, XCB_REQUEST_CHECKED, request);
}

xcb_void_cookie_t xcb_render_free_glyph_set(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset)
{
    xcb_render_free_glyph_set_request_t request{};
    request.glyphset = glyphset;
    return send_render_request(
        connection, XCB_RENDER_FREE_GLYPH_SET, 0, request);
}

xcb_void_cookie_t xcb_render_add_glyphs_checked(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    std::uint32_t glyphs_length, const std::uint32_t *glyph_ids,
    const xcb_render_glyphinfo_t *glyphs, std::uint32_t data_length,
    const std::uint8_t *data)
{
    xcb_render_add_glyphs_request_t request{};
    request.glyphset = glyphset;
    request.glyphs_len = glyphs_length;
    return send_render_request(
        connection, XCB_RENDER_ADD_GLYPHS, XCB_REQUEST_CHECKED, request,
        {{glyph_ids, static_cast<std::size_t>(glyphs_length) *
                         sizeof(*glyph_ids)},
         {glyphs, static_cast<std::size_t>(glyphs_length) * sizeof(*glyphs)},
         {data, static_cast<std::size_t>(data_length)}});
}

xcb_void_cookie_t xcb_render_add_glyphs(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    std::uint32_t glyphs_length, const std::uint32_t *glyph_ids,
    const xcb_render_glyphinfo_t *glyphs, std::uint32_t data_length,
    const std::uint8_t *data)
{
    xcb_render_add_glyphs_request_t request{};
    request.glyphset = glyphset;
    request.glyphs_len = glyphs_length;
    return send_render_request(
        connection, XCB_RENDER_ADD_GLYPHS, 0, request,
        {{glyph_ids, static_cast<std::size_t>(glyphs_length) *
                         sizeof(*glyph_ids)},
         {glyphs, static_cast<std::size_t>(glyphs_length) * sizeof(*glyphs)},
         {data, static_cast<std::size_t>(data_length)}});
}

xcb_void_cookie_t xcb_render_free_glyphs_checked(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    std::uint32_t glyphs_length, const xcb_render_glyph_t *glyphs)
{
    xcb_render_free_glyphs_request_t request{};
    request.glyphset = glyphset;
    return send_render_request(
        connection, XCB_RENDER_FREE_GLYPHS, XCB_REQUEST_CHECKED, request,
        {{glyphs, static_cast<std::size_t>(glyphs_length) * sizeof(*glyphs)}});
}

xcb_void_cookie_t xcb_render_free_glyphs(
    xcb_connection_t *connection, xcb_render_glyphset_t glyphset,
    std::uint32_t glyphs_length, const xcb_render_glyph_t *glyphs)
{
    xcb_render_free_glyphs_request_t request{};
    request.glyphset = glyphset;
    return send_render_request(
        connection, XCB_RENDER_FREE_GLYPHS, 0, request,
        {{glyphs, static_cast<std::size_t>(glyphs_length) * sizeof(*glyphs)}});
}

xcb_void_cookie_t xcb_render_composite_glyphs_8_checked(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_8_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_8, XCB_REQUEST_CHECKED,
        operation, source, destination, mask_format, glyphset, source_x,
        source_y, commands_length, commands);
}

xcb_void_cookie_t xcb_render_composite_glyphs_8(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_8_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_8, 0, operation, source,
        destination, mask_format, glyphset, source_x, source_y,
        commands_length, commands);
}

xcb_void_cookie_t xcb_render_composite_glyphs_16_checked(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_16_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_16, XCB_REQUEST_CHECKED,
        operation, source, destination, mask_format, glyphset, source_x,
        source_y, commands_length, commands);
}

xcb_void_cookie_t xcb_render_composite_glyphs_16(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_16_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_16, 0, operation, source,
        destination, mask_format, glyphset, source_x, source_y,
        commands_length, commands);
}

xcb_void_cookie_t xcb_render_composite_glyphs_32_checked(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_32_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_32, XCB_REQUEST_CHECKED,
        operation, source, destination, mask_format, glyphset, source_x,
        source_y, commands_length, commands);
}

xcb_void_cookie_t xcb_render_composite_glyphs_32(
    xcb_connection_t *connection, std::uint8_t operation,
    xcb_render_picture_t source, xcb_render_picture_t destination,
    xcb_render_pictformat_t mask_format, xcb_render_glyphset_t glyphset,
    std::int16_t source_x, std::int16_t source_y,
    std::uint32_t commands_length, const std::uint8_t *commands)
{
    return composite_glyphs<xcb_render_composite_glyphs_32_request_t>(
        connection, XCB_RENDER_COMPOSITE_GLYPHS_32, 0, operation, source,
        destination, mask_format, glyphset, source_x, source_y,
        commands_length, commands);
}

xcb_void_cookie_t xcb_render_create_solid_fill_checked(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    xcb_render_color_t color)
{
    xcb_render_create_solid_fill_request_t request{};
    request.picture = picture;
    request.color = color;
    return send_render_request(
        connection, XCB_RENDER_CREATE_SOLID_FILL, XCB_REQUEST_CHECKED,
        request);
}

xcb_void_cookie_t xcb_render_create_solid_fill(
    xcb_connection_t *connection, xcb_render_picture_t picture,
    xcb_render_color_t color)
{
    xcb_render_create_solid_fill_request_t request{};
    request.picture = picture;
    request.color = color;
    return send_render_request(
        connection, XCB_RENDER_CREATE_SOLID_FILL, 0, request);
}

} // extern "C"
