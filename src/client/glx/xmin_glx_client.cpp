/*
 * Xmin's software-direct GLX client ABI. OpenGL calls are provided by the
 * embedded OSMesa objects in the same libGL; this file owns only GLX context,
 * drawable, and X presentation behavior. It intentionally has no DRI/DRM or
 * host OpenGL dependency.
 */
#include <GL/glx.h>
#include <GL/glxext.h>
#include <OSMesa/osmesa.h>
#include <Xmin/GLXxcb.h>
#include <xmin/config.h>
#include "xmin_osmesa_adapter.h"

#include <mutex>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if XMIN_HAVE_QT_XCB
#include <xcb/xcb.h>
#endif

#if XMIN_IS_BIG_ENDIAN
#define XMIN_OSMESA_X_FORMAT OSMESA_ARGB
#else
#define XMIN_OSMESA_X_FORMAT OSMESA_BGRA
#endif

#if defined(__GNUC__) || defined(__clang__)
#define XMIN_WEAK __attribute__((weak))
#else
#define XMIN_WEAK
#endif

typedef struct _XGC *GC;
typedef struct _XImage XImage;

extern "C" {
XVisualInfo *XGetVisualInfo(Display *, long, XVisualInfo *, int *) XMIN_WEAK;
Status XGetGeometry(Display *, XID, Window *, int *, int *,
                    unsigned int *, unsigned int *, unsigned int *,
                    unsigned int *) XMIN_WEAK;
GC XCreateGC(Display *, XID, unsigned long, void *) XMIN_WEAK;
int XFreeGC(Display *, GC) XMIN_WEAK;
XImage *XCreateImage(Display *, Visual *, unsigned int, int, int, char *,
                     unsigned int, unsigned int, int, int) XMIN_WEAK;
int XPutImage(Display *, XID, GC, XImage *, int, int, int, int,
              unsigned int, unsigned int) XMIN_WEAK;
int XFlush(Display *) XMIN_WEAK;
int XSync(Display *, Bool) XMIN_WEAK;
int XFree(void *) XMIN_WEAK;
}

enum {
    XMIN_CONFIG_COUNT = 8,
    XMIN_MAX_PBUFFER = 4096,
    XMIN_TRUE_COLOR = 4,
    XMIN_VISUAL_SCREEN_MASK = 0x2,
    XMIN_VISUAL_CLASS_MASK = 0x8,
    XMIN_ZPIXMAP = 2
};

struct __GLXFBConfigRec {
    int id;
    int screen;
    int double_buffer;
    int depth_bits;
    int stencil_bits;
};

struct __GLXcontextRec {
    OSMesaContext osmesa;
    GLXFBConfig config;
    Display *display;
    Visual *visual;
    GLXDrawable draw;
    GLXDrawable read;
    struct xmin_drawable *draw_surface;
    struct xmin_drawable *read_surface;
};

struct xmin_drawable {
    GLXDrawable handle;
    XID xid;
    unsigned int width;
    unsigned int height;
    unsigned long event_mask;
    unsigned int swap_interval;
    unsigned char *pixels;
    unsigned int storage_width;
    unsigned int storage_height;
    unsigned int references;
    int destroyed;
    int pbuffer;
    struct xmin_drawable *next;
};

static struct __GLXFBConfigRec xmin_configs[XMIN_CONFIG_COUNT];
static std::once_flag xmin_config_once;
static std::mutex xmin_drawable_mutex;
static struct xmin_drawable *xmin_drawables;
static GLXDrawable xmin_next_drawable = 0x70000000UL;
static thread_local GLXContext xmin_current_context;
static thread_local GLXDrawable xmin_current_draw;
static thread_local GLXDrawable xmin_current_read;
static thread_local Display *xmin_current_display;
static thread_local unsigned int xmin_swap_interval;

#if XMIN_HAVE_QT_XCB
struct xmin_xcb_display {
    xcb_connection_t *connection = nullptr;
    xmin_xcb_display *next = nullptr;
};

static std::mutex xmin_xcb_display_mutex;
static xmin_xcb_display *xmin_xcb_displays;

static xcb_connection_t *
xmin_xcb_connection(Display *display)
{
    const std::lock_guard<std::mutex> lock(xmin_xcb_display_mutex);
    for (auto *entry = xmin_xcb_displays; entry != nullptr;
         entry = entry->next) {
        if (reinterpret_cast<Display *>(entry) == display)
            return entry->connection;
    }
    return nullptr;
}
#endif

Display *
xminGlxCreateXcbDisplay(struct xcb_connection_t *connection)
{
#if XMIN_HAVE_QT_XCB
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
        return nullptr;
    auto *entry = new (std::nothrow) xmin_xcb_display;
    if (entry == nullptr)
        return nullptr;
    entry->connection = connection;
    {
        const std::lock_guard<std::mutex> lock(xmin_xcb_display_mutex);
        entry->next = xmin_xcb_displays;
        xmin_xcb_displays = entry;
    }
    return reinterpret_cast<Display *>(entry);
#else
    (void) connection;
    return nullptr;
#endif
}

void
xminGlxDestroyXcbDisplay(Display *display)
{
#if XMIN_HAVE_QT_XCB
    if (display == nullptr)
        return;
    xmin_xcb_display *removed = nullptr;
    {
        const std::lock_guard<std::mutex> lock(xmin_xcb_display_mutex);
        for (auto **link = &xmin_xcb_displays; *link != nullptr;
             link = &(*link)->next) {
            if (reinterpret_cast<Display *>(*link) != display)
                continue;
            removed = *link;
            *link = removed->next;
            break;
        }
    }
    delete removed;
#else
    (void) display;
#endif
}

static void xmin_install_dispatch_hooks(void);

