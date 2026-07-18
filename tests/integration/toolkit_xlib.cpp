#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>

extern "C" int _XInitImageFuncPtrs(XImage *image);

namespace {

int protocol_error_count = 0;
XErrorEvent last_protocol_error{};

int record_protocol_error(Display *, XErrorEvent *error)
{
    ++protocol_error_count;
    if (error != nullptr)
        last_protocol_error = *error;
    return 0;
}

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

    const auto color_matches = [display](
        const char *name, unsigned short red, unsigned short green,
        unsigned short blue) {
        XColor parsed{};
        return XParseColor(
                   display, DefaultColormap(display, 0), name, &parsed) &&
            parsed.red == red && parsed.green == green &&
            parsed.blue == blue;
    };
    if (!color_matches("Steel Blue", 0x4646, 0x8282, 0xb4b4) ||
        !color_matches("orchid4", 0x8b8b, 0x4747, 0x8989) ||
        !color_matches("Rebecca Purple", 0x6666, 0x3333, 0x9999) ||
        !color_matches("aqua", 0x0000, 0xffff, 0xffff) ||
        !color_matches("#f08", 0xf000, 0x0000, 0x8000) ||
        !color_matches("rgb:f/00/80", 0xffff, 0x0000, 0x8080) ||
        !color_matches("rgbi:1/0.5/0", 0xffff, 0x8000, 0x0000) ||
        XParseColor(
            display, DefaultColormap(display, 0), "not-a-color", &color)) {
        return fail(display, "named or functional color parsing failed", 2);
    }

    const KeyCode a_keycode = XKeysymToKeycode(display, XK_a);
    XKeyEvent key_event{};
    key_event.display = display;
    key_event.keycode = a_keycode;
    char key_text[8]{};
    KeySym key_symbol = NoSymbol;
    key_event.state = 0;
    const bool lowercase_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == 'a' && key_symbol == XK_a;
    key_event.state = ShiftMask;
    const bool shifted_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == 'A' && key_symbol == XK_A;
    key_event.state = LockMask;
    const bool locked_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == 'A' && key_symbol == XK_A;
    key_event.state = ShiftMask | LockMask;
    const bool shifted_locked_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == 'a' && key_symbol == XK_a;
    key_event.state = ControlMask;
    const bool control_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == '\x01' && key_symbol == XK_a;
    const KeyCode keypad_zero_keycode = XKeysymToKeycode(display, XK_KP_0);
    key_event.keycode = keypad_zero_keycode;
    key_event.state = Mod2Mask;
    const bool keypad_num_lock_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 1 &&
        key_text[0] == '0' && key_symbol == XK_KP_0;
    key_event.state = Mod2Mask | ShiftMask;
    const bool keypad_shift_ok =
        XLookupString(&key_event, key_text, sizeof(key_text), &key_symbol,
                      nullptr) == 0 &&
        key_symbol == XK_KP_Insert;
    if (a_keycode == 0 || !lowercase_ok || !shifted_ok || !locked_ok ||
        !shifted_locked_ok || !control_ok || keypad_zero_keycode == 0 ||
        !keypad_num_lock_ok || !keypad_shift_ok) {
        return fail(display, "modifier-aware key translation failed", 2);
    }

    XIM input_method = XOpenIM(display, nullptr, nullptr, nullptr);
    XIMStyles *styles = nullptr;
    const XIMStyle fallback_style =
        XIMPreeditNothing | XIMStatusNothing;
    if (input_method == nullptr ||
        XGetIMValues(input_method, XNQueryInputStyle, &styles, nullptr) !=
            nullptr ||
        styles == nullptr || styles->count_styles != 1 ||
        styles->supported_styles[0] != fallback_style) {
        XFree(styles);
        if (input_method != nullptr)
            XCloseIM(input_method);
        return fail(display, "input-method style discovery failed", 2);
    }
    XFree(styles);
    const Window input_window = DefaultRootWindow(display);
    XIC input_context = XCreateIC(
        input_method, XNInputStyle, fallback_style, XNClientWindow,
        input_window, XNFocusWindow, input_window, nullptr);
    XIMStyle reported_style = 0;
    Window reported_client = None;
    Window reported_focus = None;
    long filter_events = -1;
    const bool input_context_ok = input_context != nullptr &&
        XGetICValues(
            input_context, XNInputStyle, &reported_style, XNClientWindow,
            &reported_client, XNFocusWindow, &reported_focus,
            XNFilterEvents, &filter_events, nullptr) == nullptr &&
        reported_style == fallback_style && reported_client == input_window &&
        reported_focus == input_window && filter_events == 0;
    Status lookup_status = XLookupNone;
    key_event.keycode = a_keycode;
    key_event.state = 0;
    const bool overflow_ok = input_context != nullptr &&
        Xutf8LookupString(
            input_context, &key_event, nullptr, 0, &key_symbol,
            &lookup_status) == 1 &&
        lookup_status == XBufferOverflow && key_symbol == XK_a;
    if (!input_context_ok || !overflow_ok) {
        if (input_context != nullptr)
            XDestroyIC(input_context);
        XCloseIM(input_method);
        return fail(display, "input-context compatibility failed", 2);
    }
    XSetICFocus(input_context);
    XUnsetICFocus(input_context);
    XDestroyIC(input_context);
    XCloseIM(input_method);

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

