/* ICCCM property and per-display context helpers used by desktop clients. */
#include "xlib_internal.hpp"

#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct ContextKey {
    Display *display;
    XID resource;
    XContext context;

    bool operator==(const ContextKey &other) const noexcept
    {
        return display == other.display && resource == other.resource &&
            context == other.context;
    }
};

struct ContextKeyHash {
    std::size_t operator()(const ContextKey &key) const noexcept
    {
        const auto display = reinterpret_cast<std::uintptr_t>(key.display);
        std::size_t result = std::hash<std::uintptr_t>{}(display);
        result ^= std::hash<unsigned long>{}(key.resource) +
            0x9e3779b9U + (result << 6U) + (result >> 2U);
        result ^= std::hash<int>{}(key.context) +
            0x9e3779b9U + (result << 6U) + (result >> 2U);
        return result;
    }
};

std::mutex context_mutex;
std::unordered_map<ContextKey, XPointer, ContextKeyHash> contexts;

std::mutex quark_mutex;
XrmQuark next_quark = 1;
std::unordered_map<std::string, XrmQuark> string_quarks;
std::unordered_map<XrmQuark, std::string> quark_strings;

char *duplicate_bytes(const unsigned char *value, std::size_t length)
{
    auto *result = static_cast<char *>(std::calloc(length + 1U, 1U));
    if (result != nullptr && length != 0)
        std::memcpy(result, value, length);
    return result;
}

Status get_size_hints(
    Display *display, Window window, Atom property, XSizeHints *hints,
    long *supplied)
{
    if (hints == nullptr)
        return False;
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char *raw = nullptr;
    const int status = XGetWindowProperty(
        display, window, property, 0, 18, False, XA_WM_SIZE_HINTS, &type,
        &format, &count, &after, &raw);
    if (status != Success || type != XA_WM_SIZE_HINTS || format != 32 ||
        count < 15 || raw == nullptr) {
        XFree(raw);
        return False;
    }
    const auto *values = reinterpret_cast<const unsigned long *>(raw);
    *hints = XSizeHints{};
    hints->flags = static_cast<long>(values[0]);
    hints->x = static_cast<int>(values[1]);
    hints->y = static_cast<int>(values[2]);
    hints->width = static_cast<int>(values[3]);
    hints->height = static_cast<int>(values[4]);
    hints->min_width = static_cast<int>(values[5]);
    hints->min_height = static_cast<int>(values[6]);
    hints->max_width = static_cast<int>(values[7]);
    hints->max_height = static_cast<int>(values[8]);
    hints->width_inc = static_cast<int>(values[9]);
    hints->height_inc = static_cast<int>(values[10]);
    hints->min_aspect.x = static_cast<int>(values[11]);
    hints->min_aspect.y = static_cast<int>(values[12]);
    hints->max_aspect.x = static_cast<int>(values[13]);
    hints->max_aspect.y = static_cast<int>(values[14]);
    if (count >= 18) {
        hints->base_width = static_cast<int>(values[15]);
        hints->base_height = static_cast<int>(values[16]);
        hints->win_gravity = static_cast<int>(values[17]);
    }
    if (supplied != nullptr) {
        *supplied = hints->flags & (PPosition | PSize | PMinSize | PMaxSize |
            PResizeInc | PAspect | (count >= 18 ? PBaseSize | PWinGravity : 0));
    }
    XFree(raw);
    return True;
}

} // namespace