static const char xmin_vendor[] = "Xmin Project";
static const char xmin_version[] = "1.4 Xmin OSMesa software-direct";
static const char xmin_extensions[] =
    "GLX_ARB_get_proc_address GLX_ARB_create_context "
    "GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control";

static void
xmin_init_configs(void)
{
    static const int depth_bits[4] = { 0, 16, 24, 24 };
    static const int stencil_bits[4] = { 0, 0, 0, 8 };
    int double_buffer;
    int depth;
    int index = 0;

    for (double_buffer = 0; double_buffer <= 1; ++double_buffer) {
        for (depth = 0; depth < 4; ++depth) {
            xmin_configs[index].id = index + 1;
            xmin_configs[index].screen = 0;
            xmin_configs[index].double_buffer = double_buffer;
            xmin_configs[index].depth_bits = depth_bits[depth];
            xmin_configs[index].stencil_bits = stencil_bits[depth];
            ++index;
        }
    }
}

static int
xmin_config_attribute(GLXFBConfig config, int attribute, int *value)
{
    if (config == NULL || value == NULL)
        return GLX_BAD_VALUE;
    switch (attribute) {
    case GLX_USE_GL:
    case GLX_RGBA:
    case GLX_X_RENDERABLE:
        *value = True;
        break;
    case GLX_BUFFER_SIZE:
        *value = 32;
        break;
    case GLX_LEVEL:
    case GLX_STEREO:
    case GLX_AUX_BUFFERS:
    case GLX_ACCUM_RED_SIZE:
    case GLX_ACCUM_GREEN_SIZE:
    case GLX_ACCUM_BLUE_SIZE:
    case GLX_ACCUM_ALPHA_SIZE:
    case GLX_SAMPLE_BUFFERS:
    case GLX_SAMPLES:
        *value = 0;
        break;
    case GLX_DOUBLEBUFFER:
        *value = config->double_buffer;
        break;
    case GLX_RED_SIZE:
    case GLX_GREEN_SIZE:
    case GLX_BLUE_SIZE:
    case GLX_ALPHA_SIZE:
        *value = 8;
        break;
    case GLX_DEPTH_SIZE:
        *value = config->depth_bits;
        break;
    case GLX_STENCIL_SIZE:
        *value = config->stencil_bits;
        break;
    case GLX_CONFIG_CAVEAT:
    case GLX_TRANSPARENT_TYPE:
        *value = GLX_NONE;
        break;
    case GLX_X_VISUAL_TYPE:
        *value = GLX_TRUE_COLOR;
        break;
    case GLX_VISUAL_ID:
        *value = 0;
        break;
    case GLX_SCREEN:
        *value = config->screen;
        break;
    case GLX_DRAWABLE_TYPE:
        *value = GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT;
        break;
    case GLX_RENDER_TYPE:
        *value = GLX_RGBA_BIT;
        break;
    case GLX_FBCONFIG_ID:
        *value = config->id;
        break;
    case GLX_MAX_PBUFFER_WIDTH:
    case GLX_MAX_PBUFFER_HEIGHT:
        *value = XMIN_MAX_PBUFFER;
        break;
    case GLX_MAX_PBUFFER_PIXELS:
        *value = XMIN_MAX_PBUFFER * XMIN_MAX_PBUFFER;
        break;
    default:
        return GLX_BAD_ATTRIBUTE;
    }
    return 0;
}

static int
xmin_config_matches(GLXFBConfig config, const int *attributes)
{
    int i;

    if (attributes == NULL)
        return 1;
    for (i = 0; attributes[i] != 0; i += 2) {
        int actual;
        int requested = attributes[i + 1];
        int attribute = attributes[i];

        if (requested == (int) GLX_DONT_CARE)
            continue;
        if (xmin_config_attribute(config, attribute, &actual) != 0)
            return 0;
        switch (attribute) {
        case GLX_DOUBLEBUFFER:
        case GLX_STEREO:
        case GLX_X_RENDERABLE:
        case GLX_DRAWABLE_TYPE:
        case GLX_RENDER_TYPE:
            if (attribute == GLX_DRAWABLE_TYPE || attribute == GLX_RENDER_TYPE) {
                if ((actual & requested) != requested)
                    return 0;
            }
            else if (!!actual != !!requested)
                return 0;
            break;
        case GLX_VISUAL_ID:
            /* Visual IDs belong to a Display, not to this portable template.
             * Resolve them in glXGetFBConfigAttrib; do not reject a candidate
             * while filtering templates here. */
            break;
        case GLX_FBCONFIG_ID:
        case GLX_SCREEN:
            if (actual != requested)
                return 0;
            break;
        default:
            if (actual < requested)
                return 0;
            break;
        }
    }
    return 1;
}

static XVisualInfo *
xmin_visual_info(Display *display, int screen)
{
    if (XGetVisualInfo != NULL && display != NULL) {
        XVisualInfo visual_template;
        XVisualInfo *result;
        int count = 0;

        memset(&visual_template, 0, sizeof(visual_template));
        visual_template.screen = screen;
        visual_template.c_class = XMIN_TRUE_COLOR;
        result = XGetVisualInfo(display,
                                XMIN_VISUAL_SCREEN_MASK |
                                XMIN_VISUAL_CLASS_MASK,
                                &visual_template, &count);
        if (result != NULL && count > 0)
            return result;
        if (result != NULL && XFree != NULL)
            XFree(result);
    }
    {
        auto *info = static_cast<XVisualInfo *>(
            calloc(1, sizeof(XVisualInfo) + sizeof(Visual)));
        Visual *visual;

        if (info == NULL)
            return NULL;
        visual = (Visual *) (info + 1);
        visual->c_class = XMIN_TRUE_COLOR;
        visual->red_mask = 0x00ff0000UL;
        visual->green_mask = 0x0000ff00UL;
        visual->blue_mask = 0x000000ffUL;
        visual->bits_per_rgb = 8;
        visual->map_entries = 256;
        info->visual = visual;
        info->screen = screen;
        info->depth = 24;
        info->c_class = XMIN_TRUE_COLOR;
        info->red_mask = visual->red_mask;
        info->green_mask = visual->green_mask;
        info->blue_mask = visual->blue_mask;
        info->colormap_size = 256;
        info->bits_per_rgb = 8;
        return info;
    }
}

