#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdlib>
#include <iostream>

extern "C" int _XInitImageFuncPtrs(XImage *image);

namespace {

int fail(Display *display, const char *message, int result)
{
    std::cerr << message << '\n';
    if (display != nullptr)
        XCloseDisplay(display);
    return result;
}

} // namespace

int main()
{
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr)
        return fail(nullptr, "could not open the Xmin display", 1);

    XColor color{};
    if (!XParseColor(display, DefaultColormap(display, 0),
                     "#123456789abc", &color) ||
        color.red != 0x1234 || color.green != 0x5678 ||
        color.blue != 0x9abc ||
        XStringToKeysym("backslash") != XK_backslash ||
        XStringToKeysym("F35") != XK_F35 ||
        XStringToKeysym("U03A9") != 0x010003a9UL) {
        return fail(display, "color or keysym compatibility failed", 2);
    }

    Region outer = XCreateRegion();
    Region cut = XCreateRegion();
    Region result = XCreateRegion();
    XRectangle outer_rectangle{0, 0, 20, 20};
    XRectangle cut_rectangle{5, 5, 10, 10};
    XUnionRectWithRegion(&outer_rectangle, outer, outer);
    XUnionRectWithRegion(&cut_rectangle, cut, cut);
    XSubtractRegion(outer, cut, result);
    const bool region_ok =
        XRectInRegion(result, 0, 0, 2, 2) == RectangleIn &&
        XRectInRegion(result, 7, 7, 2, 2) == RectangleOut;
    XDestroyRegion(result);
    XDestroyRegion(cut);
    XDestroyRegion(outer);
    if (!region_ok)
        return fail(display, "region subtraction failed", 3);

    XImage *callbacks = XCreateImage(
        display, nullptr, 1, XYBitmap, 0, nullptr, 8, 8, 8, 1);
    if (callbacks == nullptr || !_XInitImageFuncPtrs(callbacks) ||
        callbacks->f.put_pixel == nullptr || callbacks->f.get_pixel == nullptr) {
        if (callbacks != nullptr)
            XDestroyImage(callbacks);
        return fail(display, "XImage callbacks were not initialized", 4);
    }
    XDestroyImage(callbacks);

    Screen *screen = DefaultScreenOfDisplay(display);
    const Window window = XCreateSimpleWindow(
        display, screen->root, 0, 0, 64, 48, 0,
        screen->black_pixel, screen->black_pixel);
    XMapWindow(display, window);
    XGCValues values{};
    values.foreground = 0x00ff0000UL;
    GC gc = XCreateGC(display, window, GCForeground, &values);
    XRectangle clip{12, 8, 24, 20};
    XSetClipRectangles(display, gc, 0, 0, &clip, 1, Unsorted);
    XRectangle fill{0, 0, 64, 48};
    XFillRectangles(display, window, gc, &fill, 1);

    Window colormap_window = window;
    Window *colormap_windows = nullptr;
    int colormap_count = 0;
    XSetWMColormapWindows(display, window, &colormap_window, 1);
    const bool property_ok = XGetWMColormapWindows(
        display, window, &colormap_windows, &colormap_count);
    const bool property_value_ok = property_ok && colormap_count == 1 &&
        colormap_windows != nullptr && colormap_windows[0] == window;
    XFree(colormap_windows);

    XSync(display, False);
    XImage *image = XGetImage(
        display, window, 0, 0, 64, 48, AllPlanes, ZPixmap);
    const bool clip_ok = image != nullptr &&
        XGetPixel(image, 18, 14) == 0x00ff0000UL &&
        XGetPixel(image, 2, 2) == screen->black_pixel;
    if (image != nullptr)
        XDestroyImage(image);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    if (!property_value_ok) {
        std::cerr << "WM_COLORMAP_WINDOWS round trip failed\n";
        return 5;
    }
    if (!clip_ok) {
        std::cerr << "Xlib drawing clip failed\n";
        return 6;
    }
    return 0;
}
