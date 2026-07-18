/*
 * Small C++17 replacements for the xcb-util entry points used by Qt qxcb.
 *
 * This is deliberately not a general xcb-util implementation.  It provides
 * the ICCCM records, image upload, key lookup, Render format lookup, and core
 * cursor-font fallback that Qt needs when connected to Xmin.
 */
#include "xcb_protocol.hpp"

#include <xcb/render.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t
depth_mask(std::uint8_t depth)
{
    if (depth >= 32)
        return std::numeric_limits<std::uint32_t>::max();
    return depth == 0 ? 0U : (std::uint32_t{1} << depth) - 1U;
}

constexpr std::uint32_t
round_up_bits(std::uint32_t value, std::uint8_t alignment)
{
    return (value + alignment - 1U) & ~(std::uint32_t{alignment} - 1U);
}

bool
valid_image_format(
    std::uint8_t depth, std::uint8_t bpp, std::uint8_t unit,
    xcb_image_format_t format, std::uint8_t scanline_pad)
{
    if (depth == 0 || depth > bpp)
        return false;
    if (scanline_pad != 8 && scanline_pad != 16 && scanline_pad != 32)
        return false;

    const bool planar =
        format != XCB_IMAGE_FORMAT_Z_PIXMAP || bpp == 1;
    if (planar) {
        return (unit == 8 || unit == 16 || unit == 32) &&
               scanline_pad >= bpp;
    }

    switch (bpp) {
    case 4:
        return unit == 8;
    case 8:
    case 16:
    case 24:
    case 32:
        return unit == bpp;
    default:
        return false;
    }
}

xcb_render_pictforminfo_t *
first_render_format(const xcb_render_query_pict_formats_reply_t *reply)
{
    return reinterpret_cast<xcb_render_pictforminfo_t *>(
        const_cast<xcb_render_query_pict_formats_reply_t *>(reply) + 1);
}

std::byte *
first_render_screen(const xcb_render_query_pict_formats_reply_t *reply)
{
    return reinterpret_cast<std::byte *>(first_render_format(reply) +
                                         reply->num_formats);
}

bool
format_matches(
    const xcb_render_pictforminfo_t &candidate, unsigned long mask,
    const xcb_render_pictforminfo_t &wanted)
{
    return (!(mask & XCB_PICT_FORMAT_ID) || candidate.id == wanted.id) &&
           (!(mask & XCB_PICT_FORMAT_TYPE) || candidate.type == wanted.type) &&
           (!(mask & XCB_PICT_FORMAT_DEPTH) || candidate.depth == wanted.depth) &&
           (!(mask & XCB_PICT_FORMAT_RED) ||
            candidate.direct.red_shift == wanted.direct.red_shift) &&
           (!(mask & XCB_PICT_FORMAT_RED_MASK) ||
            candidate.direct.red_mask == wanted.direct.red_mask) &&
           (!(mask & XCB_PICT_FORMAT_GREEN) ||
            candidate.direct.green_shift == wanted.direct.green_shift) &&
           (!(mask & XCB_PICT_FORMAT_GREEN_MASK) ||
            candidate.direct.green_mask == wanted.direct.green_mask) &&
           (!(mask & XCB_PICT_FORMAT_BLUE) ||
            candidate.direct.blue_shift == wanted.direct.blue_shift) &&
           (!(mask & XCB_PICT_FORMAT_BLUE_MASK) ||
            candidate.direct.blue_mask == wanted.direct.blue_mask) &&
           (!(mask & XCB_PICT_FORMAT_ALPHA) ||
            candidate.direct.alpha_shift == wanted.direct.alpha_shift) &&
           (!(mask & XCB_PICT_FORMAT_ALPHA_MASK) ||
            candidate.direct.alpha_mask == wanted.direct.alpha_mask) &&
           (!(mask & XCB_PICT_FORMAT_COLORMAP) ||
            candidate.colormap == wanted.colormap);
}

struct StandardFormat {
    xcb_render_pictforminfo_t value;
    unsigned long mask;
};

constexpr unsigned long direct_color_mask =
    XCB_PICT_FORMAT_TYPE | XCB_PICT_FORMAT_DEPTH |
    XCB_PICT_FORMAT_RED | XCB_PICT_FORMAT_RED_MASK |
    XCB_PICT_FORMAT_GREEN | XCB_PICT_FORMAT_GREEN_MASK |
    XCB_PICT_FORMAT_BLUE | XCB_PICT_FORMAT_BLUE_MASK |
    XCB_PICT_FORMAT_ALPHA_MASK;

