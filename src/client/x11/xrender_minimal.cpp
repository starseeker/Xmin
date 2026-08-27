/* Minimal XRender Xlib facade for visual capability queries. */
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>

namespace {

short
component_shift(unsigned long mask)
{
    short shift = 0;
    while (mask != 0 && (mask & 1UL) == 0) {
        ++shift;
        mask >>= 1;
    }
    return shift;
}

short
component_mask(unsigned long mask)
{
    return static_cast<short>(mask >> component_shift(mask));
}

} // namespace

extern "C" {

XRenderPictFormat *
XRenderFindVisualFormat(Display *display, const Visual *visual)
{
    if (display == nullptr || visual == nullptr)
        return nullptr;

    thread_local XRenderPictFormat format{};
    format.id = visual->visualid;
    format.type = PictTypeDirect;
    format.depth = DefaultDepth(display, DefaultScreen(display));
    format.direct.red = component_shift(visual->red_mask);
    format.direct.redMask = component_mask(visual->red_mask);
    format.direct.green = component_shift(visual->green_mask);
    format.direct.greenMask = component_mask(visual->green_mask);
    format.direct.blue = component_shift(visual->blue_mask);
    format.direct.blueMask = component_mask(visual->blue_mask);
    format.direct.alphaMask = 0;
    format.colormap = DefaultColormap(display, DefaultScreen(display));
    return &format;
}

} // extern "C"
