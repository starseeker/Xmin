/* Focused Xlib event, keyboard, and input-method facade for FLTK/Tk. */
#include "xlib_internal.hpp"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

struct _XIM { Display *display = nullptr; };
struct _XIC {
    XIM input_method = nullptr;
    XIMStyle input_style = XIMPreeditNothing | XIMStatusNothing;
    Window client_window = None;
    Window focus_window = None;
    long filter_events = 0;
    bool focused = false;
};
struct _XOC { int unused = 0; };

namespace {

using EventQueue = std::deque<xcb_generic_event_t *>;
using PutbackQueue = std::deque<XEvent>;
std::mutex queue_mutex;
std::unordered_map<Display *, EventQueue> queues;
std::unordered_map<Display *, PutbackQueue> putback_queues;

void update_queue_length(Display *display) noexcept
{
    if (display == nullptr)
        return;
    const auto found_raw = queues.find(display);
    const auto found_putbacks = putback_queues.find(display);
    const std::size_t raw = found_raw == queues.end()
        ? 0 : found_raw->second.size();
    const std::size_t putbacks = found_putbacks == putback_queues.end()
        ? 0 : found_putbacks->second.size();
    const auto maximum =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    reinterpret_cast<_XPrivDisplay>(display)->qlen = static_cast<int>(
        std::min(raw + putbacks, maximum));
}

long xevent_mask(const XEvent &event)
{
    switch (event.type) {
    case KeyPress: return KeyPressMask;
    case KeyRelease: return KeyReleaseMask;
    case ButtonPress: return ButtonPressMask;
    case ButtonRelease: return ButtonReleaseMask;
    case MotionNotify: {
        long mask = PointerMotionMask;
        const struct { unsigned int state; long motion; } buttons[]{
            {Button1Mask, Button1MotionMask}, {Button2Mask, Button2MotionMask},
            {Button3Mask, Button3MotionMask}, {Button4Mask, Button4MotionMask},
            {Button5Mask, Button5MotionMask}};
        for (const auto &button : buttons) {
            if ((event.xmotion.state & button.state) != 0)
                mask |= ButtonMotionMask | button.motion;
        }
        return mask;
    }
    case EnterNotify: return EnterWindowMask;
    case LeaveNotify: return LeaveWindowMask;
    case FocusIn:
    case FocusOut: return FocusChangeMask;
    case Expose: return ExposureMask;
    case VisibilityNotify: return VisibilityChangeMask;
    case CreateNotify: return SubstructureNotifyMask;
    case DestroyNotify:
        return event.xdestroywindow.event == event.xdestroywindow.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case UnmapNotify:
        return event.xunmap.event == event.xunmap.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case MapNotify:
        return event.xmap.event == event.xmap.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case ReparentNotify:
        return event.xreparent.event == event.xreparent.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case ConfigureNotify:
        return event.xconfigure.event == event.xconfigure.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case GravityNotify:
        return event.xgravity.event == event.xgravity.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case CirculateNotify:
        return event.xcirculate.event == event.xcirculate.window
            ? StructureNotifyMask : SubstructureNotifyMask;
    case ResizeRequest: return ResizeRedirectMask;
    case MapRequest:
    case ConfigureRequest:
    case CirculateRequest: return SubstructureRedirectMask;
    case PropertyNotify: return PropertyChangeMask;
    case ColormapNotify: return ColormapChangeMask;
    default: return 0;
    }
}

Window xevent_window(const XEvent &event)
{
    switch (event.type) {
    case CreateNotify: return event.xcreatewindow.parent;
    case DestroyNotify: return event.xdestroywindow.event;
    case UnmapNotify: return event.xunmap.event;
    case MapNotify: return event.xmap.event;
    case ReparentNotify: return event.xreparent.event;
    case ConfigureNotify: return event.xconfigure.event;
    case GravityNotify: return event.xgravity.event;
    case CirculateNotify: return event.xcirculate.event;
    case ResizeRequest: return event.xresizerequest.window;
    case MapRequest: return event.xmaprequest.parent;
    case ConfigureRequest: return event.xconfigurerequest.parent;
    case CirculateRequest: return event.xcirculaterequest.parent;
    default: return event.xany.window;
    }
}

xcb_connection_t *connection(Display *display)
{
    return xmin::client::x11::xlib_connection(display);
}

int event_type(const xcb_generic_event_t *event)
{
    return event == nullptr ? 0 : event->response_type & 0x7f;
}

Window event_window(const xcb_generic_event_t *generic)
{
    if (generic == nullptr)
        return None;
    switch (event_type(generic)) {
    case KeyPress:
    case KeyRelease:
        return reinterpret_cast<const xcb_key_press_event_t *>(generic)->event;
    case ButtonPress:
    case ButtonRelease:
        return reinterpret_cast<const xcb_button_press_event_t *>(generic)->event;
    case MotionNotify:
        return reinterpret_cast<const xcb_motion_notify_event_t *>(generic)->event;
    case EnterNotify:
    case LeaveNotify:
        return reinterpret_cast<const xcb_enter_notify_event_t *>(generic)->event;
    case FocusIn:
    case FocusOut:
        return reinterpret_cast<const xcb_focus_in_event_t *>(generic)->event;
    case Expose:
        return reinterpret_cast<const xcb_expose_event_t *>(generic)->window;
    case VisibilityNotify:
        return reinterpret_cast<
            const xcb_visibility_notify_event_t *>(generic)->window;
    case CreateNotify:
        return reinterpret_cast<const xcb_create_notify_event_t *>(generic)->parent;
    case ConfigureNotify:
        return reinterpret_cast<const xcb_configure_notify_event_t *>(generic)->event;
    case MapNotify:
        return reinterpret_cast<const xcb_map_notify_event_t *>(generic)->event;
    case UnmapNotify:
        return reinterpret_cast<const xcb_unmap_notify_event_t *>(generic)->event;
    case DestroyNotify:
        return reinterpret_cast<const xcb_destroy_notify_event_t *>(generic)->event;
    case ReparentNotify:
        return reinterpret_cast<const xcb_reparent_notify_event_t *>(generic)->event;
    case GravityNotify:
        return reinterpret_cast<const xcb_gravity_notify_event_t *>(generic)->event;
    case CirculateNotify:
        return reinterpret_cast<const xcb_circulate_notify_event_t *>(generic)->event;
    case ResizeRequest:
        return reinterpret_cast<const xcb_resize_request_event_t *>(generic)->window;
    case MapRequest:
        return reinterpret_cast<const xcb_map_request_event_t *>(generic)->parent;
    case ConfigureRequest:
        return reinterpret_cast<
            const xcb_configure_request_event_t *>(generic)->parent;
    case CirculateRequest:
        return reinterpret_cast<
            const xcb_circulate_request_event_t *>(generic)->event;
    case PropertyNotify:
        return reinterpret_cast<const xcb_property_notify_event_t *>(generic)->window;
    case ColormapNotify:
        return reinterpret_cast<
            const xcb_colormap_notify_event_t *>(generic)->window;
    case ClientMessage:
        return reinterpret_cast<const xcb_client_message_event_t *>(generic)->window;
    case SelectionClear:
        return reinterpret_cast<const xcb_selection_clear_event_t *>(generic)->owner;
    case SelectionRequest:
        return reinterpret_cast<const xcb_selection_request_event_t *>(generic)->owner;
    case SelectionNotify:
        return reinterpret_cast<const xcb_selection_notify_event_t *>(generic)->requestor;
    default:
        return None;
    }
}

template <typename Source, typename Destination>
void common_input_event(
    Display *display, const Source &source, Destination &destination)
{
    destination.type = source.response_type & 0x7f;
    destination.serial = source.sequence;
    destination.send_event = (source.response_type & 0x80) != 0;
    destination.display = display;
    destination.window = source.event;
    destination.root = source.root;
    destination.subwindow = source.child;
    destination.time = source.time;
    destination.x = source.event_x;
    destination.y = source.event_y;
    destination.x_root = source.root_x;
    destination.y_root = source.root_y;
    destination.state = source.state;
}

bool convert(Display *display, const xcb_generic_event_t *generic, XEvent *event)
{
    if (generic == nullptr || event == nullptr)
        return false;
    std::memset(event, 0, sizeof(*event));
    const int type = event_type(generic);
    event->type = type;
    event->xany.serial = generic->sequence;
    event->xany.send_event = (generic->response_type & 0x80) != 0;
    event->xany.display = display;
    switch (type) {
    case KeyPress:
    case KeyRelease: {
        const auto &source = *reinterpret_cast<const xcb_key_press_event_t *>(generic);
        common_input_event(display, source, event->xkey);
        event->xkey.same_screen = source.same_screen;
        event->xkey.keycode = source.detail;
        break;
    }
    case ButtonPress:
    case ButtonRelease: {
        const auto &source = *reinterpret_cast<const xcb_button_press_event_t *>(generic);
        common_input_event(display, source, event->xbutton);
        event->xbutton.same_screen = source.same_screen;
        event->xbutton.button = source.detail;
        break;
    }
    case MotionNotify: {
        const auto &source = *reinterpret_cast<const xcb_motion_notify_event_t *>(generic);
        common_input_event(display, source, event->xmotion);
        event->xmotion.same_screen = source.same_screen;
        event->xmotion.is_hint = static_cast<char>(source.detail);
        break;
    }
    case EnterNotify:
    case LeaveNotify: {
        const auto &source = *reinterpret_cast<const xcb_enter_notify_event_t *>(generic);
        common_input_event(display, source, event->xcrossing);
        event->xcrossing.mode = source.mode;
        event->xcrossing.detail = source.detail;
        event->xcrossing.same_screen = (source.same_screen_focus & 1U) != 0;
        event->xcrossing.focus = (source.same_screen_focus & 2U) != 0;
        break;
    }
    case FocusIn:
    case FocusOut: {
        const auto &source = *reinterpret_cast<const xcb_focus_in_event_t *>(generic);
        event->xfocus.window = source.event;
        event->xfocus.mode = source.mode;
        event->xfocus.detail = source.detail;
        break;
    }
    case Expose: {
        const auto &source = *reinterpret_cast<const xcb_expose_event_t *>(generic);
        event->xexpose.window = source.window;
        event->xexpose.x = source.x;
        event->xexpose.y = source.y;
        event->xexpose.width = source.width;
        event->xexpose.height = source.height;
        event->xexpose.count = source.count;
        break;
    }
    case VisibilityNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_visibility_notify_event_t *>(generic);
        event->xvisibility.window = source.window;
        event->xvisibility.state = source.state;
        break;
    }
    case CreateNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_create_notify_event_t *>(generic);
        event->xcreatewindow.parent = source.parent;
        event->xcreatewindow.window = source.window;
        event->xcreatewindow.x = source.x;
        event->xcreatewindow.y = source.y;
        event->xcreatewindow.width = source.width;
        event->xcreatewindow.height = source.height;
        event->xcreatewindow.border_width = source.border_width;
        event->xcreatewindow.override_redirect = source.override_redirect;
        break;
    }
    case ConfigureNotify: {
        const auto &source = *reinterpret_cast<const xcb_configure_notify_event_t *>(generic);
        event->xconfigure.event = source.event;
        event->xconfigure.window = source.window;
        event->xconfigure.x = source.x;
        event->xconfigure.y = source.y;
        event->xconfigure.width = source.width;
        event->xconfigure.height = source.height;
        event->xconfigure.border_width = source.border_width;
        event->xconfigure.above = source.above_sibling;
        event->xconfigure.override_redirect = source.override_redirect;
        break;
    }
    case MapNotify: {
        const auto &source = *reinterpret_cast<const xcb_map_notify_event_t *>(generic);
        event->xmap.event = source.event;
        event->xmap.window = source.window;
        event->xmap.override_redirect = source.override_redirect;
        break;
    }
    case UnmapNotify: {
        const auto &source = *reinterpret_cast<const xcb_unmap_notify_event_t *>(generic);
        event->xunmap.event = source.event;
        event->xunmap.window = source.window;
        event->xunmap.from_configure = source.from_configure;
        break;
    }
    case DestroyNotify: {
        const auto &source = *reinterpret_cast<const xcb_destroy_notify_event_t *>(generic);
        event->xdestroywindow.event = source.event;
        event->xdestroywindow.window = source.window;
        break;
    }
    case ReparentNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_reparent_notify_event_t *>(generic);
        event->xreparent.event = source.event;
        event->xreparent.window = source.window;
        event->xreparent.parent = source.parent;
        event->xreparent.x = source.x;
        event->xreparent.y = source.y;
        event->xreparent.override_redirect = source.override_redirect;
        break;
    }
    case GravityNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_gravity_notify_event_t *>(generic);
        event->xgravity.event = source.event;
        event->xgravity.window = source.window;
        event->xgravity.x = source.x;
        event->xgravity.y = source.y;
        break;
    }
    case CirculateNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_circulate_notify_event_t *>(generic);
        event->xcirculate.event = source.event;
        event->xcirculate.window = source.window;
        event->xcirculate.place = source.place;
        break;
    }
    case ResizeRequest: {
        const auto &source = *reinterpret_cast<
            const xcb_resize_request_event_t *>(generic);
        event->xresizerequest.window = source.window;
        event->xresizerequest.width = source.width;
        event->xresizerequest.height = source.height;
        break;
    }
    case MapRequest: {
        const auto &source = *reinterpret_cast<
            const xcb_map_request_event_t *>(generic);
        event->xmaprequest.parent = source.parent;
        event->xmaprequest.window = source.window;
        break;
    }
    case ConfigureRequest: {
        const auto &source = *reinterpret_cast<
            const xcb_configure_request_event_t *>(generic);
        event->xconfigurerequest.parent = source.parent;
        event->xconfigurerequest.window = source.window;
        event->xconfigurerequest.x = source.x;
        event->xconfigurerequest.y = source.y;
        event->xconfigurerequest.width = source.width;
        event->xconfigurerequest.height = source.height;
        event->xconfigurerequest.border_width = source.border_width;
        event->xconfigurerequest.above = source.sibling;
        event->xconfigurerequest.detail = source.stack_mode;
        event->xconfigurerequest.value_mask = source.value_mask;
        break;
    }
    case CirculateRequest: {
        const auto &source = *reinterpret_cast<
            const xcb_circulate_request_event_t *>(generic);
        event->xcirculaterequest.parent = source.event;
        event->xcirculaterequest.window = source.window;
        event->xcirculaterequest.place = source.place;
        break;
    }
    case PropertyNotify: {
        const auto &source = *reinterpret_cast<const xcb_property_notify_event_t *>(generic);
        event->xproperty.window = source.window;
        event->xproperty.atom = source.atom;
        event->xproperty.time = source.time;
        event->xproperty.state = source.state;
        break;
    }
    case ColormapNotify: {
        const auto &source = *reinterpret_cast<
            const xcb_colormap_notify_event_t *>(generic);
        event->xcolormap.window = source.window;
        event->xcolormap.colormap = source.colormap;
        event->xcolormap.c_new = source._new;
        event->xcolormap.state = source.state;
        break;
    }
    case ClientMessage: {
        const auto &source = *reinterpret_cast<const xcb_client_message_event_t *>(generic);
        event->xclient.window = source.window;
        event->xclient.message_type = source.type;
        event->xclient.format = source.format;
        if (source.format == 8)
            std::memcpy(event->xclient.data.b, source.data.data8, 20);
        else if (source.format == 16)
            std::copy(source.data.data16, source.data.data16 + 10,
                      event->xclient.data.s);
        else
            std::copy(source.data.data32, source.data.data32 + 5,
                      event->xclient.data.l);
        break;
    }
    case SelectionClear: {
        const auto &source = *reinterpret_cast<const xcb_selection_clear_event_t *>(generic);
        event->xselectionclear.window = source.owner;
        event->xselectionclear.selection = source.selection;
        event->xselectionclear.time = source.time;
        break;
    }
    case SelectionRequest: {
        const auto &source = *reinterpret_cast<const xcb_selection_request_event_t *>(generic);
        event->xselectionrequest.owner = source.owner;
        event->xselectionrequest.requestor = source.requestor;
        event->xselectionrequest.selection = source.selection;
        event->xselectionrequest.target = source.target;
        event->xselectionrequest.property = source.property;
        event->xselectionrequest.time = source.time;
        break;
    }
    case SelectionNotify: {
        const auto &source = *reinterpret_cast<const xcb_selection_notify_event_t *>(generic);
        event->xselection.requestor = source.requestor;
        event->xselection.selection = source.selection;
        event->xselection.target = source.target;
        event->xselection.property = source.property;
        event->xselection.time = source.time;
        break;
    }
    case MappingNotify: {
        const auto &source = *reinterpret_cast<const xcb_mapping_notify_event_t *>(generic);
        event->xmapping.window = None;
        event->xmapping.request = source.request;
        event->xmapping.first_keycode = source.first_keycode;
        event->xmapping.count = source.count;
        break;
    }
    default:
        event->xany.window = event_window(generic);
        break;
    }
    return true;
}