static void
xmin_free_visual_info(XVisualInfo *info, int from_application)
{
    if (info == NULL || from_application)
        return;
    if (XFree != NULL)
        XFree(info);
    else
        free(info);
}

static struct xmin_drawable *
xmin_find_drawable(GLXDrawable handle)
{
    struct xmin_drawable *drawable;

    for (drawable = xmin_drawables; drawable != NULL;
         drawable = drawable->next) {
        if (drawable->handle == handle)
            return drawable;
    }
    return NULL;
}

static GLXDrawable
xmin_add_drawable(XID xid, unsigned int width, unsigned int height, int pbuffer)
{
    auto *drawable = static_cast<struct xmin_drawable *>(
        calloc(1, sizeof(struct xmin_drawable)));

    if (drawable == NULL)
        return 0;
    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    drawable->handle = pbuffer ? xmin_next_drawable++ : xid;
    drawable->xid = xid;
    drawable->width = width;
    drawable->height = height;
    drawable->pbuffer = pbuffer;
    drawable->next = xmin_drawables;
    xmin_drawables = drawable;
    return drawable->handle;
}

static void
xmin_remove_drawable(GLXDrawable handle)
{
    struct xmin_drawable **link;

    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    for (link = &xmin_drawables; *link != NULL; link = &(*link)->next) {
        if ((*link)->handle == handle) {
            struct xmin_drawable *removed = *link;

            *link = removed->next;
            removed->next = NULL;
            removed->destroyed = 1;
            if (removed->references == 0) {
                free(removed->pixels);
                free(removed);
            }
            break;
        }
    }
}

static struct xmin_drawable *
xmin_acquire_surface(GLXDrawable handle)
{
    struct xmin_drawable *surface;

    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    surface = xmin_find_drawable(handle);
    if (surface == NULL) {
        surface = static_cast<struct xmin_drawable *>(
            calloc(1, sizeof(*surface)));
        if (surface != NULL) {
            surface->handle = handle;
            surface->xid = handle;
            surface->next = xmin_drawables;
            xmin_drawables = surface;
        }
    }
    if (surface != NULL)
        ++surface->references;
    return surface;
}

static void
xmin_release_surface(struct xmin_drawable *surface)
{
    int free_surface = 0;

    if (surface == NULL)
        return;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        if (surface->references != 0)
            --surface->references;
        if (surface->references == 0 && surface->destroyed)
            free_surface = 1;
    }
    if (free_surface) {
        free(surface->pixels);
        free(surface);
    }
}

static int
xmin_resize_surface(struct xmin_drawable *surface,
                    unsigned int width, unsigned int height)
{
    unsigned char *pixels;
    unsigned int copy_width;
    unsigned int copy_height;
    unsigned int row;
    size_t size;

    if (surface == NULL || width == 0 || height == 0 ||
        width > XMIN_MAX_PBUFFER || height > XMIN_MAX_PBUFFER ||
        width > SIZE_MAX / 4 / height)
        return 0;
    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    if (surface->pixels != NULL && surface->storage_width == width &&
        surface->storage_height == height)
        return 1;
    size = (size_t) width * height * 4;
    pixels = static_cast<unsigned char *>(calloc(1, size));
    if (pixels == NULL)
        return 0;
    copy_width = surface->storage_width < width ?
        surface->storage_width : width;
    copy_height = surface->storage_height < height ?
        surface->storage_height : height;
    for (row = 0; row < copy_height; ++row) {
        memcpy(pixels + (size_t) row * width * 4,
               surface->pixels +
                   (size_t) row * surface->storage_width * 4,
               (size_t) copy_width * 4);
    }
    free(surface->pixels);
    surface->pixels = pixels;
    surface->storage_width = width;
    surface->storage_height = height;
    return 1;
}

static void
xmin_surface_storage(struct xmin_drawable *surface, unsigned char **pixels,
                     unsigned int *width, unsigned int *height)
{
    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    *pixels = surface->pixels;
    *width = surface->storage_width;
    *height = surface->storage_height;
}

static int
xmin_drawable_size(Display *display, GLXDrawable handle,
                   unsigned int *width, unsigned int *height, int *pbuffer)
{
    struct xmin_drawable *drawable;

    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        drawable = xmin_find_drawable(handle);
        if (drawable != NULL && drawable->pbuffer) {
            *width = drawable->width;
            *height = drawable->height;
            *pbuffer = 1;
            return 1;
        }
    }

    *pbuffer = 0;
#if XMIN_HAVE_QT_XCB
    if (auto *connection = xmin_xcb_connection(display)) {
        auto *reply = xcb_get_geometry_reply(
            connection,
            xcb_get_geometry(connection, static_cast<xcb_drawable_t>(handle)),
            nullptr);
        if (reply == nullptr)
            return 0;
        *width = reply->width;
        *height = reply->height;
        free(reply);
        return 1;
    }
#endif
    if (display != NULL && XGetGeometry != NULL) {
        Window root;
        int x;
        int y;
        unsigned int border;
        unsigned int depth;

        return XGetGeometry(display, handle, &root, &x, &y, width, height,
                            &border, &depth) != 0;
    }
    return 0;
}

static void
xmin_release_context_surfaces(GLXContext context)
{
    struct xmin_drawable *draw_surface;
    struct xmin_drawable *read_surface;

    if (context == NULL)
        return;
    draw_surface = context->draw_surface;
    read_surface = context->read_surface;
    context->draw_surface = NULL;
    context->read_surface = NULL;
    context->draw = 0;
    context->read = 0;
    xmin_release_surface(draw_surface);
    xmin_release_surface(read_surface);
}