constexpr std::array<StandardFormat, 5> standard_formats{{
    {{0, XCB_RENDER_PICT_TYPE_DIRECT, 32, {0, 0},
      {16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff}, 0},
     direct_color_mask | XCB_PICT_FORMAT_ALPHA},
    {{0, XCB_RENDER_PICT_TYPE_DIRECT, 24, {0, 0},
      {16, 0xff, 8, 0xff, 0, 0xff, 0, 0}, 0},
     direct_color_mask},
    {{0, XCB_RENDER_PICT_TYPE_DIRECT, 8, {0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0xff}, 0},
     direct_color_mask | XCB_PICT_FORMAT_ALPHA},
    {{0, XCB_RENDER_PICT_TYPE_DIRECT, 4, {0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0x0f}, 0},
     direct_color_mask | XCB_PICT_FORMAT_ALPHA},
    {{0, XCB_RENDER_PICT_TYPE_DIRECT, 1, {0, 0},
      {0, 0, 0, 0, 0, 0, 0, 0x01}, 0},
     direct_color_mask | XCB_PICT_FORMAT_ALPHA},
}};

struct CursorName {
    std::string_view name;
    std::uint16_t glyph;
};

// Core cursor-font glyph numbers.  Aliases cover the names tried by Qt.
constexpr std::array<CursorName, 45> cursor_names{{
    {"left_ptr", 68}, {"default", 68}, {"top_left_arrow", 132},
    {"left_arrow", 68}, {"up_arrow", 22}, {"center_ptr", 22},
    {"cross", 30}, {"crosshair", 34}, {"wait", 150}, {"watch", 150},
    {"ibeam", 152}, {"text", 152}, {"xterm", 152},
    {"size_ver", 116}, {"ns-resize", 116}, {"v_double_arrow", 116},
    {"sb_v_double_arrow", 116}, {"size_hor", 108}, {"ew-resize", 108},
    {"h_double_arrow", 108}, {"sb_h_double_arrow", 108},
    {"size_bdiag", 136}, {"nesw-resize", 136},
    {"size_fdiag", 14}, {"nwse-resize", 14}, {"size_all", 52},
    {"fleur", 52}, {"split_v", 116}, {"row-resize", 116},
    {"split_h", 108}, {"col-resize", 108}, {"pointing_hand", 60},
    {"pointer", 60}, {"hand1", 58}, {"hand2", 60},
    {"forbidden", 24}, {"not-allowed", 24}, {"crossed_circle", 24},
    {"circle", 24}, {"whats_this", 92}, {"help", 92},
    {"question_arrow", 92}, {"left_ptr_watch", 150}, {"half-busy", 150},
    {"progress", 150},
}};

std::uint16_t
cursor_glyph(std::string_view name)
{
    const auto match = std::find_if(
        cursor_names.begin(), cursor_names.end(),
        [name](const CursorName &candidate) { return candidate.name == name; });
    return match == cursor_names.end() ? 0 : match->glyph;
}

} // namespace

struct _XCBKeySymbols {
    xcb_connection_t *connection = nullptr;
    xcb_get_keyboard_mapping_cookie_t cookie{};
    xcb_get_keyboard_mapping_reply_t *reply = nullptr;
};

struct xcb_cursor_context_t {
    xcb_connection_t *connection = nullptr;
    xcb_font_t cursor_font = XCB_NONE;
};

