#include "render_font.hpp"
#include "xlib_internal.hpp"

#include <X11/Xft/Xft.h>
#include <X11/Xregion.h>
#include <xcb/xcb_renderutil.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using xmin::client::font::EmbeddedFace;
using xmin::client::font::RenderFont;

constexpr std::size_t maximum_cached_color_sources = 64;

struct FreeDeleter {
    void operator()(void *pointer) const noexcept
    {
        std::free(pointer);
    }
};

struct XftFontPrivate {
    XftFont public_font{};
    Display *display = nullptr;
    std::unique_ptr<RenderFont> render;
};

XftFontPrivate *font_private(XftFont *font) noexcept
{
    return reinterpret_cast<XftFontPrivate *>(font);
}

bool checked(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> error(
        xcb_request_check(connection, cookie));
    return !error;
}

int clamp_short(int value) noexcept
{
    return std::clamp(
        value, static_cast<int>(std::numeric_limits<short>::min()),
        static_cast<int>(std::numeric_limits<short>::max()));
}

double pattern_size(FcPattern *pattern)
{
    double size = 12.0;
    if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &size) !=
            FcResultMatch &&
        FcPatternGetDouble(pattern, FC_SIZE, 0, &size) != FcResultMatch) {
        size = 12.0;
    }
    return std::isfinite(size) ? std::clamp(size, 1.0, 512.0) : 12.0;
}

EmbeddedFace pattern_face(FcPattern *pattern)
{
    FcChar8 *family = nullptr;
    int spacing = FC_PROPORTIONAL;
    int weight = FC_WEIGHT_REGULAR;
    int slant = FC_SLANT_ROMAN;
    FcPatternGetString(pattern, FC_FAMILY, 0, &family);
    FcPatternGetInteger(pattern, FC_SPACING, 0, &spacing);
    FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight);
    FcPatternGetInteger(pattern, FC_SLANT, 0, &slant);
    std::string lowered = family == nullptr
        ? "sans"
        : reinterpret_cast<const char *>(family);
    std::transform(
        lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                character >= 'A' && character <= 'Z'
                    ? character - 'A' + 'a'
                    : character);
        });
    const bool monospace = spacing >= FC_MONO ||
        lowered.find("mono") != std::string::npos ||
        lowered.find("fixed") != std::string::npos ||
        lowered.find("courier") != std::string::npos ||
        lowered.find("console") != std::string::npos;
    return xmin::client::font::select_embedded_face(
        monospace, weight >= FC_WEIGHT_DEMIBOLD,
        slant >= FC_SLANT_ITALIC);
}

FcPattern *matched_pattern(const FcPattern *pattern)
{
    FcPattern *copy = FcPatternDuplicate(pattern);
    if (copy == nullptr) {
        return nullptr;
    }
    FcResult result = FcResultNoMatch;
    FcFontSet *set = FcFontSort(nullptr, copy, FcTrue, nullptr, &result);
    FcPatternDestroy(copy);
    if (set == nullptr || set->nfont == 0) {
        FcFontSetDestroy(set);
        return nullptr;
    }
    FcPattern *match = set->fonts[0];
    set->fonts[0] = nullptr;
    FcFontSetDestroy(set);
    return match;
}

XftFont *open_pattern(Display *display, FcPattern *pattern)
{
    if (display == nullptr || pattern == nullptr) {
        FcPatternDestroy(pattern);
        return nullptr;
    }
    auto font = std::unique_ptr<XftFontPrivate>(
        new (std::nothrow) XftFontPrivate);
    if (!font) {
        FcPatternDestroy(pattern);
        return nullptr;
    }
    const EmbeddedFace face = pattern_face(pattern);
    font->render = std::make_unique<RenderFont>(
        xmin::client::x11::xlib_connection(display), face,
        static_cast<float>(pattern_size(pattern)));
    if (!font->render->valid()) {
        FcPatternDestroy(pattern);
        return nullptr;
    }
    font->display = display;
    font->public_font.pattern = pattern;
    FcCharSet *charset = nullptr;
    if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) ==
        FcResultMatch) {
        font->public_font.charset = FcCharSetCopy(charset);
    }
    const auto &metrics = font->render->font().metrics();
    font->public_font.ascent = metrics.ascent;
    font->public_font.descent = metrics.descent;
    font->public_font.height = metrics.height;
    int maximum_advance = 0;
    for (char32_t codepoint = 32; codepoint < 127; ++codepoint) {
        maximum_advance = std::max(
            maximum_advance,
            font->render->font()
                .glyph_metrics(font->render->font().glyph_index(codepoint))
                .advance);
    }
    font->public_font.max_advance_width = maximum_advance;
    return &font.release()->public_font;
}

