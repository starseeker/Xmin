#include "render_font.hpp"

#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_renderutil.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

struct FreeDeleter {
    void operator()(void *pointer) const noexcept
    {
        std::free(pointer);
    }
};

bool checked(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> error(
        xcb_request_check(connection, cookie));
    return !error;
}

xcb_screen_t *default_screen(xcb_connection_t *connection)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (setup == nullptr) {
        return nullptr;
    }
    const xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    return screens.rem == 0 ? nullptr : screens.data;
}

} // namespace

int main()
{
    int preferred_screen = 0;
    xcb_connection_t *connection = xcb_connect(nullptr, &preferred_screen);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        return 1;
    }
    xcb_screen_t *screen = default_screen(connection);
    if (screen == nullptr) {
        xcb_disconnect(connection);
        return 2;
    }

    xcb_generic_error_t *raw_error = nullptr;
    std::unique_ptr<xcb_render_query_pict_formats_reply_t, FreeDeleter> formats(
        xcb_render_query_pict_formats_reply(
            connection, xcb_render_query_pict_formats(connection),
            &raw_error));
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> format_error(raw_error);
    const auto *visual = formats
        ? xcb_render_util_find_visual_format(
              formats.get(), screen->root_visual)
        : nullptr;
    if (format_error || visual == nullptr) {
        xcb_disconnect(connection);
        return 3;
    }

    const xcb_window_t window = xcb_generate_id(connection);
    xcb_create_window(
        connection, screen->root_depth, window, screen->root, 0, 0, 160, 60,
        0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_map_window(connection, window);
    const xcb_render_picture_t destination = xcb_generate_id(connection);
    const xcb_render_picture_t source = xcb_generate_id(connection);
    const xcb_render_color_t red{0xffffU, 0, 0, 0xffffU};
    if (!checked(
            connection, xcb_render_create_picture_checked(
                            connection, destination, window, visual->format,
                            0, nullptr)) ||
        !checked(
            connection,
            xcb_render_create_solid_fill_checked(connection, source, red))) {
        xcb_disconnect(connection);
        return 4;
    }

    bool rendered = false;
    {
        xmin::client::font::RenderFont font(
            connection,
            xmin::client::font::EmbeddedFace::sans_bold_italic, 24.0F);
        const std::array glyphs{
            font.font().glyph_index(U'X'), font.font().glyph_index(U'm'),
            font.font().glyph_index(U'i'), font.font().glyph_index(U'n')};
        rendered = font.valid() &&
            font.draw(
                source, destination, 8, 34, glyphs.data(), glyphs.size()) &&
            font.draw(
                source, destination, 82, 34, glyphs.data(), glyphs.size());
        xcb_flush(connection);

        std::unique_ptr<xcb_get_image_reply_t, FreeDeleter> image(
            xcb_get_image_reply(
                connection,
                xcb_get_image_unchecked(
                    connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, 160,
                    60, UINT32_MAX),
                nullptr));
        if (image && xcb_get_image_data_length(image.get()) >=
                         static_cast<int>(160 * 60 * sizeof(std::uint32_t))) {
            std::vector<std::uint32_t> pixels(160 * 60);
            std::memcpy(
                pixels.data(), xcb_get_image_data(image.get()),
                pixels.size() * sizeof(pixels.front()));
            std::size_t opaque_red = 0;
            std::size_t partial_red = 0;
            for (const std::uint32_t pixel : pixels) {
                const std::uint8_t red_channel =
                    static_cast<std::uint8_t>((pixel >> 16U) & 0xffU);
                opaque_red += red_channel == 0xffU;
                partial_red += red_channel != 0 && red_channel != 0xffU;
            }
            rendered = rendered && opaque_red > 40 && partial_red > 20;
        }
        else {
            rendered = false;
        }
    }

    xcb_render_free_picture(connection, source);
    xcb_render_free_picture(connection, destination);
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
    xcb_disconnect(connection);
    if (!rendered) {
        std::cerr << "embedded font did not reach the Xmin RENDER surface\n";
        return 5;
    }
    return 0;
}
