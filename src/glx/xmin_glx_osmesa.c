/*
 * Copyright 2026 Xmin contributors
 * SPDX-License-Identifier: MIT
 *
 * In-process GLX provider for Xmin.  This is intentionally not a DRI shim:
 * indirect GLX commands call the embedded, namespaced OSMesa renderer and a
 * CPU image is copied into the X drawable at synchronization points.
 */

#include <dix-config.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glxtokens.h>
#include <OSMesa/osmesa.h>

#include <glapi/dispatch.h>
#include <main/context.h>

#include <dix.h>
#include <gcstruct.h>
#include <os.h>
#include <pixmapstr.h>
#include <scrnintstr.h>
#include <servermd.h>

#include "extension_string.h"
#include "glxserver.h"
#include "glxutil.h"

#define XMIN_OSMESA_MAX_DIMENSION 4096
#define XMIN_OSMESA_BYTES_PER_PIXEL 4
#if IMAGE_BYTE_ORDER == MSBFirst
#define XMIN_OSMESA_X_FORMAT OSMESA_ARGB
#else
#define XMIN_OSMESA_X_FORMAT OSMESA_BGRA
#endif

typedef struct {
    __GLXscreen base;
} XminGlxScreen;

typedef struct {
    __GLXcontext base;
    OSMesaContext osmesa;
} XminGlxContext;

typedef struct {
    __GLXdrawable base;
    unsigned char *pixels;
    int width;
    int height;
    int stride;
} XminGlxDrawable;

static XminGlxContext *xminCurrentContext;

static Bool
xminDrawableStorage(XminGlxDrawable *drawable)
{
    DrawablePtr draw = drawable->base.pDraw;
    size_t bytes;
    int stride;
    unsigned char *pixels;

    if (draw == NULL || draw->width < 1 || draw->height < 1 ||
        draw->width > XMIN_OSMESA_MAX_DIMENSION ||
        draw->height > XMIN_OSMESA_MAX_DIMENSION ||
        draw->bitsPerPixel != 32) {
        return FALSE;
    }

    stride = PixmapBytePad(draw->width, draw->depth);
    if (stride < draw->width * XMIN_OSMESA_BYTES_PER_PIXEL ||
        (size_t) stride > SIZE_MAX / draw->height) {
        return FALSE;
    }

    if (drawable->pixels != NULL && drawable->width == draw->width &&
        drawable->height == draw->height && drawable->stride == stride) {
        return TRUE;
    }

    bytes = (size_t) stride * draw->height;
    pixels = calloc(1, bytes);
    if (pixels == NULL) {
        return FALSE;
    }

    free(drawable->pixels);
    drawable->pixels = pixels;
    drawable->width = draw->width;
    drawable->height = draw->height;
    drawable->stride = stride;
    return TRUE;
}

static void
xminDrawablePresent(XminGlxDrawable *drawable)
{
    DrawablePtr draw = drawable->base.pDraw;
    GCPtr gc;

    if (draw == NULL || drawable->pixels == NULL ||
        drawable->width != draw->width || drawable->height != draw->height) {
        return;
    }

    gc = GetScratchGC(draw->depth, draw->pScreen);
    if (gc == NULL) {
        return;
    }

    ValidateGC(draw, gc);
    gc->ops->PutImage(draw, gc, draw->depth, 0, 0, draw->width, draw->height,
                      0, ZPixmap, (char *) drawable->pixels);
    FreeScratchGC(gc);
}

static void
xminDrawableReadX(XminGlxDrawable *drawable)
{
    DrawablePtr draw = drawable->base.pDraw;

    if (!xminDrawableStorage(drawable)) {
        return;
    }

    draw->pScreen->SourceValidate(draw, 0, 0, draw->width, draw->height,
                                  IncludeInferiors);
    draw->pScreen->GetImage(draw, 0, 0, draw->width, draw->height, ZPixmap,
                            UINT32_MAX, (char *) drawable->pixels);
}

static void
xminDrawableDestroy(__GLXdrawable *base)
{
    XminGlxDrawable *drawable = (XminGlxDrawable *) base;

    __glXDrawableRelease(base);
    free(drawable->pixels);
    free(drawable);
}

static GLboolean
xminDrawableSwapBuffers(ClientPtr client, __GLXdrawable *base)
{
    XminGlxDrawable *drawable = (XminGlxDrawable *) base;

    (void) client;
    glFinish();
    xminDrawablePresent(drawable);
    return GL_TRUE;
}

static void
xminDrawableCopySubBuffer(__GLXdrawable *base, int x, int y, int width,
                          int height)
{
    (void) x;
    (void) y;
    (void) width;
    (void) height;

    glFinish();
    xminDrawablePresent((XminGlxDrawable *) base);
}

static void
xminDrawableWaitX(__GLXdrawable *base)
{
    xminDrawableReadX((XminGlxDrawable *) base);
}

static void
xminDrawableWaitGL(__GLXdrawable *base)
{
    glFinish();
    xminDrawablePresent((XminGlxDrawable *) base);
}