extern "C" {

void
xcb_icccm_size_hints_set_position(
    xcb_size_hints_t *hints, int user_specified, std::int32_t x,
    std::int32_t y)
{
    hints->flags |= user_specified ? XCB_ICCCM_SIZE_HINT_US_POSITION
                                   : XCB_ICCCM_SIZE_HINT_P_POSITION;
    hints->x = x;
    hints->y = y;
}

void
xcb_icccm_size_hints_set_size(
    xcb_size_hints_t *hints, int user_specified, std::int32_t width,
    std::int32_t height)
{
    hints->flags |= user_specified ? XCB_ICCCM_SIZE_HINT_US_SIZE
                                   : XCB_ICCCM_SIZE_HINT_P_SIZE;
    hints->width = width;
    hints->height = height;
}

void
xcb_icccm_size_hints_set_min_size(
    xcb_size_hints_t *hints, std::int32_t width, std::int32_t height)
{
    hints->flags |= XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
    hints->min_width = width;
    hints->min_height = height;
}

void
xcb_icccm_size_hints_set_max_size(
    xcb_size_hints_t *hints, std::int32_t width, std::int32_t height)
{
    hints->flags |= XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    hints->max_width = width;
    hints->max_height = height;
}

void
xcb_icccm_size_hints_set_resize_inc(
    xcb_size_hints_t *hints, std::int32_t width, std::int32_t height)
{
    hints->flags |= XCB_ICCCM_SIZE_HINT_P_RESIZE_INC;
    hints->width_inc = width;
    hints->height_inc = height;
}

void
xcb_icccm_size_hints_set_base_size(
    xcb_size_hints_t *hints, std::int32_t width, std::int32_t height)
{
    hints->flags |= XCB_ICCCM_SIZE_HINT_BASE_SIZE;
    hints->base_width = width;
    hints->base_height = height;
}

void
xcb_icccm_size_hints_set_win_gravity(
    xcb_size_hints_t *hints, xcb_gravity_t gravity)
{
    hints->flags |= XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY;
    hints->win_gravity = gravity;
}

xcb_void_cookie_t
xcb_icccm_set_wm_normal_hints(
    xcb_connection_t *connection, xcb_window_t window,
    xcb_size_hints_t *hints)
{
    return xcb_change_property(
        connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NORMAL_HINTS,
        XCB_ATOM_WM_SIZE_HINTS, 32, XCB_ICCCM_NUM_WM_SIZE_HINTS_ELEMENTS,
        hints);
}

void
xcb_icccm_wm_hints_set_input(xcb_icccm_wm_hints_t *hints, std::uint8_t input)
{
    hints->flags |= XCB_ICCCM_WM_HINT_INPUT;
    hints->input = input;
}

void
xcb_icccm_wm_hints_set_iconic(xcb_icccm_wm_hints_t *hints)
{
    hints->flags |= XCB_ICCCM_WM_HINT_STATE;
    hints->initial_state = XCB_ICCCM_WM_STATE_ICONIC;
}

void
xcb_icccm_wm_hints_set_normal(xcb_icccm_wm_hints_t *hints)
{
    hints->flags |= XCB_ICCCM_WM_HINT_STATE;
    hints->initial_state = XCB_ICCCM_WM_STATE_NORMAL;
}

xcb_void_cookie_t
xcb_icccm_set_wm_hints(
    xcb_connection_t *connection, xcb_window_t window,
    xcb_icccm_wm_hints_t *hints)
{
    return xcb_change_property(
        connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_HINTS,
        XCB_ATOM_WM_HINTS, 32, XCB_ICCCM_NUM_WM_HINTS_ELEMENTS, hints);
}

xcb_get_property_cookie_t
xcb_icccm_get_wm_hints_unchecked(
    xcb_connection_t *connection, xcb_window_t window)
{
    return xcb_get_property_unchecked(
        connection, 0, window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 0,
        XCB_ICCCM_NUM_WM_HINTS_ELEMENTS);
}

std::uint8_t
xcb_icccm_get_wm_hints_reply(
    xcb_connection_t *connection, xcb_get_property_cookie_t cookie,
    xcb_icccm_wm_hints_t *hints, xcb_generic_error_t **error)
{
    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(connection, cookie, error);
    if (reply == nullptr)
        return 0;
    const bool valid = reply->type == XCB_ATOM_WM_HINTS &&
                       reply->format == 32 &&
                       xcb_get_property_value_length(reply) >=
                           static_cast<int>(sizeof(*hints));
    if (valid)
        std::memcpy(hints, xcb_get_property_value(reply), sizeof(*hints));
    std::free(reply);
    return valid ? 1 : 0;
}

void
xcb_image_annotate(xcb_image_t *image)
{
    const bool planar = image->format != XCB_IMAGE_FORMAT_Z_PIXMAP ||
                        image->bpp == 1;
    if (planar) {
        image->stride = round_up_bits(image->width, image->scanline_pad) / 8U;
        image->size = image->height * image->stride * image->depth;
    } else {
        image->stride = round_up_bits(
                            static_cast<std::uint32_t>(image->width) *
                                image->bpp,
                            image->scanline_pad) /
                        8U;
        image->size = image->height * image->stride;
    }
}

xcb_image_t *
xcb_image_create(
    std::uint16_t width, std::uint16_t height, xcb_image_format_t format,
    std::uint8_t scanline_pad, std::uint8_t depth, std::uint8_t bpp,
    std::uint8_t unit, xcb_image_order_t byte_order,
    xcb_image_order_t bit_order, void *base, std::uint32_t bytes,
    std::uint8_t *data)
{
    if (unit == 0) {
        if (format != XCB_IMAGE_FORMAT_Z_PIXMAP || bpp == 1)
            unit = 32;
        else if (bpp < 8)
            unit = 8;
        else
            unit = bpp;
    }
    if (!valid_image_format(depth, bpp, unit, format, scanline_pad))
        return nullptr;

    auto *image = static_cast<xcb_image_t *>(
        std::calloc(1, sizeof(xcb_image_t)));
    if (image == nullptr)
        return nullptr;
    image->width = width;
    image->height = height;
    image->format = format;
    image->scanline_pad = scanline_pad;
    image->depth = depth;
    image->bpp = bpp;
    image->unit = unit;
    image->plane_mask = depth_mask(depth);
    image->byte_order = byte_order;
    image->bit_order = bit_order;
    xcb_image_annotate(image);

    if (base == nullptr && data == nullptr &&
        bytes == std::numeric_limits<std::uint32_t>::max())
        return image;
    if (base == nullptr && data != nullptr && bytes == 0)
        bytes = image->size;

    image->base = base;
    image->data = data;
    if (image->data == nullptr) {
        if (image->base != nullptr) {
            image->data = static_cast<std::uint8_t *>(image->base);
        } else {
            bytes = image->size;
            image->base = std::malloc(bytes);
            image->data = static_cast<std::uint8_t *>(image->base);
        }
    }
    if (image->data == nullptr || bytes < image->size) {
        if (base == nullptr)
            std::free(image->base);
        std::free(image);
        return nullptr;
    }
    return image;
}

void
xcb_image_destroy(xcb_image_t *image)
{
    if (image == nullptr)
        return;
    std::free(image->base);
    std::free(image);
}

xcb_void_cookie_t
xcb_put_image(
    xcb_connection_t *connection, std::uint8_t format,
    xcb_drawable_t drawable, xcb_gcontext_t gc, std::uint16_t width,
    std::uint16_t height, std::int16_t x, std::int16_t y,
    std::uint8_t left_pad, std::uint8_t depth, std::uint32_t data_length,
    const std::uint8_t *data)
{
    xcb_put_image_request_t request{};
    request.format = format;
    request.drawable = drawable;
    request.gc = gc;
    request.width = width;
    request.height = height;
    request.dst_x = x;
    request.dst_y = y;
    request.left_pad = left_pad;
    request.depth = depth;
    return xmin::client::x11::send_request<xcb_void_cookie_t>(
        connection, nullptr, XCB_PUT_IMAGE, true, 0, request,
        {{data, data_length}});
}

xcb_void_cookie_t
xcb_image_put(
    xcb_connection_t *connection, xcb_drawable_t drawable, xcb_gcontext_t gc,
    xcb_image_t *image, std::int16_t x, std::int16_t y,
    std::uint8_t left_pad)
{
    return xcb_put_image(
        connection, image->format, drawable, gc, image->width, image->height,
        x, y, left_pad, image->depth, image->size, image->data);
}

xcb_image_t *
xcb_image_create_from_bitmap_data(
    std::uint8_t *data, std::uint32_t width, std::uint32_t height)
{
    if (width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max())
        return nullptr;
    return xcb_image_create(
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
        XCB_IMAGE_FORMAT_XY_PIXMAP, 8, 1, 1, 8,
        XCB_IMAGE_ORDER_LSB_FIRST, XCB_IMAGE_ORDER_LSB_FIRST, nullptr, 0,
        data);
}

xcb_pixmap_t
xcb_create_pixmap_from_bitmap_data(
    xcb_connection_t *connection, xcb_drawable_t drawable, std::uint8_t *data,
    std::uint32_t width, std::uint32_t height, std::uint32_t depth,
    std::uint32_t foreground, std::uint32_t background,
    xcb_gcontext_t *returned_gc)
{
    xcb_image_t *image = xcb_image_create_from_bitmap_data(data, width, height);
    const xcb_setup_t *setup = connection == nullptr
        ? nullptr
        : xcb_get_setup(connection);
    if (image == nullptr || setup == nullptr || depth == 0 ||
        depth > std::numeric_limits<std::uint8_t>::max()) {
        xcb_image_destroy(image);
        return XCB_NONE;
    }
    const std::uint32_t native_stride = round_up_bits(
        static_cast<std::uint32_t>(image->width),
        setup->bitmap_format_scanline_pad) / 8U;
    const std::uint64_t native_size =
        static_cast<std::uint64_t>(native_stride) * image->height;
    if (native_size > std::numeric_limits<std::uint32_t>::max()) {
        xcb_image_destroy(image);
        return XCB_NONE;
    }
    std::vector<std::uint8_t> native;
    try {
        native.assign(static_cast<std::size_t>(native_size), 0);
    }
    catch (...) {
        xcb_image_destroy(image);
        return XCB_NONE;
    }
    for (std::uint32_t row = 0; row < image->height; ++row) {
        for (std::uint32_t column = 0; column < image->width; ++column) {
            const auto source = image->data +
                static_cast<std::size_t>(row) * image->stride;
            if (((source[column / 8U] >> (column & 7U)) & 1U) == 0)
                continue;
            auto &destination = native[
                static_cast<std::size_t>(row) * native_stride + column / 8U];
            destination |= static_cast<std::uint8_t>(
                setup->bitmap_format_bit_order == XCB_IMAGE_ORDER_LSB_FIRST
                    ? 1U << (column & 7U)
                    : 0x80U >> (column & 7U));
        }
    }

    const xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(
        connection, static_cast<std::uint8_t>(depth), pixmap, drawable,
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height));
    const xcb_gcontext_t gc = xcb_generate_id(connection);
    const std::array<std::uint32_t, 2> values{{foreground, background}};
    xcb_create_gc(
        connection, gc, pixmap, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND,
        values.data());
    xcb_put_image(
        connection,
        depth > 1 ? XCB_IMAGE_FORMAT_XY_BITMAP : XCB_IMAGE_FORMAT_XY_PIXMAP,
        pixmap, gc, image->width, image->height, 0, 0, 0, 1,
        static_cast<std::uint32_t>(native.size()), native.data());
    xcb_image_destroy(image);
    if (returned_gc != nullptr)
        *returned_gc = gc;
    else
        xcb_free_gc(connection, gc);
    return pixmap;
}