    Region adjacent = XCreateRegion();
    XRectangle adjacent_left{0, 0, 5, 10};
    XRectangle adjacent_right{5, 0, 5, 10};
    XUnionRectWithRegion(&adjacent_left, adjacent, adjacent);
    XUnionRectWithRegion(&adjacent_right, adjacent, adjacent);
    const bool adjacent_region_ok =
        XRectInRegion(adjacent, 0, 0, 10, 10) == RectangleIn;
    XDestroyRegion(adjacent);
    if (!adjacent_region_ok)
        return fail(display, "region union coverage failed", 4);

    XImage *callbacks = XCreateImage(
        display, nullptr, 1, XYBitmap, 0, nullptr, 8, 8, 8, 1);
    if (callbacks == nullptr || !_XInitImageFuncPtrs(callbacks) ||
        callbacks->f.put_pixel == nullptr || callbacks->f.get_pixel == nullptr) {
        if (callbacks != nullptr)
            XDestroyImage(callbacks);
        return fail(display, "XImage callbacks were not initialized", 5);
    }
    XDestroyImage(callbacks);

    XImage *planar = XCreateImage(
        display, nullptr, 4, XYPixmap, 1, nullptr, 3, 2, 8, 0);
    if (planar != nullptr) {
        planar->data = static_cast<char *>(std::calloc(
            static_cast<std::size_t>(planar->bytes_per_line) *
                planar->height,
            static_cast<std::size_t>(planar->depth)));
    }
    const bool planar_ok = planar != nullptr && planar->data != nullptr &&
        XPutPixel(planar, 0, 0, 0x0a) &&
        XPutPixel(planar, 2, 1, 0x05) &&
        XGetPixel(planar, 0, 0) == 0x0a &&
        XGetPixel(planar, 2, 1) == 0x05 &&
        XGetPixel(planar, 1, 0) == 0;
    if (planar != nullptr)
        XDestroyImage(planar);
    if (!planar_ok)
        return fail(display, "XYPixmap plane handling failed", 5);

    Screen *screen = DefaultScreenOfDisplay(display);

    XSelectInput(
        display, screen->root,
        PropertyChangeMask | SubstructureNotifyMask);
    const Window structure_window = XCreateSimpleWindow(
        display, screen->root, 0, 0, 8, 8, 0,
        screen->black_pixel, screen->black_pixel);
    XMapWindow(display, structure_window);
    XEvent structure_event{};
    if (XWindowEvent(
            display, screen->root, SubstructureNotifyMask,
            &structure_event) != 0 || structure_event.type != MapNotify ||
        structure_event.xmap.event != screen->root ||
        structure_event.xmap.window != structure_window) {
        return fail(display, "substructure event matching failed", 6);
    }

    const Atom event_property = XInternAtom(
        display, "XMIN_TOOLKIT_EVENT_ORDER", False);
    const Atom event_message = XInternAtom(
        display, "XMIN_TOOLKIT_QUEUED_MESSAGE", False);
    XEvent message{};
    message.xclient.type = ClientMessage;
    message.xclient.window = screen->root;
    message.xclient.message_type = event_message;
    message.xclient.format = 32;
    message.xclient.data.l[0] = 0x584d494eL;
    const unsigned char property_value = 0x5a;
    XSendEvent(display, structure_window, False, NoEventMask, &message);
    XChangeProperty(
        display, screen->root, event_property, XA_INTEGER, 8,
        PropModeReplace, &property_value, 1);
    XSync(display, False);
    const int queued_before_filter = XPending(display);
    XEvent property_event{};
    const int property_wait = XWindowEvent(
        display, screen->root, PropertyChangeMask, &property_event);
    const int retained_events = QLength(display);
    if (queued_before_filter != 2 || property_wait != 0 ||
        property_event.type != PropertyNotify ||
        property_event.xproperty.atom != event_property ||
        retained_events != 1) {
        std::cerr << "queued before filter " << queued_before_filter
                  << ", property wait " << property_wait << ", type "
                  << property_event.type << ", atom "
                  << property_event.xproperty.atom << ", expected atom "
                  << event_property << ", retained " << retained_events
                  << '\n';
        return fail(display, "filtered event queue stalled or lost order", 7);
    }
    XEvent queued_message{};
    if (XNextEvent(display, &queued_message) != 0 ||
        queued_message.type != ClientMessage ||
        queued_message.xclient.message_type != event_message ||
        queued_message.xclient.data.l[0] != 0x584d494eL ||
        QLength(display) != 0) {
        return fail(display, "filtered event was not retained", 8);
    }