static int
xmin_restore_context_binding(GLXContext context)
{
    unsigned char *draw_pixels;
    unsigned char *read_pixels;
    unsigned int draw_width;
    unsigned int draw_height;
    unsigned int read_width;
    unsigned int read_height;

    if (context == NULL || context->draw_surface == NULL ||
        context->read_surface == NULL)
        return 0;
    xmin_surface_storage(context->draw_surface, &draw_pixels, &draw_width,
                         &draw_height);
    xmin_surface_storage(context->read_surface, &read_pixels, &read_width,
                         &read_height);
    if (draw_pixels == NULL || read_pixels == NULL ||
        !OSMesaMakeCurrentSeparate(context->osmesa,
                                   draw_pixels, (GLsizei) draw_width,
                                   (GLsizei) draw_height,
                                   read_pixels, (GLsizei) read_width,
                                   (GLsizei) read_height,
                                   GL_UNSIGNED_BYTE))
        return 0;
    OSMesaPixelStore(OSMESA_ROW_LENGTH, (GLint) draw_width);
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    xmin_install_dispatch_hooks();
    return 1;
}

static int
xmin_bind_context_surfaces(GLXContext context, Display *display,
                           GLXDrawable draw, GLXDrawable read)
{
    struct xmin_drawable *draw_surface = NULL;
    struct xmin_drawable *read_surface = NULL;
    struct xmin_drawable *old_draw;
    struct xmin_drawable *old_read;
    unsigned char *draw_pixels;
    unsigned char *read_pixels;
    unsigned int draw_width;
    unsigned int draw_height;
    unsigned int read_width;
    unsigned int read_height;
    int draw_pbuffer;
    int read_pbuffer;

    if (context == NULL || draw == 0 || read == 0 ||
        !xmin_drawable_size(display, draw, &draw_width, &draw_height,
                            &draw_pbuffer) ||
        !xmin_drawable_size(display, read, &read_width, &read_height,
                            &read_pbuffer))
        return 0;
    (void) draw_pbuffer;
    (void) read_pbuffer;
    draw_surface = xmin_acquire_surface(draw);
    read_surface = xmin_acquire_surface(read);
    if (draw_surface == NULL || read_surface == NULL)
        goto failure;

    if (xmin_current_context == context)
        glFinish();
    if (!xmin_resize_surface(draw_surface, draw_width, draw_height) ||
        !xmin_resize_surface(read_surface, read_width, read_height))
        goto failure;
    xmin_surface_storage(draw_surface, &draw_pixels, &draw_width,
                         &draw_height);
    xmin_surface_storage(read_surface, &read_pixels, &read_width,
                         &read_height);
    if (!OSMesaMakeCurrentSeparate(context->osmesa,
                                   draw_pixels, (GLsizei) draw_width,
                                   (GLsizei) draw_height,
                                   read_pixels, (GLsizei) read_width,
                                   (GLsizei) read_height,
                                   GL_UNSIGNED_BYTE))
        goto failure;

    old_draw = context->draw_surface;
    old_read = context->read_surface;
    context->draw_surface = draw_surface;
    context->read_surface = read_surface;
    context->draw = draw;
    context->read = read;
    OSMesaPixelStore(OSMESA_ROW_LENGTH, (GLint) draw_width);
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    xmin_install_dispatch_hooks();
    xmin_release_surface(old_draw);
    xmin_release_surface(old_read);
    return 1;

failure:
    xmin_release_surface(draw_surface);
    xmin_release_surface(read_surface);
    if (xmin_current_context == context)
        (void) xmin_restore_context_binding(context);
    return 0;
}

static GLXContext
xmin_create_context(GLXFBConfig config, GLXContext share)
{
    GLXContext context;
    OSMesaContext shared = share != NULL ? share->osmesa : NULL;

    if (config == NULL)
        return NULL;
    context = static_cast<GLXContext>(calloc(1, sizeof(*context)));
    if (context == NULL)
        return NULL;
    context->osmesa = OSMesaCreateContextExt(XMIN_OSMESA_X_FORMAT,
                                              config->depth_bits,
                                              config->stencil_bits, 0, shared);
    if (context->osmesa == NULL) {
        free(context);
        return NULL;
    }
    context->config = config;
    return context;
}

static int
xmin_present(Display *display, GLXDrawable handle,
             struct xmin_drawable *surface)
{
    XVisualInfo *visual_info;
    XImage *image;
    GC gc;
    XID xid = handle;
    unsigned char *pixels;
    unsigned int storage_width;
    unsigned int storage_height;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int border;
    Window root;
    int x;
    int y;

    if (surface == NULL)
        return 0;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        if (surface->pbuffer)
            return 1;
        xid = surface->xid;
        pixels = surface->pixels;
        storage_width = surface->storage_width;
        storage_height = surface->storage_height;
    }

#if XMIN_HAVE_QT_XCB
    if (auto *connection = xmin_xcb_connection(display)) {
        auto *geometry = xcb_get_geometry_reply(
            connection,
            xcb_get_geometry(connection, static_cast<xcb_drawable_t>(xid)),
            nullptr);
        if (geometry == nullptr)
            return 0;
        width = geometry->width;
        height = geometry->height;
        depth = geometry->depth;
        free(geometry);
        const std::uint64_t image_size =
            static_cast<std::uint64_t>(width) * height * 4U;
        if (width != storage_width || height != storage_height ||
            pixels == nullptr || (depth != 24 && depth != 32) ||
            image_size > UINT32_MAX) {
            return 0;
        }
        const xcb_gcontext_t xcb_gc = xcb_generate_id(connection);
        xcb_create_gc(connection, xcb_gc,
                      static_cast<xcb_drawable_t>(xid), 0, nullptr);
        xcb_put_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
                      static_cast<xcb_drawable_t>(xid), xcb_gc,
                      static_cast<std::uint16_t>(width),
                      static_cast<std::uint16_t>(height),
                      0, 0, 0, static_cast<std::uint8_t>(depth),
                      static_cast<std::uint32_t>(image_size), pixels);
        xcb_free_gc(connection, xcb_gc);
        return xcb_flush(connection) > 0;
    }