std::vector<std::uint32_t> glyphs_for_codepoints(
    XftFont *font, const FcChar32 *codepoints, int length)
{
    std::vector<std::uint32_t> result;
    if (font == nullptr || codepoints == nullptr || length <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(length));
    auto *render = font_private(font)->render.get();
    for (int index = 0; index < length; ++index) {
        result.push_back(render->font().glyph_index(codepoints[index]));
    }
    return result;
}

std::vector<FcChar32> decode_utf8(const FcChar8 *text, int length)
{
    std::vector<FcChar32> result;
    if (text == nullptr || length <= 0) {
        return result;
    }
    result.reserve(static_cast<std::size_t>(length));
    for (int offset = 0; offset < length;) {
        FcChar32 codepoint = 0xfffdU;
        int consumed = FcUtf8ToUcs4(text + offset, &codepoint, length - offset);
        if (consumed <= 0) {
            consumed = 1;
        }
        result.push_back(codepoint);
        offset += consumed;
    }
    return result;
}

void glyph_extents(
    XftFont *font, const std::uint32_t *glyphs, int count,
    XGlyphInfo *extents)
{
    if (extents == nullptr) {
        return;
    }
    *extents = {};
    if (font == nullptr || glyphs == nullptr || count <= 0) {
        return;
    }
    const auto &engine = font_private(font)->render->font();
    int pen = 0;
    int left = std::numeric_limits<int>::max();
    int top = std::numeric_limits<int>::max();
    int right = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    for (int index = 0; index < count; ++index) {
        const auto metrics = engine.glyph_metrics(glyphs[index]);
        if (metrics.width > 0 && metrics.height > 0) {
            left = std::min(left, pen + metrics.x);
            top = std::min(top, metrics.y);
            right = std::max(right, pen + metrics.x + metrics.width);
            bottom = std::max(bottom, metrics.y + metrics.height);
        }
        pen += metrics.advance;
    }
    if (left <= right && top <= bottom) {
        extents->width = static_cast<unsigned short>(
            std::clamp(right - left, 0, 65535));
        extents->height = static_cast<unsigned short>(
            std::clamp(bottom - top, 0, 65535));
        extents->x = static_cast<short>(clamp_short(-left));
        extents->y = static_cast<short>(clamp_short(-top));
    }
    extents->xOff = static_cast<short>(clamp_short(pen));
}

std::uint64_t color_key(const XRenderColor &color) noexcept
{
    return (static_cast<std::uint64_t>(color.red) << 48U) |
           (static_cast<std::uint64_t>(color.green) << 32U) |
           (static_cast<std::uint64_t>(color.blue) << 16U) | color.alpha;
}

} // namespace

struct _XftDraw {
    Display *display = nullptr;
    Drawable drawable = None;
    Visual *visual = nullptr;
    Colormap colormap = None;
    Picture destination = None;
    std::unordered_map<std::uint64_t, Picture> sources;
    std::vector<xcb_rectangle_t> clip_rectangles;
    bool clip_set = false;
};