static void GLAPIENTRY
xminContextFlush(void)
{
    XminGlxContext *context = xminCurrentContext;

    _mesa_Flush();
    if (context != NULL && context->base.drawPriv != NULL &&
        context->base.config != NULL &&
        !context->base.config->doubleBufferMode) {
        xminDrawablePresent((XminGlxDrawable *) context->base.drawPriv);
    }
}

static void
xminContextInstallDispatch(XminGlxContext *context)
{
    GLcontext *mesa = _mesa_get_current_context();

    xminCurrentContext = context;
    if (mesa != NULL && mesa->Exec != NULL)
        SET_Flush(mesa->Exec, xminContextFlush);
}

static Bool
xminContextBind(XminGlxContext *context)
{
    XminGlxDrawable *draw = (XminGlxDrawable *) context->base.drawPriv;
    XminGlxDrawable *read = (XminGlxDrawable *) context->base.readPriv;

    if (draw == NULL || read == NULL || !xminDrawableStorage(draw) ||
        !xminDrawableStorage(read) ||
        read->stride != read->width * XMIN_OSMESA_BYTES_PER_PIXEL) {
        return FALSE;
    }

    if (!OSMesaMakeCurrentSeparate(context->osmesa,
                                   draw->pixels, draw->width, draw->height,
                                   read->pixels, read->width, read->height,
                                   GL_UNSIGNED_BYTE)) {
        return FALSE;
    }

    OSMesaPixelStore(OSMESA_ROW_LENGTH,
                     draw->stride / XMIN_OSMESA_BYTES_PER_PIXEL);
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    xminContextInstallDispatch(context);
    return TRUE;
}

static void
xminContextDestroy(__GLXcontext *base)
{
    XminGlxContext *context = (XminGlxContext *) base;

    if (xminCurrentContext == context)
        xminCurrentContext = NULL;
    OSMesaDestroyContext(context->osmesa);
    __glXContextDestroy(base);
    free(context);
}

static int
xminContextMakeCurrent(__GLXcontext *base)
{
    return xminContextBind((XminGlxContext *) base) ? GL_TRUE : GL_FALSE;
}

static int
xminContextLoseCurrent(__GLXcontext *base)
{
    int result;

    if (base->drawPriv != NULL && base->config != NULL &&
        !base->config->doubleBufferMode) {
        xminDrawableWaitGL(base->drawPriv);
    }

    result = OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
    if (result && xminCurrentContext == (XminGlxContext *) base)
        xminCurrentContext = NULL;
    return result;
}

static int
xminContextCopy(__GLXcontext *destination, __GLXcontext *source,
                unsigned long mask)
{
    XminGlxContext *dst = (XminGlxContext *) destination;
    XminGlxContext *src = (XminGlxContext *) source;

    if (dst == NULL || src == NULL ||
        (mask & ~(unsigned long) GL_ALL_ATTRIB_BITS) != 0) {
        return GL_FALSE;
    }
    if (dst != src) {
        /* OSMesa derives its context from GLcontext, so Mesa's native copier
         * provides the GL attribute-group semantics required by GLX. */
        _mesa_copy_context((const GLcontext *) src->osmesa,
                           (GLcontext *) dst->osmesa, (GLuint) mask);
    }
    return GL_TRUE;
}

static Bool
xminContextWait(__GLXcontext *base, __GLXclientState *client, int *error)
{
    (void) client;
    if (xminContextBind((XminGlxContext *) base)) {
        return FALSE;
    }

    *error = BadAlloc;
    return TRUE;
}

static __GLXcontext *
xminScreenCreateContext(__GLXscreen *screen, __GLXconfig *config,
                        __GLXcontext *share, unsigned numAttributes,
                        const uint32_t *attributes, int *error)
{
    XminGlxContext *context;
    OSMesaContext shareContext = NULL;

    (void) screen;
    (void) numAttributes;
    (void) attributes;
    (void) error;

    if (config == NULL) {
        return NULL;
    }
    if (share != NULL) {
        shareContext = ((XminGlxContext *) share)->osmesa;
    }

    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }

    context->osmesa = OSMesaCreateContextExt(XMIN_OSMESA_X_FORMAT,
                                              config->depthBits,
                                              config->stencilBits, 0,
                                              shareContext);
    if (context->osmesa == NULL) {
        free(context);
        return NULL;
    }

    context->base.config = config;
    context->base.destroy = xminContextDestroy;
    context->base.makeCurrent = xminContextMakeCurrent;
    context->base.loseCurrent = xminContextLoseCurrent;
    context->base.copy = xminContextCopy;
    context->base.wait = xminContextWait;
    return &context->base;
}

static __GLXdrawable *
xminScreenCreateDrawable(ClientPtr client, __GLXscreen *screen,
                         DrawablePtr draw, XID drawId, int type,
                         XID glxDrawId, __GLXconfig *config)
{
    XminGlxDrawable *drawable;

    (void) client;
    (void) drawId;

    drawable = calloc(1, sizeof(*drawable));
    if (drawable == NULL) {
        return NULL;
    }

    if (!__glXDrawableInit(&drawable->base, screen, draw, type, glxDrawId,
                           config) ||
        !xminDrawableStorage(drawable)) {
        free(drawable->pixels);
        free(drawable);
        return NULL;
    }

    drawable->base.destroy = xminDrawableDestroy;
    drawable->base.swapBuffers = xminDrawableSwapBuffers;
    drawable->base.copySubBuffer = xminDrawableCopySubBuffer;
    drawable->base.waitX = xminDrawableWaitX;
    drawable->base.waitGL = xminDrawableWaitGL;
    return &drawable->base;
}