#endif

    if (display == NULL || XGetGeometry == NULL || XCreateGC == NULL ||
        XFreeGC == NULL || XCreateImage == NULL || XPutImage == NULL ||
        XFlush == NULL)
        return 0;
    if (!XGetGeometry(display, xid, &root, &x, &y, &width, &height,
                      &border, &depth) || width != storage_width ||
        height != storage_height || pixels == NULL ||
        (depth != 24 && depth != 32))
        return 0;
    visual_info = xmin_visual_info(display, 0);
    if (visual_info == NULL)
        return 0;
    image = XCreateImage(display, visual_info->visual, depth, XMIN_ZPIXMAP, 0,
                         (char *) pixels, width, height, 32,
                         (int) width * 4);
    xmin_free_visual_info(visual_info, 0);
    if (image == NULL)
        return 0;
    gc = XCreateGC(display, xid, 0, NULL);
    if (gc == NULL) {
        if (XFree != NULL)
            XFree(image);
        else
            free(image);
        return 0;
    }
    XPutImage(display, xid, gc, image, 0, 0, 0, 0, width, height);
    XFreeGC(display, gc);
    if (XFree != NULL)
        XFree(image);
    else
        free(image);
    XFlush(display);
    return 1;
}

static int
xmin_prepare_present(Display *display, GLXDrawable drawable,
                     GLXContext context, int *pbuffer)
{
    unsigned char *pixels;
    unsigned int draw_width;
    unsigned int draw_height;
    unsigned int read_width;
    unsigned int read_height;
    unsigned int storage_width;
    unsigned int storage_height;
    int read_pbuffer;

    if (context == NULL || context->draw_surface == NULL ||
        context->read_surface == NULL || drawable != context->draw ||
        !xmin_drawable_size(display, context->draw, &draw_width, &draw_height,
                            pbuffer) ||
        !xmin_drawable_size(display, context->read, &read_width, &read_height,
                            &read_pbuffer))
        return 0;
    (void) read_pbuffer;
    xmin_surface_storage(context->draw_surface, &pixels, &storage_width,
                         &storage_height);
    if (pixels == NULL || storage_width != draw_width ||
        storage_height != draw_height)
        return xmin_bind_context_surfaces(context, display, context->draw,
                                          context->read);
    xmin_surface_storage(context->read_surface, &pixels, &storage_width,
                         &storage_height);
    if (pixels == NULL || storage_width != read_width ||
        storage_height != read_height)
        return xmin_bind_context_surfaces(context, display, context->draw,
                                          context->read);
    return 1;
}

static void GLAPIENTRY
xmin_client_flush(void)
{
    int pbuffer;

    XminOSMesaFlush();
    if (xmin_current_context == NULL || xmin_current_draw == 0 ||
        xmin_current_context->config->double_buffer ||
        !xmin_prepare_present(xmin_current_display, xmin_current_draw,
                              xmin_current_context, &pbuffer) || pbuffer)
        return;
    (void) xmin_present(xmin_current_display, xmin_current_draw,
                        xmin_current_context->draw_surface);
}

static void
xmin_install_dispatch_hooks(void)
{
    XminOSMesaInstallDispatchHooks(xmin_client_flush);
}

XVisualInfo *
glXChooseVisual(Display *display, int screen, int *attributes)
{
    int requested_double = 0;
    int requested_depth = 0;
    int requested_stencil = 0;
    int rgba = 0;
    int i;

    if (attributes != NULL) {
        for (i = 0; attributes[i] != 0; ++i) {
            switch (attributes[i]) {
            case GLX_RGBA:
                rgba = 1;
                break;
            case GLX_DOUBLEBUFFER:
                requested_double = 1;
                break;
            case GLX_DEPTH_SIZE:
                requested_depth = attributes[++i];
                break;
            case GLX_STENCIL_SIZE:
                requested_stencil = attributes[++i];
                break;
            default:
                if (attributes[i] >= GLX_RED_SIZE &&
                    attributes[i] <= GLX_ACCUM_ALPHA_SIZE)
                    ++i;
                break;
            }
        }
    }
    if (!rgba)
        return NULL;
    std::call_once(xmin_config_once, xmin_init_configs);
    for (i = 0; i < XMIN_CONFIG_COUNT; ++i) {
        GLXFBConfig config = &xmin_configs[i];

        if (config->double_buffer == requested_double &&
            config->depth_bits >= requested_depth &&
            config->stencil_bits >= requested_stencil)
            return xmin_visual_info(display, screen);
    }
    return NULL;
}

GLXContext
glXCreateContext(Display *display, XVisualInfo *visual, GLXContext share,
                 Bool direct)
{
    GLXContext context;
    int i;

    (void) direct;
    std::call_once(xmin_config_once, xmin_init_configs);
    for (i = 0; i < XMIN_CONFIG_COUNT; ++i) {
        if (xmin_configs[i].double_buffer) {
            context = xmin_create_context(&xmin_configs[i], share);
            if (context != NULL) {
                context->display = display;
                context->visual = visual != NULL ? visual->visual : NULL;
            }
            return context;
        }
    }
    return NULL;
}