void pump(Display *display)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return;
    while (auto *event = xcb_poll_for_event(xcb)) {
        if (event->response_type == 0) {
            xmin::client::x11::xlib_dispatch_error(
                display,
                reinterpret_cast<const xcb_generic_error_t *>(event));
            std::free(event);
            continue;
        }
        try {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto &queue = queues[display];
            queue.push_back(event);
            update_queue_length(display);
        }
        catch (...) {
            std::free(event);
            xmin::client::x11::xlib_dispatch_io_error(display);
            return;
        }
    }
    if (xcb_connection_has_error(xcb) != 0)
        xmin::client::x11::xlib_dispatch_io_error(display);
}

xcb_generic_event_t *next_raw(Display *display, bool block)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const auto found = queues.find(display);
        if (found != queues.end() && !found->second.empty()) {
            auto *result = found->second.front();
            found->second.pop_front();
            update_queue_length(display);
            return result;
        }
    }
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return nullptr;
    if (block)
        XFlush(display);
    for (;;) {
        auto *result = block
            ? xcb_wait_for_event(xcb)
            : xcb_poll_for_event(xcb);
        if (result == nullptr) {
            if (xcb_connection_has_error(xcb) != 0)
                xmin::client::x11::xlib_dispatch_io_error(display);
            return nullptr;
        }
        if (result->response_type != 0)
            return result;
        xmin::client::x11::xlib_dispatch_error(
            display,
            reinterpret_cast<const xcb_generic_error_t *>(result));
        std::free(result);
    }
}