static __GLXconfig *
xminCreateConfigs(void)
{
    static const int depthBits[] = { 0, 16, 24, 24 };
    static const int stencilBits[] = { 0, 0, 0, 8 };
    __GLXconfig *head = NULL;
    __GLXconfig **tail = &head;
    size_t i;
    int doubleBuffered;

    for (doubleBuffered = 1; doubleBuffered >= 0; --doubleBuffered) {
        for (i = 0; i < sizeof(depthBits) / sizeof(depthBits[0]); ++i) {
            __GLXconfig *config = calloc(1, sizeof(*config));
            if (config == NULL) {
                __GLXconfig *next;
                while (head != NULL) {
                    next = head->next;
                    free(head);
                    head = next;
                }
                return NULL;
            }

            config->doubleBufferMode = doubleBuffered;
            config->redBits = 8;
            config->greenBits = 8;
            config->blueBits = 8;
            config->redMask = 0x00ff0000U;
            config->greenMask = 0x0000ff00U;
            config->blueMask = 0x000000ffU;
            config->rgbBits = 24;
            config->depthBits = depthBits[i];
            config->stencilBits = stencilBits[i];
            config->visualType = GLX_TRUE_COLOR;
            config->visualRating = GLX_NONE;
            config->transparentPixel = GLX_NONE;
            config->drawableType =
                GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT;
            config->renderType = GLX_RGBA_BIT;
            config->maxPbufferWidth = XMIN_OSMESA_MAX_DIMENSION;
            config->maxPbufferHeight = XMIN_OSMESA_MAX_DIMENSION;
            config->maxPbufferPixels =
                XMIN_OSMESA_MAX_DIMENSION * XMIN_OSMESA_MAX_DIMENSION;
            config->optimalPbufferWidth = 0;
            config->optimalPbufferHeight = 0;
            config->swapMethod = GLX_SWAP_COPY_OML;
            config->yInverted = GL_TRUE;

            *tail = config;
            tail = &config->next;
        }
    }

    return head;
}

static char *
xminRendererExtensions(void)
{
    unsigned char pixel[XMIN_OSMESA_BYTES_PER_PIXEL] = { 0 };
    OSMesaContext context;
    const GLubyte *extensions;
    char *copy = NULL;

    context = OSMesaCreateContextExt(XMIN_OSMESA_X_FORMAT, 0, 0, 0, NULL);
    if (context == NULL)
        return NULL;
    if (OSMesaMakeCurrent(context, pixel, GL_UNSIGNED_BYTE, 1, 1)) {
        extensions = glGetString(GL_EXTENSIONS);
        if (extensions != NULL)
            copy = strdup((const char *) extensions);
        OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
    }
    OSMesaDestroyContext(context);
    return copy;
}

static void
xminScreenDestroy(__GLXscreen *base)
{
    __glXScreenDestroy(base);
    free(base);
}

static glx_func_ptr
xminGetProcAddress(const char *name)
{
    return (glx_func_ptr) OSMesaGetProcAddress(name);
}

static __GLXscreen *
xminScreenProbe(ScreenPtr xScreen)
{
    XminGlxScreen *screen = calloc(1, sizeof(*screen));
    char *rendererExtensions;

    if (screen == NULL) {
        return NULL;
    }

    screen->base.destroy = xminScreenDestroy;
    screen->base.createContext = xminScreenCreateContext;
    screen->base.createDrawable = xminScreenCreateDrawable;
    screen->base.swapInterval = NULL;
    screen->base.fbconfigs = xminCreateConfigs();
    if (screen->base.fbconfigs == NULL) {
        free(screen);
        return NULL;
    }

    rendererExtensions = xminRendererExtensions();
    __glXInitExtensionEnableBits(screen->base.glx_enable_bits);
    __glXEnableExtension(screen->base.glx_enable_bits,
                         "GLX_MESA_copy_sub_buffer");
    __glXScreenInit(&screen->base, xScreen);
    if (rendererExtensions != NULL) {
        free(screen->base.GLextensions);
        screen->base.GLextensions = rendererExtensions;
    }
    __glXsetGetProcAddress(xminGetProcAddress);

    LogMessage(X_INFO,
               "IGLX: initialized embedded OSMesa software renderer\n");
    return &screen->base;
}

/*
 * glxext.c uses this historical built-in provider symbol as the bottom of
 * its provider stack. Xmin supplies OSMesa here and does not compile the DRI
 * implementation that normally owns the symbol.
 */
__GLXprovider __glXDRISWRastProvider = {
    xminScreenProbe,
    "Xmin OSMesa",
    NULL
};