xcb_key_symbols_t *
xcb_key_symbols_alloc(xcb_connection_t *connection)
{
    if (connection == nullptr || xcb_connection_has_error(connection))
        return nullptr;
    auto *symbols = new (std::nothrow) xcb_key_symbols_t;
    if (symbols == nullptr)
        return nullptr;
    symbols->connection = connection;
    const xcb_setup_t *setup = xcb_get_setup(connection);
    symbols->cookie = xcb_get_keyboard_mapping(
        connection, setup->min_keycode,
        static_cast<std::uint8_t>(setup->max_keycode - setup->min_keycode + 1));
    return symbols;
}

void
xcb_key_symbols_free(xcb_key_symbols_t *symbols)
{
    if (symbols == nullptr)
        return;
    std::free(symbols->reply);
    delete symbols;
}

xcb_keycode_t *
xcb_key_symbols_get_keycode(
    xcb_key_symbols_t *symbols, xcb_keysym_t wanted)
{
    if (symbols == nullptr || xcb_connection_has_error(symbols->connection))
        return nullptr;
    if (symbols->reply == nullptr) {
        symbols->reply = xcb_get_keyboard_mapping_reply(
            symbols->connection, symbols->cookie, nullptr);
    }
    if (symbols->reply == nullptr)
        return nullptr;

    const xcb_setup_t *setup = xcb_get_setup(symbols->connection);
    const auto per_keycode = symbols->reply->keysyms_per_keycode;
    const xcb_keysym_t *keysyms =
        xcb_get_keyboard_mapping_keysyms(symbols->reply);
    std::vector<xcb_keycode_t> matches;
    for (unsigned code = setup->min_keycode; code <= setup->max_keycode; ++code) {
        const std::size_t offset =
            (code - setup->min_keycode) * per_keycode;
        if (std::find(
                keysyms + offset, keysyms + offset + per_keycode, wanted) !=
            keysyms + offset + per_keycode) {
            matches.push_back(static_cast<xcb_keycode_t>(code));
        }
    }
    if (matches.empty())
        return nullptr;
    auto *result = static_cast<xcb_keycode_t *>(
        std::calloc(257, sizeof(xcb_keycode_t)));
    if (result == nullptr)
        return nullptr;
    std::copy(matches.begin(), matches.end(), result);
    return result;
}