bool enqueue_raw(Display *display, xcb_generic_event_t *event)
{
    try {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto &queue = queues[display];
        queue.push_back(event);
        update_queue_length(display);
        return true;
    }
    catch (...) {
        return false;
    }
}

template <typename Predicate>
bool take_matching_event(
    Display *display, XEvent *result, Predicate predicate, bool pump_first)
{
    if (display == nullptr || result == nullptr)
        return false;
    if (pump_first)
        pump(display);

    std::size_t putback_count = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const auto found = putback_queues.find(display);
        if (found != putback_queues.end())
            putback_count = found->second.size();
    }
    for (std::size_t index = 0; index < putback_count; ++index) {
        XEvent candidate{};
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto &queue = putback_queues[display];
            if (queue.empty())
                break;
            candidate = queue.front();
            queue.pop_front();
            update_queue_length(display);
        }
        if (predicate(candidate)) {
            *result = candidate;
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            putback_queues[display].push_back(candidate);
            update_queue_length(display);
        }
    }

    std::size_t raw_count = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const auto found = queues.find(display);
        if (found != queues.end())
            raw_count = found->second.size();
    }
    for (std::size_t index = 0; index < raw_count; ++index) {
        xcb_generic_event_t *raw = nullptr;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto &queue = queues[display];
            if (queue.empty())
                break;
            raw = queue.front();
            queue.pop_front();
            update_queue_length(display);
        }
        XEvent candidate{};
        const bool converted = convert(display, raw, &candidate);
        if (converted && predicate(candidate)) {
            std::free(raw);
            *result = candidate;
            return true;
        }
        if (!enqueue_raw(display, raw)) {
            std::free(raw);
            xmin::client::x11::xlib_dispatch_io_error(display);
            return false;
        }
    }
    return false;
}