extern "C" {

int XSaveContext(
    Display *display, XID resource, XContext context, const char *data)
{
    try {
        std::lock_guard<std::mutex> lock(context_mutex);
        contexts[{display, resource, context}] = const_cast<char *>(data);
        return XCSUCCESS;
    }
    catch (...) {
        return XCNOMEM;
    }
}

int XFindContext(
    Display *display, XID resource, XContext context, XPointer *data)
{
    std::lock_guard<std::mutex> lock(context_mutex);
    const auto found = contexts.find({display, resource, context});
    if (found == contexts.end())
        return XCNOENT;
    if (data != nullptr)
        *data = found->second;
    return XCSUCCESS;
}

int XDeleteContext(Display *display, XID resource, XContext context)
{
    std::lock_guard<std::mutex> lock(context_mutex);
    return contexts.erase({display, resource, context}) == 0
        ? XCNOENT : XCSUCCESS;
}

XrmQuark XrmUniqueQuark(void)
{
    std::lock_guard<std::mutex> lock(quark_mutex);
    while (next_quark == NULLQUARK || quark_strings.count(next_quark) != 0)
        ++next_quark;
    return next_quark++;
}

XrmQuark XrmStringToQuark(const char *string)
{
    if (string == nullptr)
        return NULLQUARK;
    std::lock_guard<std::mutex> lock(quark_mutex);
    const auto found = string_quarks.find(string);
    if (found != string_quarks.end())
        return found->second;
    while (next_quark == NULLQUARK || quark_strings.count(next_quark) != 0)
        ++next_quark;
    const XrmQuark quark = next_quark++;
    try {
        string_quarks.emplace(string, quark);
        quark_strings.emplace(quark, string);
    }
    catch (...) {
        return NULLQUARK;
    }
    return quark;
}

XrmQuark XrmPermStringToQuark(const char *string)
{
    return XrmStringToQuark(string);
}

XrmString XrmQuarkToString(XrmQuark quark)
{
    std::lock_guard<std::mutex> lock(quark_mutex);
    const auto found = quark_strings.find(quark);
    return found == quark_strings.end()
        ? nullptr : const_cast<char *>(found->second.c_str());
}

Status XGetTextProperty(
    Display *display, Window window, XTextProperty *property, Atom atom)
{
    if (property == nullptr)
        return False;
    *property = XTextProperty{};
    unsigned long after = 0;
    const int status = XGetWindowProperty(
        display, window, atom, 0, std::numeric_limits<long>::max(), False,
        AnyPropertyType, &property->encoding, &property->format,
        &property->nitems, &after, &property->value);
    return status == Success && property->encoding != None;
}

Status XGetWMName(
    Display *display, Window window, XTextProperty *property)
{
    return XGetTextProperty(display, window, property, XA_WM_NAME);
}

Status XGetWMIconName(
    Display *display, Window window, XTextProperty *property)
{
    return XGetTextProperty(display, window, property, XA_WM_ICON_NAME);
}

Status XGetWMClientMachine(
    Display *display, Window window, XTextProperty *property)
{
    return XGetTextProperty(display, window, property, XA_WM_CLIENT_MACHINE);
}

Status XFetchName(Display *display, Window window, char **name)
{
    if (name == nullptr)
        return False;
    *name = nullptr;
    XTextProperty property{};
    if (!XGetWMName(display, window, &property) || property.format != 8) {
        XFree(property.value);
        return False;
    }
    *name = duplicate_bytes(property.value, property.nitems);
    XFree(property.value);
    return *name != nullptr;
}

int XmbTextPropertyToTextList(
    Display *display, const XTextProperty *property, char ***list,
    int *count)
{
    if (list != nullptr)
        *list = nullptr;
    if (count != nullptr)
        *count = 0;
    if (property == nullptr || list == nullptr || count == nullptr ||
        property->format != 8) {
        return XConverterNotFound;
    }
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
    if (property->encoding != XA_STRING && property->encoding != utf8)
        return XConverterNotFound;
    if (property->nitems == 0)
        return Success;

    std::vector<std::pair<const unsigned char *, std::size_t>> strings;
    const unsigned char *begin = property->value;
    const unsigned char *end = begin + property->nitems;
    while (begin < end) {
        const auto *separator = static_cast<const unsigned char *>(
            std::memchr(begin, 0, static_cast<std::size_t>(end - begin)));
        const auto *finish = separator == nullptr ? end : separator;
        strings.emplace_back(begin, static_cast<std::size_t>(finish - begin));
        begin = separator == nullptr ? end : separator + 1;
    }
    auto **result = static_cast<char **>(
        std::calloc(strings.size() + 1U, sizeof(char *)));
    if (result == nullptr)
        return XNoMemory;
    for (std::size_t index = 0; index < strings.size(); ++index) {
        result[index] = duplicate_bytes(strings[index].first, strings[index].second);
        if (result[index] == nullptr) {
            XFreeStringList(result);
            return XNoMemory;
        }
    }
    *list = result;
    *count = static_cast<int>(strings.size());
    return Success;
}

Status XGetClassHint(Display *display, Window window, XClassHint *hint)
{
    if (hint == nullptr)
        return False;
    hint->res_name = nullptr;
    hint->res_class = nullptr;
    XTextProperty property{};
    if (!XGetTextProperty(display, window, &property, XA_WM_CLASS) ||
        property.encoding != XA_STRING || property.format != 8 ||
        property.value == nullptr) {
        XFree(property.value);
        return False;
    }
    const auto *begin = property.value;
    const auto *end = begin + property.nitems;
    const auto *separator = static_cast<const unsigned char *>(
        std::memchr(begin, 0, property.nitems));
    const auto *class_begin = separator == nullptr ? end : separator + 1;
    const auto *class_end = static_cast<const unsigned char *>(
        std::memchr(class_begin, 0, static_cast<std::size_t>(end - class_begin)));
    if (class_end == nullptr)
        class_end = end;
    hint->res_name = duplicate_bytes(
        begin, static_cast<std::size_t>((separator == nullptr ? end : separator) - begin));
    hint->res_class = duplicate_bytes(
        class_begin, static_cast<std::size_t>(class_end - class_begin));
    XFree(property.value);
    if (hint->res_name == nullptr || hint->res_class == nullptr) {
        XFree(hint->res_name);
        XFree(hint->res_class);
        hint->res_name = nullptr;
        hint->res_class = nullptr;
        return False;
    }
    return True;
}

Status XGetTransientForHint(
    Display *display, Window window, Window *parent)
{
    if (parent == nullptr)
        return False;
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char *raw = nullptr;
    const int status = XGetWindowProperty(
        display, window, XA_WM_TRANSIENT_FOR, 0, 1, False, XA_WINDOW,
        &type, &format, &count, &after, &raw);
    const bool valid = status == Success && type == XA_WINDOW && format == 32 &&
        count == 1 && raw != nullptr;
    if (valid)
        *parent = reinterpret_cast<unsigned long *>(raw)[0];
    XFree(raw);
    return valid;
}

Status XSetWMProtocols(
    Display *display, Window window, Atom *protocols, int count)
{
    if (count < 0 || (count != 0 && protocols == nullptr))
        return False;
    const Atom property = XInternAtom(display, "WM_PROTOCOLS", False);
    return XChangeProperty(
        display, window, property, XA_ATOM, 32, PropModeReplace,
        reinterpret_cast<const unsigned char *>(protocols), count);
}

Status XGetWMSizeHints(
    Display *display, Window window, XSizeHints *hints, long *supplied,
    Atom property)
{
    return get_size_hints(display, window, property, hints, supplied);
}

Status XGetWMNormalHints(
    Display *display, Window window, XSizeHints *hints, long *supplied)
{
    return get_size_hints(
        display, window, XA_WM_NORMAL_HINTS, hints, supplied);
}

Status XGetNormalHints(Display *display, Window window, XSizeHints *hints)
{
    return get_size_hints(display, window, XA_WM_NORMAL_HINTS, hints, nullptr);
}

Status XGetSizeHints(
    Display *display, Window window, XSizeHints *hints, Atom property)
{
    return get_size_hints(display, window, property, hints, nullptr);
}

XWMHints *XGetWMHints(Display *display, Window window)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char *raw = nullptr;
    const int status = XGetWindowProperty(
        display, window, XA_WM_HINTS, 0, 9, False, XA_WM_HINTS, &type,
        &format, &count, &after, &raw);
    if (status != Success || type != XA_WM_HINTS || format != 32 ||
        count < 2 || raw == nullptr) {
        XFree(raw);
        return nullptr;
    }
    const auto *values = reinterpret_cast<const unsigned long *>(raw);
    auto *hints = static_cast<XWMHints *>(std::calloc(1, sizeof(XWMHints)));
    if (hints != nullptr) {
        hints->flags = static_cast<long>(values[0]);
        hints->input = static_cast<Bool>(values[1]);
        if (count > 2) hints->initial_state = static_cast<int>(values[2]);
        if (count > 3) hints->icon_pixmap = values[3];
        if (count > 4) hints->icon_window = values[4];
        if (count > 5) hints->icon_x = static_cast<int>(values[5]);
        if (count > 6) hints->icon_y = static_cast<int>(values[6]);
        if (count > 7) hints->icon_mask = values[7];
        if (count > 8) hints->window_group = values[8];
    }
    XFree(raw);
    return hints;
}

} // extern "C"