void
glXDestroyContext(Display *display, GLXContext context)
{
    (void) display;
    if (context == NULL)
        return;
    if (xmin_current_context == context)
        glXMakeCurrent(context->display, 0, NULL);
    else
        xmin_release_context_surfaces(context);
    OSMesaDestroyContext(context->osmesa);
    free(context);
}

Bool
glXMakeContextCurrent(Display *display, GLXDrawable draw, GLXDrawable read,
                      GLXContext context)
{
    GLXContext previous = xmin_current_context;

    if (context == NULL) {
        OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
        xmin_current_context = NULL;
        xmin_current_draw = 0;
        xmin_current_read = 0;
        xmin_current_display = NULL;
        xmin_swap_interval = 0;
        xmin_release_context_surfaces(previous);
        return True;
    }
    if (previous != NULL && previous != context)
        glFlush();
    if (!xmin_bind_context_surfaces(context, display, draw, read))
        return False;
    if (previous != NULL && previous != context)
        xmin_release_context_surfaces(previous);
    context->display = display;
    xmin_current_context = context;
    xmin_current_draw = draw;
    xmin_current_read = read;
    xmin_current_display = display;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        struct xmin_drawable *registered = xmin_find_drawable(draw);

        xmin_swap_interval = registered != NULL ?
            registered->swap_interval : 0;
    }
    return True;
}

Bool
glXMakeCurrent(Display *display, GLXDrawable drawable, GLXContext context)
{
    return glXMakeContextCurrent(display, drawable, drawable, context);
}

void
glXCopyContext(Display *display, GLXContext source, GLXContext destination,
               unsigned long mask)
{
    (void) display;

    /* GLX forbids a current destination, so leave it untouched when that case
     * can be detected in this thread. */
    if (source == NULL || destination == NULL || source == destination ||
        destination == xmin_current_context)
        return;
    XminOSMesaCopyContext(source->osmesa, destination->osmesa, mask);
}

void
glXSwapBuffers(Display *display, GLXDrawable drawable)
{
    int pbuffer;

    if (xmin_current_context == NULL || xmin_current_draw != drawable)
        return;
    glFinish();
    if (!xmin_prepare_present(display, drawable, xmin_current_context,
                              &pbuffer))
        return;
    if (pbuffer)
        return;
    (void) xmin_present(display, drawable,
                        xmin_current_context->draw_surface);
}

GLXPixmap
glXCreateGLXPixmap(Display *display, XVisualInfo *visual, Pixmap pixmap)
{
    (void) display;
    (void) visual;
    return xmin_add_drawable(pixmap, 0, 0, 0);
}

void
glXDestroyGLXPixmap(Display *display, GLXPixmap pixmap)
{
    (void) display;
    xmin_remove_drawable(pixmap);
}

Bool
glXQueryExtension(Display *display, int *error_base, int *event_base)
{
    (void) display;
    if (error_base != NULL)
        *error_base = 0;
    if (event_base != NULL)
        *event_base = 0;
    return True;
}

Bool
glXQueryVersion(Display *display, int *major, int *minor)
{
    (void) display;
    if (major != NULL)
        *major = 1;
    if (minor != NULL)
        *minor = 4;
    return True;
}

Bool
glXIsDirect(Display *display, GLXContext context)
{
    (void) display;
    return context != NULL;
}

int
glXGetConfig(Display *display, XVisualInfo *visual, int attribute, int *value)
{
    (void) display;
    (void) visual;
    std::call_once(xmin_config_once, xmin_init_configs);
    return xmin_config_attribute(&xmin_configs[4], attribute, value);
}

GLXContext
glXGetCurrentContext(void)
{
    return xmin_current_context;
}

GLXDrawable
glXGetCurrentDrawable(void)
{
    return xmin_current_draw;
}

GLXDrawable
glXGetCurrentReadDrawable(void)
{
    return xmin_current_read;
}

Display *
glXGetCurrentDisplay(void)
{
    return xmin_current_display;
}

void
glXWaitGL(void)
{
    glFinish();
}

void
glXWaitX(void)
{
    if (xmin_current_display != NULL && XSync != NULL)
        XSync(xmin_current_display, False);
}

void
glXUseXFont(Font font, int first, int count, int list)
{
    (void) font;
    (void) first;
    (void) count;
    (void) list;
}

const char *
glXQueryExtensionsString(Display *display, int screen)
{
    (void) display;
    (void) screen;
    return xmin_extensions;
}

const char *
glXQueryServerString(Display *display, int screen, int name)
{
    (void) display;
    (void) screen;
    return glXGetClientString(display, name);
}

const char *
glXGetClientString(Display *display, int name)
{
    (void) display;
    switch (name) {
    case GLX_VENDOR:
        return xmin_vendor;
    case GLX_VERSION:
        return xmin_version;
    case GLX_EXTENSIONS:
        return xmin_extensions;
    default:
        return NULL;
    }
}

GLXFBConfig *
glXGetFBConfigs(Display *display, int screen, int *count)
{
    GLXFBConfig *result;
    int i;
    int found = 0;

    (void) display;
    std::call_once(xmin_config_once, xmin_init_configs);
    result = static_cast<GLXFBConfig *>(
        calloc(XMIN_CONFIG_COUNT, sizeof(*result)));
    if (result == NULL) {
        if (count != NULL)
            *count = 0;
        return NULL;
    }
    for (i = 0; i < XMIN_CONFIG_COUNT; ++i) {
        if (xmin_configs[i].screen == screen)
            result[found++] = &xmin_configs[i];
    }
    if (count != NULL)
        *count = found;
    return result;
}