int
xcb_refresh_keyboard_mapping(
    xcb_key_symbols_t *symbols, xcb_mapping_notify_event_t *event)
{
    if (symbols == nullptr || event == nullptr ||
        event->request != XCB_MAPPING_KEYBOARD ||
        xcb_connection_has_error(symbols->connection))
        return 0;
    std::free(symbols->reply);
    symbols->reply = nullptr;
    const xcb_setup_t *setup = xcb_get_setup(symbols->connection);
    symbols->cookie = xcb_get_keyboard_mapping(
        symbols->connection, setup->min_keycode,
        static_cast<std::uint8_t>(setup->max_keycode - setup->min_keycode + 1));
    return 1;
}

xcb_render_pictforminfo_t *
xcb_render_util_find_format(
    const xcb_render_query_pict_formats_reply_t *formats, unsigned long mask,
    const xcb_render_pictforminfo_t *wanted, int count)
{
    if (formats == nullptr || wanted == nullptr || count < 0)
        return nullptr;
    auto *candidate = first_render_format(formats);
    for (std::uint32_t index = 0; index < formats->num_formats;
         ++index, ++candidate) {
        if (format_matches(*candidate, mask, *wanted) && count-- == 0)
            return candidate;
    }
    return nullptr;
}

xcb_render_pictforminfo_t *
xcb_render_util_find_standard_format(
    const xcb_render_query_pict_formats_reply_t *formats,
    xcb_pict_standard_t standard)
{
    const auto index = static_cast<std::size_t>(standard);
    if (index >= standard_formats.size())
        return nullptr;
    return xcb_render_util_find_format(
        formats, standard_formats[index].mask, &standard_formats[index].value,
        0);
}