template <typename Predicate>
int wait_matching_event(
    Display *display, XEvent *result, Predicate predicate)
{
    for (;;) {
        if (take_matching_event(display, result, predicate, true))
            return 0;
        auto *xcb = connection(display);
        if (xcb == nullptr)
            return -1;
        XFlush(display);
        auto *raw = xcb_wait_for_event(xcb);
        while (raw != nullptr && raw->response_type == 0) {
            xmin::client::x11::xlib_dispatch_error(
                display, reinterpret_cast<const xcb_generic_error_t *>(raw));
            std::free(raw);
            raw = xcb_wait_for_event(xcb);
        }
        if (raw == nullptr) {
            if (xcb_connection_has_error(xcb) != 0)
                xmin::client::x11::xlib_dispatch_io_error(display);
            return -1;
        }
        XEvent candidate{};
        if (convert(display, raw, &candidate) && predicate(candidate)) {
            std::free(raw);
            *result = candidate;
            return 0;
        }
        if (!enqueue_raw(display, raw)) {
            std::free(raw);
            xmin::client::x11::xlib_dispatch_io_error(display);
            return -1;
        }
    }
}

KeySym keycode_to_keysym(Display *display, KeyCode code, int column)
{
    auto *xcb = connection(display);
    const auto storage = reinterpret_cast<_XPrivDisplay>(display);
    if (xcb == nullptr || storage == nullptr || code < storage->min_keycode ||
        code > storage->max_keycode)
        return NoSymbol;
    std::unique_ptr<xcb_get_keyboard_mapping_reply_t, decltype(&std::free)> reply(
        xcb_get_keyboard_mapping_reply(
            xcb, xcb_get_keyboard_mapping(xcb, code, 1), nullptr), std::free);
    if (reply == nullptr || reply->keysyms_per_keycode == 0)
        return NoSymbol;
    const int selected = std::clamp(
        column, 0, static_cast<int>(reply->keysyms_per_keycode) - 1);
    return xcb_get_keyboard_mapping_keysyms(reply.get())[selected];
}

KeySym event_keysym(const XKeyEvent *event)
{
    if (event == nullptr)
        return NoSymbol;
    const bool shifted = (event->state & ShiftMask) != 0;
    const KeySym base =
        keycode_to_keysym(event->display, event->keycode, 0);
    const KeySym upper =
        keycode_to_keysym(event->display, event->keycode, 1);
    const bool keypad_pair = base >= XK_KP_Space && base <= XK_KP_Delete &&
        upper >= XK_KP_0 && upper <= XK_KP_9;
    if (keypad_pair) {
        const bool num_lock = (event->state & Mod2Mask) != 0;
        return num_lock != shifted ? upper : base;
    }
    if ((event->state & LockMask) != 0 && base >= XK_a && base <= XK_z &&
        upper >= XK_A && upper <= XK_Z) {
        return shifted ? base : upper;
    }
    return shifted ? upper : base;
}

std::optional<std::uint32_t> keysym_codepoint(
    KeySym symbol, unsigned int state)
{
    std::optional<std::uint32_t> codepoint;
    if ((symbol >= 0x20 && symbol <= 0x7e) ||
        (symbol >= 0xa0 && symbol <= 0xff)) {
        codepoint = static_cast<std::uint32_t>(symbol);
    }
    else if ((symbol & 0xff000000UL) == 0x01000000UL) {
        codepoint = static_cast<std::uint32_t>(symbol & 0x00ffffffUL);
    }
    else if (symbol >= XK_KP_0 && symbol <= XK_KP_9) {
        codepoint = static_cast<std::uint32_t>('0' + symbol - XK_KP_0);
    }
    else {
        switch (symbol) {
        case XK_BackSpace: codepoint = '\b'; break;
        case XK_Tab:
        case XK_KP_Tab: codepoint = '\t'; break;
        case XK_Return:
        case XK_KP_Enter: codepoint = '\r'; break;
        case XK_Escape: codepoint = 0x1b; break;
        case XK_Delete:
        case XK_KP_Delete: codepoint = 0x7f; break;
        case XK_KP_Space: codepoint = ' '; break;
        case XK_KP_Equal: codepoint = '='; break;
        case XK_KP_Multiply: codepoint = '*'; break;
        case XK_KP_Add: codepoint = '+'; break;
        case XK_KP_Separator: codepoint = ','; break;
        case XK_KP_Subtract: codepoint = '-'; break;
        case XK_KP_Decimal: codepoint = '.'; break;
        case XK_KP_Divide: codepoint = '/'; break;
        default: break;
        }
    }
    if (codepoint && (state & ControlMask) != 0) {
        if ((*codepoint >= '@' && *codepoint <= '_') ||
            (*codepoint >= 'a' && *codepoint <= 'z')) {
            *codepoint &= 0x1fU;
        }
        else if (*codepoint == ' ')
            *codepoint = 0;
    }
    return codepoint;
}

int utf8_length(std::uint32_t codepoint) noexcept
{
    if (codepoint <= 0x7f)
        return 1;
    if (codepoint <= 0x7ff)
        return 2;
    if (codepoint >= 0xd800 && codepoint <= 0xdfff)
        return 0;
    if (codepoint <= 0xffff)
        return 3;
    return codepoint <= 0x10ffff ? 4 : 0;
}