namespace {

bool create_destination(XftDraw *draw)
{
    if (draw == nullptr || draw->display == nullptr || draw->visual == nullptr) {
        return false;
    }
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    if (draw->destination != None) {
        xcb_render_free_picture(connection, draw->destination);
        draw->destination = None;
    }
    xcb_generic_error_t *raw_error = nullptr;
    std::unique_ptr<xcb_render_query_pict_formats_reply_t, FreeDeleter> formats(
        xcb_render_query_pict_formats_reply(
            connection, xcb_render_query_pict_formats(connection),
            &raw_error));
    std::unique_ptr<xcb_generic_error_t, FreeDeleter> error(raw_error);
    const auto *format = formats
        ? xcb_render_util_find_visual_format(
              formats.get(), static_cast<xcb_visualid_t>(draw->visual->visualid))
        : nullptr;
    if (error || format == nullptr) {
        return false;
    }
    draw->destination = xcb_generate_id(connection);
    if (!checked(
            connection, xcb_render_create_picture_checked(
                            connection, draw->destination,
                            static_cast<xcb_drawable_t>(draw->drawable),
                            format->format, 0, nullptr))) {
        draw->destination = None;
        return false;
    }
    if (draw->clip_set && !checked(
            connection, xcb_render_set_picture_clip_rectangles_checked(
                            connection, draw->destination, 0, 0,
                            static_cast<std::uint32_t>(
                                draw->clip_rectangles.size()),
                            draw->clip_rectangles.data()))) {
        xcb_render_free_picture(connection, draw->destination);
        draw->destination = None;
        return false;
    }
    return true;
}

Picture source_picture(XftDraw *draw, const XftColor *color)
{
    if (draw == nullptr || color == nullptr) {
        return None;
    }
    const std::uint64_t key = color_key(color->color);
    const auto found = draw->sources.find(key);
    if (found != draw->sources.end()) {
        return found->second;
    }
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    const Picture picture = xcb_generate_id(connection);
    const xcb_render_color_t value{
        color->color.red, color->color.green, color->color.blue,
        color->color.alpha};
    if (!checked(
            connection,
            xcb_render_create_solid_fill_checked(connection, picture, value))) {
        return None;
    }
    if (draw->sources.size() >= maximum_cached_color_sources) {
        const auto evicted = draw->sources.begin();
        xcb_render_free_picture(connection, evicted->second);
        draw->sources.erase(evicted);
    }
    try {
        draw->sources.emplace(key, picture);
    }
    catch (...) {
        xcb_render_free_picture(connection, picture);
        return None;
    }
    return picture;
}

void draw_glyphs(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const std::uint32_t *glyphs, int count)
{
    if (draw == nullptr || font == nullptr || glyphs == nullptr || count <= 0 ||
        draw->destination == None) {
        return;
    }
    const Picture source = source_picture(draw, color);
    if (source != None) {
        font_private(font)->render->draw(
            source, draw->destination, x, y, glyphs,
            static_cast<std::size_t>(count));
    }
}

} // namespace