GLXFBConfig *
glXChooseFBConfig(Display *display, int screen, const int *attributes, int *count)
{
    GLXFBConfig *result;
    int i;
    int found = 0;

    (void) display;
    std::call_once(xmin_config_once, xmin_init_configs);
    result = static_cast<GLXFBConfig *>(
        calloc(XMIN_CONFIG_COUNT, sizeof(*result)));
    if (result == NULL) {
        if (count != NULL)
            *count = 0;
        return NULL;
    }
    for (i = 0; i < XMIN_CONFIG_COUNT; ++i) {
        if (xmin_configs[i].screen == screen &&
            xmin_config_matches(&xmin_configs[i], attributes))
            result[found++] = &xmin_configs[i];
    }
    if (count != NULL)
        *count = found;
    if (found == 0) {
        free(result);
        return NULL;
    }
    return result;
}

int
glXGetFBConfigAttrib(Display *display, GLXFBConfig config, int attribute,
                     int *value)
{
    if (attribute == GLX_VISUAL_ID && config != NULL && value != NULL) {
        XVisualInfo *visual = xmin_visual_info(display, config->screen);

        if (visual == NULL)
            return GLX_BAD_VISUAL;
        *value = (int) visual->visualid;
        xmin_free_visual_info(visual, 0);
        return 0;
    }
    return xmin_config_attribute(config, attribute, value);
}

XVisualInfo *
glXGetVisualFromFBConfig(Display *display, GLXFBConfig config)
{
    if (config == NULL)
        return NULL;
    return xmin_visual_info(display, config->screen);
}

GLXWindow
glXCreateWindow(Display *display, GLXFBConfig config, Window window,
                const int *attributes)
{
    (void) display;
    (void) config;
    (void) attributes;
    return xmin_add_drawable(window, 0, 0, 0);
}

void
glXDestroyWindow(Display *display, GLXWindow window)
{
    (void) display;
    xmin_remove_drawable(window);
}

GLXPixmap
glXCreatePixmap(Display *display, GLXFBConfig config, Pixmap pixmap,
                const int *attributes)
{
    (void) display;
    (void) config;
    (void) attributes;
    return xmin_add_drawable(pixmap, 0, 0, 0);
}

void
glXDestroyPixmap(Display *display, GLXPixmap pixmap)
{
    (void) display;
    xmin_remove_drawable(pixmap);
}

GLXPbuffer
glXCreatePbuffer(Display *display, GLXFBConfig config, const int *attributes)
{
    unsigned int width = 1;
    unsigned int height = 1;
    int i;

    (void) display;
    if (config == NULL)
        return 0;
    if (attributes != NULL) {
        for (i = 0; attributes[i] != 0; i += 2) {
            if (attributes[i] == GLX_PBUFFER_WIDTH)
                width = (unsigned int) attributes[i + 1];
            else if (attributes[i] == GLX_PBUFFER_HEIGHT)
                height = (unsigned int) attributes[i + 1];
        }
    }
    if (width == 0 || height == 0 || width > XMIN_MAX_PBUFFER ||
        height > XMIN_MAX_PBUFFER)
        return 0;
    return xmin_add_drawable(0, width, height, 1);
}

void
glXDestroyPbuffer(Display *display, GLXPbuffer pbuffer)
{
    (void) display;
    xmin_remove_drawable(pbuffer);
}

void
glXQueryDrawable(Display *display, GLXDrawable drawable, int attribute,
                 unsigned int *value)
{
    struct xmin_drawable *registered;
    int query_geometry = 0;

    if (value == NULL)
        return;
    *value = 0;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        registered = xmin_find_drawable(drawable);
        if (registered != NULL) {
            if (registered->pbuffer && attribute == GLX_WIDTH)
                *value = registered->width;
            else if (registered->pbuffer && attribute == GLX_HEIGHT)
                *value = registered->height;
            else if (attribute == GLX_EVENT_MASK) {
                *value = (unsigned int) registered->event_mask;
                return;
            }
            else if (attribute == GLX_SWAP_INTERVAL_EXT) {
                *value = registered->swap_interval;
                return;
            }
            else if (attribute == GLX_MAX_SWAP_INTERVAL_EXT) {
                *value = 1;
                return;
            }
            else if (!registered->pbuffer &&
                     (attribute == GLX_WIDTH || attribute == GLX_HEIGHT))
                query_geometry = 1;
            if (!query_geometry)
                return;
        }
    }
    if (attribute == GLX_SWAP_INTERVAL_EXT) {
        *value = drawable == xmin_current_draw ? xmin_swap_interval : 0;
        return;
    }
    if (attribute == GLX_MAX_SWAP_INTERVAL_EXT) {
        *value = 1;
        return;
    }
    if (attribute == GLX_WIDTH || attribute == GLX_HEIGHT) {
        unsigned int width;
        unsigned int height;
        int pbuffer;

        if (xmin_drawable_size(display, drawable, &width, &height, &pbuffer))
            *value = attribute == GLX_WIDTH ? width : height;
    }
}

GLXContext
glXCreateNewContext(Display *display, GLXFBConfig config, int render_type,
                    GLXContext share, Bool direct)
{
    GLXContext context;

    (void) direct;
    if (render_type != GLX_RGBA_TYPE)
        return NULL;
    context = xmin_create_context(config, share);
    if (context != NULL)
        context->display = display;
    return context;
}

int
glXQueryContext(Display *display, GLXContext context, int attribute, int *value)
{
    (void) display;
    if (context == NULL || value == NULL)
        return GLX_BAD_CONTEXT;
    if (attribute == GLX_FBCONFIG_ID)
        *value = context->config->id;
    else if (attribute == GLX_RENDER_TYPE)
        *value = GLX_RGBA_TYPE;
    else if (attribute == GLX_SCREEN)
        *value = context->config->screen;
    else
        return GLX_BAD_ATTRIBUTE;
    return 0;
}

void
glXSelectEvent(Display *display, GLXDrawable drawable, unsigned long mask)
{
    struct xmin_drawable *registered;

    (void) display;
    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    registered = xmin_find_drawable(drawable);
    if (registered != NULL)
        registered->event_mask = mask;
}