xcb_render_pictvisual_t *
xcb_render_util_find_visual_format(
    const xcb_render_query_pict_formats_reply_t *formats,
    xcb_visualid_t visual)
{
    if (formats == nullptr)
        return nullptr;
    std::byte *cursor = first_render_screen(formats);
    for (std::uint32_t screen_index = 0;
         screen_index < formats->num_screens; ++screen_index) {
        auto *screen = reinterpret_cast<xcb_render_pictscreen_t *>(cursor);
        cursor += sizeof(*screen);
        for (std::uint32_t depth_index = 0;
             depth_index < screen->num_depths; ++depth_index) {
            auto *depth = reinterpret_cast<xcb_render_pictdepth_t *>(cursor);
            cursor += sizeof(*depth);
            auto *visuals = reinterpret_cast<xcb_render_pictvisual_t *>(cursor);
            for (std::uint16_t visual_index = 0;
                 visual_index < depth->num_visuals; ++visual_index) {
                if (visuals[visual_index].visual == visual)
                    return &visuals[visual_index];
            }
            cursor += sizeof(*visuals) * depth->num_visuals;
        }
    }
    return nullptr;
}

int
xcb_cursor_context_new(
    xcb_connection_t *connection, xcb_screen_t *, xcb_cursor_context_t **result)
{
    if (connection == nullptr || result == nullptr ||
        xcb_connection_has_error(connection))
        return -1;
    auto *context = new (std::nothrow) xcb_cursor_context_t;
    if (context == nullptr)
        return -1;
    context->connection = connection;
    context->cursor_font = xcb_generate_id(connection);
    constexpr char font_name[] = "cursor";
    xcb_open_font(
        connection, context->cursor_font, sizeof(font_name) - 1, font_name);
    *result = context;
    return 0;
}

xcb_cursor_t
xcb_cursor_load_cursor(xcb_cursor_context_t *context, const char *name)
{
    if (context == nullptr || name == nullptr)
        return XCB_NONE;
    const std::uint16_t glyph = cursor_glyph(name);
    if (glyph == 0)
        return XCB_NONE;
    const xcb_cursor_t cursor = xcb_generate_id(context->connection);
    xcb_create_glyph_cursor(
        context->connection, cursor, context->cursor_font,
        context->cursor_font, glyph, glyph + 1, 0, 0, 0,
        std::numeric_limits<std::uint16_t>::max(),
        std::numeric_limits<std::uint16_t>::max(),
        std::numeric_limits<std::uint16_t>::max());
    return cursor;
}

void
xcb_cursor_context_free(xcb_cursor_context_t *context)
{
    if (context == nullptr)
        return;
    xcb_close_font(context->connection, context->cursor_font);
    delete context;
}

} // extern "C"