int utf8_encode(std::uint32_t codepoint, char *buffer, int capacity)
{
    const int needed = utf8_length(codepoint);
    if (buffer == nullptr || capacity < needed || needed == 0)
        return 0;
    if (needed == 1) {
        buffer[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (needed == 2) {
        buffer[0] = static_cast<char>(0xc0 | (codepoint >> 6));
        buffer[1] = static_cast<char>(0x80 | (codepoint & 0x3f));
        return 2;
    }
    if (needed == 3) {
        buffer[0] = static_cast<char>(0xe0 | (codepoint >> 12));
        buffer[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        buffer[2] = static_cast<char>(0x80 | (codepoint & 0x3f));
        return 3;
    }
    if (needed == 4) {
        buffer[0] = static_cast<char>(0xf0 | (codepoint >> 18));
        buffer[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        buffer[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        buffer[3] = static_cast<char>(0x80 | (codepoint & 0x3f));
        return 4;
    }
    return 0;
}

struct NamedKeysym {
    const char *name;
    KeySym symbol;
};

constexpr NamedKeysym named_keysyms[]{{"BackSpace", XK_BackSpace},
    {"Tab", XK_Tab}, {"Linefeed", XK_Linefeed}, {"Clear", XK_Clear},
    {"Return", XK_Return}, {"Pause", XK_Pause},
    {"Scroll_Lock", XK_Scroll_Lock}, {"Sys_Req", XK_Sys_Req},
    {"Escape", XK_Escape}, {"Delete", XK_Delete}, {"Home", XK_Home},
    {"Left", XK_Left}, {"Up", XK_Up}, {"Right", XK_Right},
    {"Down", XK_Down}, {"Prior", XK_Prior}, {"Page_Up", XK_Page_Up},
    {"Next", XK_Next}, {"Page_Down", XK_Page_Down}, {"End", XK_End},
    {"Begin", XK_Begin}, {"Select", XK_Select}, {"Print", XK_Print},
    {"Execute", XK_Execute}, {"Insert", XK_Insert}, {"Undo", XK_Undo},
    {"Redo", XK_Redo}, {"Menu", XK_Menu}, {"Find", XK_Find},
    {"Cancel", XK_Cancel}, {"Help", XK_Help}, {"Break", XK_Break},
    {"Mode_switch", XK_Mode_switch}, {"Num_Lock", XK_Num_Lock},
    {"KP_Space", XK_KP_Space}, {"KP_Tab", XK_KP_Tab},
    {"KP_Enter", XK_KP_Enter}, {"KP_Home", XK_KP_Home},
    {"KP_Left", XK_KP_Left}, {"KP_Up", XK_KP_Up},
    {"KP_Right", XK_KP_Right}, {"KP_Down", XK_KP_Down},
    {"KP_Prior", XK_KP_Prior}, {"KP_Next", XK_KP_Next},
    {"KP_End", XK_KP_End}, {"KP_Begin", XK_KP_Begin},
    {"KP_Insert", XK_KP_Insert}, {"KP_Delete", XK_KP_Delete},
    {"KP_Equal", XK_KP_Equal}, {"KP_Multiply", XK_KP_Multiply},
    {"KP_Add", XK_KP_Add}, {"KP_Separator", XK_KP_Separator},
    {"KP_Subtract", XK_KP_Subtract}, {"KP_Decimal", XK_KP_Decimal},
    {"KP_Divide", XK_KP_Divide}, {"Shift_L", XK_Shift_L},
    {"Shift_R", XK_Shift_R}, {"Control_L", XK_Control_L},
    {"Control_R", XK_Control_R}, {"Caps_Lock", XK_Caps_Lock},
    {"Shift_Lock", XK_Shift_Lock}, {"Meta_L", XK_Meta_L},
    {"Meta_R", XK_Meta_R}, {"Alt_L", XK_Alt_L}, {"Alt_R", XK_Alt_R},
    {"Super_L", XK_Super_L}, {"Super_R", XK_Super_R},
    {"Hyper_L", XK_Hyper_L}, {"Hyper_R", XK_Hyper_R},
    {"ISO_Left_Tab", XK_ISO_Left_Tab}, {"space", XK_space},
    {"exclam", XK_exclam}, {"quotedbl", XK_quotedbl},
    {"numbersign", XK_numbersign}, {"dollar", XK_dollar},
    {"percent", XK_percent}, {"ampersand", XK_ampersand},
    {"apostrophe", XK_apostrophe}, {"parenleft", XK_parenleft},
    {"parenright", XK_parenright}, {"asterisk", XK_asterisk},
    {"plus", XK_plus}, {"comma", XK_comma}, {"minus", XK_minus},
    {"period", XK_period}, {"slash", XK_slash}, {"colon", XK_colon},
    {"semicolon", XK_semicolon}, {"less", XK_less}, {"equal", XK_equal},
    {"greater", XK_greater}, {"question", XK_question}, {"at", XK_at},
    {"bracketleft", XK_bracketleft}, {"backslash", XK_backslash},
    {"bracketright", XK_bracketright}, {"asciicircum", XK_asciicircum},
    {"underscore", XK_underscore}, {"grave", XK_grave},
    {"braceleft", XK_braceleft}, {"bar", XK_bar},
    {"braceright", XK_braceright}, {"asciitilde", XK_asciitilde}};

KeySym named_keysym(std::string_view name)
{
    for (const auto &entry : named_keysyms)
        if (name == entry.name)
            return entry.symbol;
    if (name.size() >= 2 && name[0] == 'F') {
        char *end = nullptr;
        const long number = std::strtol(name.data() + 1, &end, 10);
        if (end == name.data() + name.size() && number >= 1 && number <= 35)
            return XK_F1 + static_cast<KeySym>(number - 1);
    }
    if (name.size() == 4 && name.substr(0, 3) == "KP_" &&
        name[3] >= '0' && name[3] <= '9')
        return XK_KP_0 + static_cast<KeySym>(name[3] - '0');
    if (name.size() > 1 && name[0] == 'U') {
        char *end = nullptr;
        const unsigned long codepoint = std::strtoul(name.data() + 1, &end, 16);
        if (end == name.data() + name.size() && codepoint <= 0x10ffff)
            return static_cast<KeySym>(0x01000000UL | codepoint);
    }
    return NoSymbol;
}

} // namespace

namespace xmin::client::x11 {

void xlib_pump_events(Display *display) noexcept
{
    pump(display);
}

void xlib_forget_events(Display *display) noexcept
{
    std::lock_guard<std::mutex> lock(queue_mutex);
    auto found = queues.find(display);
    if (found != queues.end()) {
        for (auto *event : found->second)
            std::free(event);
        queues.erase(found);
    }
    putback_queues.erase(display);
    if (display != nullptr)
        reinterpret_cast<_XPrivDisplay>(display)->qlen = 0;
}

} // namespace xmin::client::x11

extern "C" {

int XQLength(Display *display)
{
    std::lock_guard<std::mutex> lock(queue_mutex);
    const auto found = queues.find(display);
    const auto found_putbacks = putback_queues.find(display);
    const std::size_t raw = found == queues.end() ? 0 : found->second.size();
    const std::size_t putbacks = found_putbacks == putback_queues.end()
        ? 0 : found_putbacks->second.size();
    const auto maximum =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(raw + putbacks, maximum));
}

int XPending(Display *display)
{
    XFlush(display);
    pump(display);
    return XQLength(display);
}

int XEventsQueued(Display *display, int mode)
{
    if (mode == QueuedAfterFlush)
        XFlush(display);
    if (mode != QueuedAlready)
        pump(display);
    return XQLength(display);
}

int XNextEvent(Display *display, XEvent *event)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto found = putback_queues.find(display);
        if (found != putback_queues.end() && !found->second.empty()) {
            *event = found->second.front();
            found->second.pop_front();
            update_queue_length(display);
            return 0;
        }
    }
    std::unique_ptr<xcb_generic_event_t, decltype(&std::free)> raw(
        next_raw(display, true), std::free);
    return convert(display, raw.get(), event) ? 0 : -1;
}

int XPeekEvent(Display *display, XEvent *event)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        const auto found = putback_queues.find(display);
        if (found != putback_queues.end() && !found->second.empty()) {
            *event = found->second.front();
            return 0;
        }
    }
    auto *raw = next_raw(display, true);
    if (raw == nullptr)
        return -1;
    const bool converted = convert(display, raw, event);
    try {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto &queue = queues[display];
        queue.push_front(raw);
        update_queue_length(display);
    }
    catch (...) {
        std::free(raw);
        return -1;
    }
    return converted ? 0 : -1;
}