extern "C" {

FcPattern *XftFontMatch(
    Display *, int, const FcPattern *pattern, FcResult *result)
{
    FcPattern *match = matched_pattern(pattern);
    if (result != nullptr) {
        *result = match == nullptr ? FcResultNoMatch : FcResultMatch;
    }
    return match;
}

XftFont *XftFontOpenPattern(Display *display, FcPattern *pattern)
{
    return open_pattern(display, pattern);
}

XftFont *XftFontOpen(Display *display, int screen, ...)
{
    FcPattern *pattern = FcPatternCreate();
    if (pattern == nullptr) {
        return nullptr;
    }
    va_list arguments;
    va_start(arguments, screen);
    for (;;) {
        const char *object = va_arg(arguments, const char *);
        if (object == nullptr) {
            break;
        }
        const FcType type = static_cast<FcType>(va_arg(arguments, int));
        FcBool added = FcFalse;
        switch (type) {
        case FcTypeInteger:
            added = FcPatternAddInteger(pattern, object, va_arg(arguments, int));
            break;
        case FcTypeDouble:
            added = FcPatternAddDouble(pattern, object, va_arg(arguments, double));
            break;
        case FcTypeString:
            added = FcPatternAddString(
                pattern, object,
                reinterpret_cast<const FcChar8 *>(
                    va_arg(arguments, const char *)));
            break;
        case FcTypeBool:
            added = FcPatternAddBool(pattern, object, va_arg(arguments, int));
            break;
        case FcTypeMatrix:
            added = FcPatternAddMatrix(
                pattern, object, va_arg(arguments, const FcMatrix *));
            break;
        default:
            added = FcFalse;
            break;
        }
        if (!added) {
            va_end(arguments);
            FcPatternDestroy(pattern);
            return nullptr;
        }
    }
    va_end(arguments);
    FcPattern *match = matched_pattern(pattern);
    FcPatternDestroy(pattern);
    return open_pattern(display, match);
}

XftFont *XftFontOpenName(Display *display, int, const char *name)
{
    return open_pattern(
        display,
        FcNameParse(reinterpret_cast<const FcChar8 *>(name == nullptr
                                                         ? "sans"
                                                         : name)));
}

XftFont *XftFontOpenXlfd(Display *display, int, const char *name)
{
    return open_pattern(display, XftXlfdParse(name, FcFalse, FcFalse));
}

XftFont *XftFontCopy(Display *display, XftFont *font)
{
    return font == nullptr
        ? nullptr
        : open_pattern(display, FcPatternDuplicate(font->pattern));
}

void XftFontClose(Display *, XftFont *font)
{
    if (font == nullptr) {
        return;
    }
    auto *private_font = font_private(font);
    FcCharSetDestroy(font->charset);
    FcPatternDestroy(font->pattern);
    delete private_font;
}

FT_UInt XftCharIndex(Display *, XftFont *font, FcChar32 codepoint)
{
    return font == nullptr
        ? 0
        : font_private(font)->render->font().glyph_index(codepoint);
}

FcBool XftCharExists(Display *display, XftFont *font, FcChar32 codepoint)
{
    return XftCharIndex(display, font, codepoint) != 0 ? FcTrue : FcFalse;
}

void XftGlyphExtents(
    Display *, XftFont *font, const FT_UInt *glyphs, int count,
    XGlyphInfo *extents)
{
    glyph_extents(font, glyphs, count, extents);
}

void XftTextExtents8(
    Display *display, XftFont *font, const FcChar8 *text, int length,
    XGlyphInfo *extents)
{
    std::vector<FcChar32> codepoints;
    if (text != nullptr && length > 0) {
        codepoints.assign(text, text + length);
    }
    const auto glyphs = glyphs_for_codepoints(
        font, codepoints.data(), static_cast<int>(codepoints.size()));
    XftGlyphExtents(
        display, font, glyphs.data(), static_cast<int>(glyphs.size()), extents);
}

void XftTextExtents16(
    Display *display, XftFont *font, const FcChar16 *text, int length,
    XGlyphInfo *extents)
{
    std::vector<FcChar32> codepoints;
    if (text != nullptr && length > 0) {
        codepoints.assign(text, text + length);
    }
    const auto glyphs = glyphs_for_codepoints(
        font, codepoints.data(), static_cast<int>(codepoints.size()));
    XftGlyphExtents(
        display, font, glyphs.data(), static_cast<int>(glyphs.size()), extents);
}

void XftTextExtents32(
    Display *display, XftFont *font, const FcChar32 *text, int length,
    XGlyphInfo *extents)
{
    const auto glyphs = glyphs_for_codepoints(font, text, length);
    XftGlyphExtents(
        display, font, glyphs.data(), static_cast<int>(glyphs.size()), extents);
}

void XftTextExtentsUtf8(
    Display *display, XftFont *font, const FcChar8 *text, int length,
    XGlyphInfo *extents)
{
    const auto codepoints = decode_utf8(text, length);
    XftTextExtents32(
        display, font, codepoints.data(), static_cast<int>(codepoints.size()),
        extents);
}

XftDraw *XftDrawCreate(
    Display *display, Drawable drawable, Visual *visual, Colormap colormap)
{
    auto draw = std::unique_ptr<XftDraw>(new (std::nothrow) XftDraw);
    if (!draw) {
        return nullptr;
    }
    draw->display = display;
    draw->drawable = drawable;
    draw->visual = visual;
    draw->colormap = colormap;
    return create_destination(draw.get()) ? draw.release() : nullptr;
}

void XftDrawChange(XftDraw *draw, Drawable drawable)
{
    if (draw != nullptr && draw->drawable != drawable) {
        draw->drawable = drawable;
        create_destination(draw);
    }
}

Display *XftDrawDisplay(XftDraw *draw)
{
    return draw == nullptr ? nullptr : draw->display;
}

Drawable XftDrawDrawable(XftDraw *draw)
{
    return draw == nullptr ? None : draw->drawable;
}

Colormap XftDrawColormap(XftDraw *draw)
{
    return draw == nullptr ? None : draw->colormap;
}

Visual *XftDrawVisual(XftDraw *draw)
{
    return draw == nullptr ? nullptr : draw->visual;
}

Picture XftDrawPicture(XftDraw *draw)
{
    return draw == nullptr ? None : draw->destination;
}

Picture XftDrawSrcPicture(XftDraw *draw, const XftColor *color)
{
    return source_picture(draw, color);
}

void XftDrawDestroy(XftDraw *draw)
{
    if (draw == nullptr) {
        return;
    }
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    for (const auto &source : draw->sources) {
        xcb_render_free_picture(connection, source.second);
    }
    if (draw->destination != None) {
        xcb_render_free_picture(connection, draw->destination);
    }
    delete draw;
}

void XftDrawGlyphs(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const FT_UInt *glyphs, int count)
{
    draw_glyphs(draw, color, font, x, y, glyphs, count);
}

void XftDrawString8(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const FcChar8 *text, int length)
{
    std::vector<FcChar32> codepoints;
    if (text != nullptr && length > 0) {
        codepoints.assign(text, text + length);
    }
    const auto glyphs = glyphs_for_codepoints(
        font, codepoints.data(), static_cast<int>(codepoints.size()));
    draw_glyphs(
        draw, color, font, x, y, glyphs.data(),
        static_cast<int>(glyphs.size()));
}

void XftDrawString16(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const FcChar16 *text, int length)
{
    std::vector<FcChar32> codepoints;
    if (text != nullptr && length > 0) {
        codepoints.assign(text, text + length);
    }
    const auto glyphs = glyphs_for_codepoints(
        font, codepoints.data(), static_cast<int>(codepoints.size()));
    draw_glyphs(
        draw, color, font, x, y, glyphs.data(),
        static_cast<int>(glyphs.size()));
}

void XftDrawString32(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const FcChar32 *text, int length)
{
    const auto glyphs = glyphs_for_codepoints(font, text, length);
    draw_glyphs(
        draw, color, font, x, y, glyphs.data(),
        static_cast<int>(glyphs.size()));
}

void XftDrawStringUtf8(
    XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
    const FcChar8 *text, int length)
{
    const auto codepoints = decode_utf8(text, length);
    XftDrawString32(
        draw, color, font, x, y, codepoints.data(),
        static_cast<int>(codepoints.size()));
}

void XftDrawGlyphFontSpec(
    XftDraw *draw, const XftColor *color, const XftGlyphFontSpec *glyphs,
    int count)
{
    if (glyphs == nullptr) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        XftDrawGlyphs(
            draw, color, glyphs[index].font, glyphs[index].x,
            glyphs[index].y, &glyphs[index].glyph, 1);
    }
}

