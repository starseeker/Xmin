#define explicit dont_use_cxx_explicit
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/shm.h>
#include <xcb/sync.h>
#include <xcb/xfixes.h>
#include <xcb/xkb.h>
#undef explicit
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

int main()
{
    int screen = 0;
    xcb_connection_t *connection = xcb_connect(nullptr, &screen);
    xcb_render_query_pict_formats(connection);
    xcb_render_util_find_standard_format(nullptr, XCB_PICT_STANDARD_ARGB_32);
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    xkb_x11_setup_xkb_extension(
        connection,
        XKB_X11_MIN_MAJOR_XKB_VERSION,
        XKB_X11_MIN_MINOR_XKB_VERSION,
        XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
        nullptr, nullptr, nullptr, nullptr);
    xkb_context_unref(context);
    xcb_disconnect(connection);
    return 0;
}
