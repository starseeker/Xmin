#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

int main()
{
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return 1;
    }
    const int screen_number = DefaultScreen(display);
    Screen *screen = ScreenOfDisplay(display, screen_number);
    const Window window = XCreateSimpleWindow(
        display, screen->root, 0, 0, 260, 64, 0, screen->black_pixel,
        screen->black_pixel);
    XMapWindow(display, window);
    const Pixmap buffer = XCreatePixmap(
        display, window, 260, 64, static_cast<unsigned int>(screen->root_depth));
    GC buffer_gc = XCreateGC(display, buffer, 0, nullptr);
    XSetForeground(display, buffer_gc, screen->black_pixel);
    XFillRectangle(display, buffer, buffer_gc, 0, 0, 260, 64);

    FcPattern *request = XftPatternCreate();
    XftPatternAddString(request, XFT_FAMILY, "sans");
    XftPatternAddInteger(request, XFT_WEIGHT, XFT_WEIGHT_BOLD);
    XftPatternAddInteger(request, XFT_SLANT, XFT_SLANT_ITALIC);
    XftPatternAddDouble(request, XFT_PIXEL_SIZE, 24.0);
    XftResult match_result = XftResultNoMatch;
    FcPattern *match = XftFontMatch(
        display, screen_number, request, &match_result);
    XftPatternDestroy(request);
    XftFont *font = match == nullptr
        ? nullptr
        : XftFontOpenPattern(display, match);
    if (font == nullptr || match_result != XftResultMatch ||
        font->ascent <= 0 || font->descent < 0 || font->height < 20) {
        XCloseDisplay(display);
        return 2;
    }

    constexpr std::array<FcChar32, 6> text{
        U'X', U'm', U'i', U'n', U' ', U'\u03a9'};
    XGlyphInfo extents{};
    XftTextExtents32(
        display, font, text.data(), static_cast<int>(text.size()), &extents);
    if (extents.xOff <= 40 || extents.width == 0 || extents.height == 0) {
        XftFontClose(display, font);
        XCloseDisplay(display);
        return 3;
    }

    XftDraw *draw = XftDrawCreate(
        display, window, screen->root_visual, screen->cmap);
    if (draw == nullptr) {
        XftFontClose(display, font);
        XFreeGC(display, buffer_gc);
        XFreePixmap(display, buffer);
        XCloseDisplay(display);
        return 4;
    }
    XftDrawChange(draw, buffer);
    Region clip = XCreateRegion();
    XRectangle clip_rectangle{0, 0, 86, 64};
    XUnionRectWithRegion(&clip_rectangle, clip, clip);
    if (!XftDrawSetClip(draw, clip)) {
        XDestroyRegion(clip);
        XftFontClose(display, font);
        XCloseDisplay(display);
        return 4;
    }
    XDestroyRegion(clip);
    const XftColor color{
        0x00ff0000UL, {0xffffU, 0, 0, 0xffffU}};
    XftDrawString32(
        draw, &color, font, 8, 38, text.data(),
        static_cast<int>(text.size()));
    XftDrawString32(
        draw, &color, font, 94, 38, text.data(), 2);
    XftDrawSetClip(draw, nullptr);
    XRectangle terminal_clip{0, 0, 100, 56};
    if (!XftDrawSetClipRectangles(draw, 140, 4, &terminal_clip, 1)) {
        XftDrawDestroy(draw);
        XftFontClose(display, font);
        XFreeGC(display, buffer_gc);
        XFreePixmap(display, buffer);
        XCloseDisplay(display);
        return 5;
    }
    XftFont *terminal_font = XftFontOpenName(
        display, screen_number, "Go Mono:pixelsize=14:antialias=true");
    XftColor terminal_color{};
    if (terminal_font == nullptr ||
        !XftColorAllocName(
            display, screen->root_visual, screen->cmap, "gray90",
            &terminal_color)) {
        XftDrawDestroy(draw);
        XftFontClose(display, font);
        XFreeGC(display, buffer_gc);
        XFreePixmap(display, buffer);
        XCloseDisplay(display);
        return 6;
    }
    const std::array<XftGlyphFontSpec, 2> glyph_specs{{
        {terminal_font, XftCharIndex(display, terminal_font, U'X'), 150, 38},
        {terminal_font, XftCharIndex(display, terminal_font, U'm'), 170, 38},
    }};
    XftDrawGlyphFontSpec(
        draw, &terminal_color, glyph_specs.data(),
        static_cast<int>(glyph_specs.size()));
    XftFont *desktop_font = XftFontOpenName(
        display, screen_number, "Go Sans-12");
    const XRenderColor desktop_value{
        0xf2f2U, 0xf2f2U, 0xf2f2U, 0xffffU};
    XftColor desktop_color{};
    Region desktop_clip = XCreateRegion();
    XRectangle desktop_rectangle{2, 3, 33, 12};
    XUnionRectWithRegion(
        &desktop_rectangle, desktop_clip, desktop_clip);
    if (desktop_font == nullptr ||
        !XftColorAllocValue(
            display, screen->root_visual, screen->cmap, &desktop_value,
            &desktop_color) ||
        !XftDrawSetClip(draw, desktop_clip)) {
        XDestroyRegion(desktop_clip);
        XftDrawDestroy(draw);
        XftFontClose(display, font);
        XFreeGC(display, buffer_gc);
        XFreePixmap(display, buffer);
        XCloseDisplay(display);
        return 7;
    }
    XDestroyRegion(desktop_clip);
    constexpr std::array<FcChar8, 5> desktop_text{'1', '2', ':', '3', '4'};
    XftDrawStringUtf8(
        draw, &desktop_color, desktop_font, 2, 13, desktop_text.data(),
        static_cast<int>(desktop_text.size()));
    XftDrawDestroy(draw);
    draw = nullptr;
    XCopyArea(display, buffer, window, buffer_gc, 0, 0, 260, 64, 0, 0);
    XFlush(display);

    XImage *image = XGetImage(
        display, window, 0, 0, 260, 64, AllPlanes, ZPixmap);
    std::size_t opaque = 0;
    std::size_t partial = 0;
    std::size_t outside_clip = 0;
    std::size_t glyph_spec_pixels = 0;
    std::size_t desktop_pixels = 0;
    if (image != nullptr) {
        for (int y = 0; y < image->height; ++y) {
            for (int x = 0; x < image->width; ++x) {
                const auto pixel = XGetPixel(image, x, y);
                const auto red = static_cast<std::uint8_t>(pixel >> 16U);
                opaque += red == 0xffU;
                partial += red != 0 && red != 0xffU;
                outside_clip += x >= 86 && x < 130 && red != 0;
                glyph_spec_pixels += x >= 140 && red != 0;
                desktop_pixels += x >= 2 && x < 35 && y >= 3 && y < 15 &&
                    (pixel & 0x00ffffffUL) != 0;
            }
        }
        XDestroyImage(image);
    }

    XftDrawDestroy(draw);
    XftColorFree(
        display, screen->root_visual, screen->cmap, &terminal_color);
    XftColorFree(
        display, screen->root_visual, screen->cmap, &desktop_color);
    XftFontClose(display, desktop_font);
    XftFontClose(display, terminal_font);
    XFreeGC(display, buffer_gc);
    XFreePixmap(display, buffer);
    XftFontClose(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    if (opaque < 30 || partial < 20 || outside_clip != 0 ||
        glyph_spec_pixels < 20 || desktop_pixels < 20) {
        std::cerr << "Xft facade did not render the embedded face\n";
        return 5;
    }
    return 0;
}