Bool XftDrawSetClip(XftDraw *draw, Region region)
{
    if (draw == nullptr || draw->destination == None)
        return False;
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    if (connection == nullptr)
        return False;
    if (region == nullptr) {
        const std::uint32_t no_clip_mask = XCB_NONE;
        if (!checked(
                connection, xcb_render_change_picture_checked(
                                connection, draw->destination,
                                XCB_RENDER_CP_CLIP_MASK, &no_clip_mask)))
            return False;
        draw->clip_rectangles.clear();
        draw->clip_set = false;
        return True;
    }
    std::vector<xcb_rectangle_t> rectangles;
    rectangles.reserve(static_cast<std::size_t>(region->numRects));
    for (long index = 0; index < region->numRects; ++index) {
        const BOX &box = region->rects[index];
        if (box.x2 > box.x1 && box.y2 > box.y1) {
            rectangles.push_back({
                box.x1, box.y1,
                static_cast<std::uint16_t>(box.x2 - box.x1),
                static_cast<std::uint16_t>(box.y2 - box.y1)});
        }
    }
    if (!checked(
            connection, xcb_render_set_picture_clip_rectangles_checked(
                            connection, draw->destination, 0, 0,
                            static_cast<std::uint32_t>(rectangles.size()),
                            rectangles.data())))
        return False;
    draw->clip_rectangles = std::move(rectangles);
    draw->clip_set = true;
    return True;
}