    XSendEvent(display, screen->root, False, NoEventMask, &message);
    XSync(display, True);
    if (QLength(display) != 0)
        return fail(display, "XSync did not discard queued events", 9);

    XErrorHandler previous_handler = XSetErrorHandler(record_protocol_error);
    if (previous_handler == nullptr)
        return fail(display, "default X error handler was not restorable", 10);
    Cursor cursor = XCreateFontCursor(display, XC_left_ptr);
    XColor cursor_foreground{};
    cursor_foreground.red = 0xffff;
    cursor_foreground.flags = DoRed | DoGreen | DoBlue;
    XColor cursor_background{};
    cursor_background.green = 0xffff;
    cursor_background.flags = DoRed | DoGreen | DoBlue;
    if (cursor == None ||
        !XRecolorCursor(
            display, cursor, &cursor_foreground, &cursor_background)) {
        if (cursor != None)
            XFreeCursor(display, cursor);
        XSetErrorHandler(previous_handler);
        return fail(display, "cursor recoloring failed", 10);
    }
    XFreeCursor(display, cursor);
    XSync(display, False);
    if (protocol_error_count != 0) {
        XSetErrorHandler(previous_handler);
        return fail(display, "cursor recoloring raised a protocol error", 10);
    }
    XDestroyWindow(display, static_cast<Window>(0xffffffffUL));
    XSync(display, False);
    XSetErrorHandler(previous_handler);
    if (protocol_error_count != 1 ||
        last_protocol_error.error_code != BadWindow ||
        last_protocol_error.request_code != X_DestroyWindow ||
        last_protocol_error.resourceid != 0xffffffffUL) {
        return fail(display, "asynchronous X error dispatch failed", 11);
    }

    char pressed_keys[32];
    std::fill(std::begin(pressed_keys), std::end(pressed_keys), '\x7f');
    if (!XQueryKeymap(display, pressed_keys) ||
        !std::all_of(
            std::begin(pressed_keys), std::end(pressed_keys),
            [](char value) { return value == 0; })) {
        return fail(display, "keyboard state query failed", 12);
    }

    XDeleteProperty(display, screen->root, event_property);
    XSelectInput(display, screen->root, NoEventMask);
    XDestroyWindow(display, structure_window);
    const Window window = XCreateSimpleWindow(
        display, screen->root, 0, 0, 64, 48, 0,
        screen->black_pixel, screen->black_pixel);
    XMapWindow(display, window);

    char title_text[] = "Qt \xce\xa9";
    char *title_list[]{title_text};
    XTextProperty title_property{};
    const int title_conversion = XmbTextListToTextProperty(
        display, title_list, 1, XStdICCTextStyle, &title_property);
    if (title_conversion == 0)
        XSetWMName(display, window, &title_property);
    XFree(title_property.value);
    const Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    Atom title_type = None;
    int title_format = 0;
    unsigned long title_items = 0;
    unsigned long title_after = 0;
    unsigned char *stored_title = nullptr;
    const int title_status = XGetWindowProperty(
        display, window, XA_WM_NAME, 0, 64, False, AnyPropertyType,
        &title_type, &title_format, &title_items, &title_after,
        &stored_title);
    const bool title_ok = title_conversion == 0 && title_status == Success &&
        title_type == utf8_string && title_format == 8 &&
        title_items == sizeof(title_text) - 1 && title_after == 0 &&
        stored_title != nullptr &&
        std::equal(stored_title, stored_title + title_items,
                   reinterpret_cast<unsigned char *>(title_text));
    XFree(stored_title);

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

    if (!property_value_ok || !title_ok) {
        std::cerr << "ICCCM text property round trip failed\n";
        return 13;
    }
    if (!clip_ok) {
        std::cerr << "Xlib drawing clip failed\n";
        return 14;
    }
    return 0;
}