int XWindowEvent(Display *display, Window window, long mask, XEvent *event)
{
    return wait_matching_event(display, event, [window, mask](const XEvent &value) {
        return xevent_window(value) == window &&
            (xevent_mask(value) & mask) != 0;
    });
}

Bool XCheckMaskEvent(Display *display, long mask, XEvent *event)
{
    return take_matching_event(display, event, [mask](const XEvent &value) {
        return (xevent_mask(value) & mask) != 0;
    }, true);
}

Bool XCheckTypedEvent(Display *display, int type, XEvent *event)
{
    return take_matching_event(display, event, [type](const XEvent &value) {
        return value.type == type;
    }, true);
}

Bool XCheckTypedWindowEvent(
    Display *display, Window window, int type, XEvent *event)
{
    return take_matching_event(
        display, event, [window, type](const XEvent &value) {
            return value.type == type && xevent_window(value) == window;
        }, true);
}

Bool XCheckWindowEvent(
    Display *display, Window window, long mask, XEvent *event)
{
    return take_matching_event(
        display, event, [window, mask](const XEvent &value) {
            return xevent_window(value) == window &&
                (xevent_mask(value) & mask) != 0;
        }, true);
}

Bool XCheckIfEvent(
    Display *display, XEvent *event,
    Bool (*predicate)(Display *, XEvent *, XPointer), XPointer argument)
{
    if (predicate == nullptr)
        return False;
    return take_matching_event(
        display, event, [display, predicate, argument](XEvent value) {
            return predicate(display, &value, argument) != False;
        }, true);
}

int XIfEvent(
    Display *display, XEvent *event,
    Bool (*predicate)(Display *, XEvent *, XPointer), XPointer argument)
{
    if (predicate == nullptr)
        return -1;
    return wait_matching_event(
        display, event, [display, predicate, argument](XEvent value) {
            return predicate(display, &value, argument) != False;
        });
}

int XPutBackEvent(Display *display, XEvent *event)
{
    if (display == nullptr || event == nullptr)
        return 0;
    try {
        std::lock_guard<std::mutex> lock(queue_mutex);
        putback_queues[display].push_front(*event);
        update_queue_length(display);
        return 1;
    }
    catch (...) {
        return 0;
    }
}

Status XSendEvent(
    Display *display, Window window, Bool propagate, long event_mask_value,
    XEvent *event)
{
    auto *xcb = connection(display);
    if (xcb == nullptr || event == nullptr)
        return 0;
    std::array<std::uint8_t, 32> wire{};
    if (event->type == ClientMessage) {
        auto *message = reinterpret_cast<xcb_client_message_event_t *>(wire.data());
        message->response_type = XCB_CLIENT_MESSAGE;
        message->format = static_cast<std::uint8_t>(event->xclient.format);
        message->window = event->xclient.window;
        message->type = event->xclient.message_type;
        if (message->format == 8)
            std::memcpy(message->data.data8, event->xclient.data.b, 20);
        else if (message->format == 16)
            std::copy(event->xclient.data.s, event->xclient.data.s + 10,
                      message->data.data16);
        else
            std::transform(event->xclient.data.l, event->xclient.data.l + 5,
                           message->data.data32, [](long value) {
                               return static_cast<std::uint32_t>(value);
                           });
    }
    else if (event->type == SelectionNotify) {
        auto *selection = reinterpret_cast<xcb_selection_notify_event_t *>(wire.data());
        selection->response_type = XCB_SELECTION_NOTIFY;
        selection->time = event->xselection.time;
        selection->requestor = event->xselection.requestor;
        selection->selection = event->xselection.selection;
        selection->target = event->xselection.target;
        selection->property = event->xselection.property;
    }
    else {
        return 0;
    }
    xcb_send_event(
        xcb, propagate, window, static_cast<std::uint32_t>(event_mask_value),
        reinterpret_cast<const char *>(wire.data()));
    return 1;
}

int XGrabPointer(
    Display *display, Window grab_window, Bool owner_events,
    unsigned int event_mask_value, int pointer_mode, int keyboard_mode,
    Window confine_to, Cursor cursor, Time time)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return GrabNotViewable;
    std::unique_ptr<xcb_grab_pointer_reply_t, decltype(&std::free)> reply(
        xcb_grab_pointer_reply(
            xcb,
            xcb_grab_pointer(
                xcb, owner_events, grab_window,
                static_cast<std::uint16_t>(event_mask_value),
                static_cast<std::uint8_t>(pointer_mode),
                static_cast<std::uint8_t>(keyboard_mode), confine_to, cursor,
                time),
            nullptr),
        std::free);
    return reply == nullptr ? GrabNotViewable : reply->status;
}

int XUngrabPointer(Display *display, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_ungrab_pointer(xcb, time);
    return xcb != nullptr;
}

int XGrabButton(
    Display *display, unsigned int button, unsigned int modifiers,
    Window grab_window, Bool owner_events, unsigned int event_mask_value,
    int pointer_mode, int keyboard_mode, Window confine_to, Cursor cursor)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) {
        xcb_grab_button(
            xcb, owner_events, grab_window,
            static_cast<std::uint16_t>(event_mask_value),
            static_cast<std::uint8_t>(pointer_mode),
            static_cast<std::uint8_t>(keyboard_mode), confine_to, cursor,
            static_cast<std::uint8_t>(button),
            static_cast<std::uint16_t>(modifiers));
    }
    return xcb != nullptr;
}

int XUngrabButton(
    Display *display, unsigned int button, unsigned int modifiers,
    Window grab_window)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) {
        xcb_ungrab_button(
            xcb, static_cast<std::uint8_t>(button), grab_window,
            static_cast<std::uint16_t>(modifiers));
    }
    return xcb != nullptr;
}