void
glXGetSelectedEvent(Display *display, GLXDrawable drawable, unsigned long *mask)
{
    struct xmin_drawable *registered;

    (void) display;
    if (mask == NULL)
        return;
    *mask = 0;
    const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
    registered = xmin_find_drawable(drawable);
    if (registered != NULL)
        *mask = registered->event_mask;
}

GLXContext
glXCreateContextAttribsARB(Display *display, GLXFBConfig config,
                           GLXContext share, Bool direct,
                           const int *attributes)
{
    int major = 1;
    int minor = 0;
    int i;

    if (attributes != NULL) {
        for (i = 0; attributes[i] != 0; i += 2) {
            if (attributes[i] == GLX_CONTEXT_MAJOR_VERSION_ARB)
                major = attributes[i + 1];
            else if (attributes[i] == GLX_CONTEXT_MINOR_VERSION_ARB)
                minor = attributes[i + 1];
            else if (attributes[i] == GLX_CONTEXT_PROFILE_MASK_ARB &&
                     (attributes[i + 1] & GLX_CONTEXT_CORE_PROFILE_BIT_ARB))
                return NULL;
        }
    }
    if (major > 2 || (major == 2 && minor > 0))
        return NULL;
    return glXCreateNewContext(display, config, GLX_RGBA_TYPE, share, direct);
}

int
glXSwapIntervalMESA(unsigned int interval)
{
    struct xmin_drawable *drawable;

    if (xmin_current_context == NULL)
        return GLX_BAD_CONTEXT;
    xmin_swap_interval = interval;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        drawable = xmin_find_drawable(xmin_current_draw);
        if (drawable != NULL)
            drawable->swap_interval = interval;
    }
    return 0;
}

int
glXGetSwapIntervalMESA(void)
{
    return (int) xmin_swap_interval;
}

void
glXSwapIntervalEXT(Display *display, GLXDrawable drawable, int interval)
{
    struct xmin_drawable *registered;

    (void) display;
    if (interval < 0 || interval > 1)
        return;
    {
        const std::lock_guard<std::mutex> lock(xmin_drawable_mutex);
        registered = xmin_find_drawable(drawable);
        if (registered != NULL)
            registered->swap_interval = (unsigned int) interval;
    }
    if (drawable == xmin_current_draw)
        xmin_swap_interval = (unsigned int) interval;
}

int
glXSwapIntervalSGI(int interval)
{
    if (interval <= 0)
        return GLX_BAD_VALUE;
    return glXSwapIntervalMESA((unsigned int) interval);
}

struct xmin_proc {
    const char *name;
    __GLXextFuncPtr address;
};

#define XMIN_PROC(name) { #name, (__GLXextFuncPtr) name }

static const struct xmin_proc xmin_glx_procs[] = {
    XMIN_PROC(glXGetProcAddress),
    XMIN_PROC(glXGetProcAddressARB),
    XMIN_PROC(glXChooseVisual),
    XMIN_PROC(glXCreateContext),
    XMIN_PROC(glXDestroyContext),
    XMIN_PROC(glXMakeCurrent),
    XMIN_PROC(glXMakeContextCurrent),
    XMIN_PROC(glXCopyContext),
    XMIN_PROC(glXSwapBuffers),
    XMIN_PROC(glXCreateGLXPixmap),
    XMIN_PROC(glXDestroyGLXPixmap),
    XMIN_PROC(glXQueryExtension),
    XMIN_PROC(glXQueryVersion),
    XMIN_PROC(glXIsDirect),
    XMIN_PROC(glXGetConfig),
    XMIN_PROC(glXGetCurrentContext),
    XMIN_PROC(glXGetCurrentDrawable),
    XMIN_PROC(glXGetCurrentReadDrawable),
    XMIN_PROC(glXGetCurrentDisplay),
    XMIN_PROC(glXWaitGL),
    XMIN_PROC(glXWaitX),
    XMIN_PROC(glXUseXFont),
    XMIN_PROC(glXQueryExtensionsString),
    XMIN_PROC(glXQueryServerString),
    XMIN_PROC(glXGetClientString),
    XMIN_PROC(glXChooseFBConfig),
    XMIN_PROC(glXGetFBConfigAttrib),
    XMIN_PROC(glXGetFBConfigs),
    XMIN_PROC(glXGetVisualFromFBConfig),
    XMIN_PROC(glXCreateWindow),
    XMIN_PROC(glXDestroyWindow),
    XMIN_PROC(glXCreatePixmap),
    XMIN_PROC(glXDestroyPixmap),
    XMIN_PROC(glXCreatePbuffer),
    XMIN_PROC(glXDestroyPbuffer),
    XMIN_PROC(glXQueryDrawable),
    XMIN_PROC(glXCreateNewContext),
    XMIN_PROC(glXQueryContext),
    XMIN_PROC(glXSelectEvent),
    XMIN_PROC(glXGetSelectedEvent),
    XMIN_PROC(glXCreateContextAttribsARB),
    XMIN_PROC(glXSwapIntervalEXT),
    XMIN_PROC(glXSwapIntervalMESA),
    XMIN_PROC(glXGetSwapIntervalMESA),
    XMIN_PROC(glXSwapIntervalSGI)
};

__GLXextFuncPtr
glXGetProcAddress(const GLubyte *name)
{
    size_t i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < sizeof(xmin_glx_procs) / sizeof(xmin_glx_procs[0]); ++i) {
        if (strcmp((const char *) name, xmin_glx_procs[i].name) == 0)
            return xmin_glx_procs[i].address;
    }
    return (__GLXextFuncPtr) OSMesaGetProcAddress((const char *) name);
}

__GLXextFuncPtr
glXGetProcAddressARB(const GLubyte *name)
{
    return glXGetProcAddress(name);
}