Bool XftDrawSetClipRectangles(
    XftDraw *draw, int x_origin, int y_origin, const XRectangle *rectangles,
    int count)
{
    if (draw == nullptr || draw->destination == None || count < 0 ||
        (count != 0 && rectangles == nullptr)) {
        return False;
    }
    std::vector<xcb_rectangle_t> wire;
    wire.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (rectangles[index].width != 0 && rectangles[index].height != 0) {
            wire.push_back({
                rectangles[index].x, rectangles[index].y,
                rectangles[index].width, rectangles[index].height});
        }
    }
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    if (!checked(
            connection, xcb_render_set_picture_clip_rectangles_checked(
                            connection, draw->destination,
                            static_cast<std::int16_t>(x_origin),
                            static_cast<std::int16_t>(y_origin),
                            static_cast<std::uint32_t>(wire.size()),
                            wire.data()))) {
        return False;
    }
    draw->clip_rectangles = std::move(wire);
    draw->clip_set = true;
    return True;
}

void XftDrawRect(
    XftDraw *draw, const XftColor *color, int x, int y,
    unsigned int width, unsigned int height)
{
    if (draw == nullptr || color == nullptr || draw->destination == None ||
        width == 0 || height == 0) {
        return;
    }
    const Picture source = source_picture(draw, color);
    auto *connection = xmin::client::x11::xlib_connection(draw->display);
    if (source != None && connection != nullptr) {
        xcb_render_composite(
            connection, XCB_RENDER_PICT_OP_SRC, source, None,
            draw->destination, 0, 0, 0, 0,
            static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
            static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height));
    }
}

void XftDefaultSubstitute(Display *, int, FcPattern *)
{
}

Bool XftDefaultHasRender(Display *)
{
    return True;
}

FcBool XftInit(const char *)
{
    return FcTrue;
}

int XftGetVersion(void)
{
    return XFT_VERSION;
}

FcFontSet *XftListFonts(Display *, int, ...)
{
    return FcFontList(nullptr, nullptr, nullptr);
}

FcPattern *XftNameParse(const char *name)
{
    return FcNameParse(reinterpret_cast<const FcChar8 *>(name));
}

FcBool XftNameUnparse(FcPattern *pattern, char *destination, int length)
{
    if (destination == nullptr || length <= 0) {
        return FcFalse;
    }
    std::unique_ptr<FcChar8, FreeDeleter> text(FcNameUnparse(pattern));
    if (!text) {
        return FcFalse;
    }
    std::strncpy(
        destination, reinterpret_cast<const char *>(text.get()),
        static_cast<std::size_t>(length));
    destination[length - 1] = '\0';
    return FcTrue;
}

FcPattern *XftXlfdParse(const char *name, Bool, Bool)
{
    if (name == nullptr) {
        return nullptr;
    }
    return FcNameParse(reinterpret_cast<const FcChar8 *>(name));
}

Bool XftColorAllocValue(
    Display *, Visual *, Colormap, const XRenderColor *color,
    XftColor *result)
{
    if (color == nullptr || result == nullptr) {
        return False;
    }
    result->color = *color;
    result->pixel =
        (static_cast<unsigned long>(color->red >> 8U) << 16U) |
        (static_cast<unsigned long>(color->green >> 8U) << 8U) |
        static_cast<unsigned long>(color->blue >> 8U);
    return True;
}

Bool XftColorAllocName(
    Display *display, const Visual *visual, Colormap colormap,
    const char *name, XftColor *result)
{
    XColor parsed{};
    if (result == nullptr ||
        !XParseColor(display, colormap, name, &parsed) ||
        !XAllocColor(display, colormap, &parsed)) {
        return False;
    }
    result->pixel = parsed.pixel;
    result->color = {
        parsed.red, parsed.green, parsed.blue, 0xffffU};
    (void)visual;
    return True;
}

void XftColorFree(Display *, Visual *, Colormap, XftColor *)
{
}

} // extern "C"