int XGrabKey(
    Display *display, int keycode, unsigned int modifiers,
    Window grab_window, Bool owner_events, int pointer_mode,
    int keyboard_mode)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) {
        xcb_grab_key(
            xcb, owner_events, grab_window,
            static_cast<std::uint16_t>(modifiers),
            static_cast<std::uint8_t>(keycode),
            static_cast<std::uint8_t>(pointer_mode),
            static_cast<std::uint8_t>(keyboard_mode));
    }
    return xcb != nullptr;
}

int XUngrabKey(
    Display *display, int keycode, unsigned int modifiers,
    Window grab_window)
{
    auto *xcb = connection(display);
    if (xcb != nullptr) {
        xcb_ungrab_key(
            xcb, static_cast<std::uint8_t>(keycode), grab_window,
            static_cast<std::uint16_t>(modifiers));
    }
    return xcb != nullptr;
}

int XAllowEvents(Display *display, int event_mode, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_allow_events(xcb, static_cast<std::uint8_t>(event_mode), time);
    return xcb != nullptr;
}

int XGrabKeyboard(
    Display *display, Window grab_window, Bool owner_events,
    int pointer_mode, int keyboard_mode, Time time)
{
    auto *xcb = connection(display);
    if (xcb == nullptr)
        return GrabNotViewable;
    std::unique_ptr<xcb_grab_keyboard_reply_t, decltype(&std::free)> reply(
        xcb_grab_keyboard_reply(
            xcb,
            xcb_grab_keyboard(
                xcb, owner_events, grab_window, time,
                static_cast<std::uint8_t>(pointer_mode),
                static_cast<std::uint8_t>(keyboard_mode)),
            nullptr),
        std::free);
    return reply == nullptr ? GrabNotViewable : reply->status;
}

int XUngrabKeyboard(Display *display, Time time)
{
    auto *xcb = connection(display);
    if (xcb != nullptr)
        xcb_ungrab_keyboard(xcb, time);
    return xcb != nullptr;
}

int XQueryKeymap(Display *display, char keys[32])
{
    auto *xcb = connection(display);
    if (xcb == nullptr || keys == nullptr)
        return 0;
    std::unique_ptr<xcb_query_keymap_reply_t, decltype(&std::free)> reply(
        xcb_query_keymap_reply(xcb, xcb_query_keymap(xcb), nullptr),
        std::free);
    if (reply == nullptr)
        return 0;
    std::memcpy(keys, reply->keys, 32);
    return 1;
}

KeySym XKeycodeToKeysym(Display *display, KeyCode keycode, int index)
{
    return keycode_to_keysym(display, keycode, index);
}

KeyCode XKeysymToKeycode(Display *display, KeySym keysym)
{
    const auto storage = reinterpret_cast<_XPrivDisplay>(display);
    if (storage == nullptr)
        return 0;
    for (int code = storage->min_keycode; code <= storage->max_keycode; ++code) {
        for (int column = 0; column < 4; ++column) {
            if (keycode_to_keysym(display, static_cast<KeyCode>(code), column) == keysym)
                return static_cast<KeyCode>(code);
        }
    }
    return 0;
}

char *XKeysymToString(KeySym keysym)
{
    for (const auto &entry : named_keysyms)
        if (entry.symbol == keysym)
            return const_cast<char *>(entry.name);
    static thread_local char text[16];
    if (keysym >= 0x20 && keysym <= 0x7e) {
        text[0] = static_cast<char>(keysym);
        text[1] = '\0';
        return text;
    }
    if (keysym >= XK_F1 && keysym <= XK_F35) {
        std::snprintf(text, sizeof(text), "F%lu",
                      static_cast<unsigned long>(keysym - XK_F1 + 1));
        return text;
    }
    return nullptr;
}

KeySym XLookupKeysym(XKeyEvent *event, int index)
{
    return event == nullptr
        ? NoSymbol : keycode_to_keysym(event->display, event->keycode, index);
}

KeySym XStringToKeysym(const char *name)
{
    if (name == nullptr || *name == '\0')
        return NoSymbol;
    if (name[1] == '\0')
        return static_cast<unsigned char>(name[0]);
    return named_keysym(name);
}

KeySym XkbKeycodeToKeysym(
    Display *display, KeyCode keycode, int group, int level)
{
    return keycode_to_keysym(display, keycode, group * 2 + level);
}

Display *XkbOpenDisplay(
    const char *name, int *event_base, int *error_base, int *major,
    int *minor, int *reason)
{
    Display *display = XOpenDisplay(name);
    if (event_base != nullptr) *event_base = 0;
    if (error_base != nullptr) *error_base = 0;
    if (major != nullptr) *major = XkbMajorVersion;
    if (minor != nullptr) *minor = XkbMinorVersion;
    if (reason != nullptr)
        *reason = display == nullptr ? XkbOD_ConnectionRefused : XkbOD_Success;
    return display;
}

int XLookupString(
    XKeyEvent *event, char *buffer, int capacity, KeySym *keysym,
    XComposeStatus *)
{
    if (event == nullptr)
        return 0;
    const KeySym symbol = event_keysym(event);
    if (keysym != nullptr)
        *keysym = symbol;
    const auto codepoint = keysym_codepoint(symbol, event->state);
    if (codepoint && *codepoint <= 0xff && capacity > 0 && buffer != nullptr) {
        buffer[0] = static_cast<char>(*codepoint);
        return 1;
    }
    return 0;
}

int XRefreshKeyboardMapping(XMappingEvent *)
{
    return 1;
}

Bool XFilterEvent(XEvent *, Window)
{
    return False;
}

char *XSetLocaleModifiers(const char *)
{
    return const_cast<char *>("");
}

XIM XOpenIM(
    Display *display, struct _XrmHashBucketRec *, char *, char *)
{
    auto *input_method = new (std::nothrow) _XIM;
    if (input_method != nullptr)
        input_method->display = display;
    return input_method;
}

Status XCloseIM(XIM input_method)
{
    delete input_method;
    return 1;
}

char *XGetIMValues(XIM input_method, ...)
{
    va_list arguments;
    va_start(arguments, input_method);
    const char *name = va_arg(arguments, const char *);
    while (name != nullptr) {
        void *destination = va_arg(arguments, void *);
        if (std::strcmp(name, XNQueryInputStyle) == 0 && destination != nullptr) {
            auto *styles = static_cast<XIMStyles *>(
                std::calloc(1, sizeof(XIMStyles) + sizeof(XIMStyle)));
            if (styles != nullptr) {
                styles->count_styles = 1;
                styles->supported_styles = reinterpret_cast<XIMStyle *>(styles + 1);
                styles->supported_styles[0] = XIMPreeditNothing | XIMStatusNothing;
            }
            *static_cast<XIMStyles **>(destination) = styles;
        }
        name = va_arg(arguments, const char *);
    }
    va_end(arguments);
    return nullptr;
}

