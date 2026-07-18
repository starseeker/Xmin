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
        display, screen->root, 0, 0, 180, 64, 0, screen->black_pixel,
        screen->black_pixel);
    XMapWindow(display, window);

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
    Region clip = XCreateRegion();
    XRectangle clip_rectangle{0, 0, 86, 64};
    XUnionRectWithRegion(&clip_rectangle, clip, clip);
    if (draw == nullptr || !XftDrawSetClip(draw, clip)) {
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
    XFlush(display);

    XImage *image = XGetImage(
        display, window, 0, 0, 180, 64, AllPlanes, ZPixmap);
    std::size_t opaque = 0;
    std::size_t partial = 0;
    std::size_t outside_clip = 0;
    if (image != nullptr) {
        for (int y = 0; y < image->height; ++y) {
            for (int x = 0; x < image->width; ++x) {
                const auto pixel = XGetPixel(image, x, y);
                const auto red = static_cast<std::uint8_t>(pixel >> 16U);
                opaque += red == 0xffU;
                partial += red != 0 && red != 0xffU;
                outside_clip += x >= 86 && red != 0;
            }
        }
        XDestroyImage(image);
    }

    XftDrawDestroy(draw);
    XftFontClose(display, font);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    if (opaque < 30 || partial < 20 || outside_clip != 0) {
        std::cerr << "Xft facade did not render the embedded face\n";
        return 5;
    }
    return 0;
}