char *XSetIMValues(XIM input_method, ...)
{
    va_list arguments;
    va_start(arguments, input_method);
    const char *name = va_arg(arguments, const char *);
    while (name != nullptr) {
        (void)va_arg(arguments, XPointer);
        name = va_arg(arguments, const char *);
    }
    va_end(arguments);
    return nullptr;
}

Bool XRegisterIMInstantiateCallback(
    Display *, struct _XrmHashBucketRec *, char *, char *, XIDProc, XPointer)
{
    return True;
}

Bool XUnregisterIMInstantiateCallback(
    Display *, struct _XrmHashBucketRec *, char *, char *, XIDProc, XPointer)
{
    return True;
}

XIC XCreateIC(XIM input_method, ...)
{
    if (input_method == nullptr)
        return nullptr;
    auto *context = new (std::nothrow) _XIC;
    if (context == nullptr)
        return nullptr;
    context->input_method = input_method;
    va_list arguments;
    va_start(arguments, input_method);
    const char *name = va_arg(arguments, const char *);
    while (name != nullptr) {
        const auto value = va_arg(arguments, XPointer);
        if (std::strcmp(name, XNInputStyle) == 0) {
            context->input_style = static_cast<XIMStyle>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        else if (std::strcmp(name, XNClientWindow) == 0) {
            context->client_window = static_cast<Window>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        else if (std::strcmp(name, XNFocusWindow) == 0) {
            context->focus_window = static_cast<Window>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        name = va_arg(arguments, const char *);
    }
    va_end(arguments);
    if (context->input_style !=
        (XIMPreeditNothing | XIMStatusNothing)) {
        delete context;
        return nullptr;
    }
    if (context->focus_window == None)
        context->focus_window = context->client_window;
    return context;
}

void XDestroyIC(XIC context)
{
    delete context;
}

char *XGetICValues(XIC context, ...)
{
    if (context == nullptr)
        return const_cast<char *>("");
    char *unsupported = nullptr;
    va_list arguments;
    va_start(arguments, context);
    const char *name = va_arg(arguments, const char *);
    while (name != nullptr) {
        void *destination = va_arg(arguments, void *);
        if (destination == nullptr) {
            unsupported = const_cast<char *>(name);
            break;
        }
        if (std::strcmp(name, XNInputStyle) == 0) {
            *static_cast<XIMStyle *>(destination) = context->input_style;
        }
        else if (std::strcmp(name, XNClientWindow) == 0) {
            *static_cast<Window *>(destination) = context->client_window;
        }
        else if (std::strcmp(name, XNFocusWindow) == 0) {
            *static_cast<Window *>(destination) = context->focus_window;
        }
        else if (std::strcmp(name, XNFilterEvents) == 0) {
            *static_cast<long *>(destination) = context->filter_events;
        }
        else if (std::strcmp(name, XNPreeditAttributes) != 0 &&
                 std::strcmp(name, XNStatusAttributes) != 0) {
            unsupported = const_cast<char *>(name);
            break;
        }
        name = va_arg(arguments, const char *);
    }
    va_end(arguments);
    return unsupported;
}

char *XSetICValues(XIC context, ...)
{
    if (context == nullptr)
        return const_cast<char *>("");
    char *unsupported = nullptr;
    va_list arguments;
    va_start(arguments, context);
    const char *name = va_arg(arguments, const char *);
    while (name != nullptr) {
        const auto value = va_arg(arguments, XPointer);
        if (std::strcmp(name, XNInputStyle) == 0) {
            context->input_style = static_cast<XIMStyle>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        else if (std::strcmp(name, XNClientWindow) == 0) {
            context->client_window = static_cast<Window>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        else if (std::strcmp(name, XNFocusWindow) == 0) {
            context->focus_window = static_cast<Window>(
                reinterpret_cast<std::uintptr_t>(value));
        }
        else if (std::strcmp(name, XNPreeditAttributes) != 0 &&
                 std::strcmp(name, XNStatusAttributes) != 0) {
            unsupported = const_cast<char *>(name);
            break;
        }
        name = va_arg(arguments, const char *);
    }
    va_end(arguments);
    return unsupported;
}

void XSetICFocus(XIC context)
{
    if (context != nullptr)
        context->focused = true;
}

void XUnsetICFocus(XIC context)
{
    if (context != nullptr)
        context->focused = false;
}

char *XmbResetIC(XIC)
{
    return const_cast<char *>("");
}

int Xutf8LookupString(
    XIC, XKeyPressedEvent *event, char *buffer, int capacity,
    KeySym *keysym, Status *status)
{
    const KeySym symbol = event_keysym(event);
    if (keysym != nullptr)
        *keysym = symbol;
    const auto codepoint = event == nullptr
        ? std::optional<std::uint32_t>{}
        : keysym_codepoint(symbol, event->state);
    const int needed = codepoint ? utf8_length(*codepoint) : 0;
    if (needed > capacity || (needed != 0 && buffer == nullptr)) {
        if (status != nullptr)
            *status = XBufferOverflow;
        return needed;
    }
    const int written = codepoint
        ? utf8_encode(*codepoint, buffer, capacity)
        : 0;
    if (status != nullptr) {
        if (written != 0 && symbol != NoSymbol)
            *status = XLookupBoth;
        else if (written != 0)
            *status = XLookupChars;
        else if (symbol != NoSymbol)
            *status = XLookupKeySym;
        else
            *status = XLookupNone;
    }
    return written;
}

int XmbLookupString(
    XIC context, XKeyPressedEvent *event, char *buffer, int capacity,
    KeySym *keysym, Status *status)
{
    return Xutf8LookupString(
        context, event, buffer, capacity, keysym, status);
}

Bool XkbBell(Display *display, Window, int percent, Atom)
{
    return XBell(display, percent) != 0;
}

XVaNestedList XVaCreateNestedList(int, ...)
{
    return std::calloc(1, 1);
}

XFontSet XCreateFontSet(
    Display *, const char *, char ***missing, int *missing_count,
    char **default_string)
{
    if (missing != nullptr) *missing = nullptr;
    if (missing_count != nullptr) *missing_count = 0;
    if (default_string != nullptr) *default_string = const_cast<char *>("");
    return new (std::nothrow) _XOC;
}

void XFreeFontSet(Display *, XFontSet font_set)
{
    delete font_set;
}

} // extern "C"
