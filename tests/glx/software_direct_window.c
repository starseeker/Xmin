#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <GL/glx.h>
#include <GL/glxext.h>

#include <limits.h>
#include <stdio.h>

int
main(void)
{
    static const int config_attributes[] = {
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        0
    };
    static const int single_config_attributes[] = {
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, False,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        0
    };
    Display *display = XOpenDisplay(NULL);
    GLXFBConfig *configs = NULL;
    GLXFBConfig *single_configs = NULL;
    XVisualInfo *visual = NULL;
    GLXContext context = NULL;
    GLXContext single_context = NULL;
    Window window = 0;
    GLXWindow glx_window = 0;
    Pixmap pixmap = 0;
    GLXPixmap glx_pixmap = 0;
    Pixmap single_pixmap = 0;
    GLXPixmap single_glx_pixmap = 0;
    XImage *image = NULL;
    unsigned long pixel;
    unsigned long red;
    unsigned long green;
    unsigned long blue;
    int screen;
    int config_count = 0;
    int single_config_count = 0;
    int result = 1;

    if (display == NULL) {
        fprintf(stderr, "cannot open launcher display\n");
        return 2;
    }
    screen = DefaultScreen(display);
    configs = glXChooseFBConfig(display, screen, config_attributes,
                                &config_count);
    if (configs == NULL || config_count < 1)
        goto cleanup;
    visual = glXGetVisualFromFBConfig(display, configs[0]);
    if (visual == NULL)
        goto cleanup;
    window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                 0, 0, 64, 64, 0, 0, 0);
    if (window == 0)
        goto cleanup;
    XMapWindow(display, window);
    XSync(display, False);

    glx_window = glXCreateWindow(display, configs[0], window, NULL);
    context = glXCreateNewContext(display, configs[0], GLX_RGBA_TYPE,
                                  NULL, True);
    if (context == NULL || !glXIsDirect(display, context) ||
        glx_window == 0 || !glXMakeCurrent(display, glx_window, context))
        goto cleanup;
    {
        PFNGLXSWAPINTERVALEXTPROC set_swap_interval =
            (PFNGLXSWAPINTERVALEXTPROC) glXGetProcAddress(
                (const GLubyte *) "glXSwapIntervalEXT");
        unsigned int interval = UINT_MAX;
        unsigned int maximum = 0;

        if (set_swap_interval == NULL)
            goto cleanup;
        set_swap_interval(display, glx_window, 1);
        glXQueryDrawable(display, glx_window, GLX_SWAP_INTERVAL_EXT,
                         &interval);
        glXQueryDrawable(display, glx_window, GLX_MAX_SWAP_INTERVAL_EXT,
                         &maximum);
        if (interval != 1 || maximum < 1)
            goto cleanup;
        set_swap_interval(display, glx_window, 0);
    }
    glViewport(0, 0, 64, 64);
    glClearColor(0.0F, 1.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glXSwapBuffers(display, glx_window);
    XSync(display, False);

    image = XGetImage(display, window, 32, 32, 1, 1, AllPlanes, ZPixmap);
    if (image == NULL)
        goto cleanup;
    pixel = XGetPixel(image, 0, 0);
    red = pixel & visual->red_mask;
    green = pixel & visual->green_mask;
    blue = pixel & visual->blue_mask;
    if (green == 0 || red != 0 || blue != 0)
        goto cleanup;

    XDestroyImage(image);
    image = NULL;
    XResizeWindow(display, window, 80, 72);
    XSync(display, False);
    {
        unsigned int width = 0;
        unsigned int height = 0;

        glXQueryDrawable(display, glx_window, GLX_WIDTH, &width);
        glXQueryDrawable(display, glx_window, GLX_HEIGHT, &height);
        if (width != 80 || height != 72)
            goto cleanup;
    }
    glViewport(0, 0, 80, 72);
    glClearColor(0.0F, 0.0F, 1.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glXSwapBuffers(display, glx_window);
    XSync(display, False);

    image = XGetImage(display, window, 40, 36, 1, 1, AllPlanes, ZPixmap);
    if (image == NULL)
        goto cleanup;
    pixel = XGetPixel(image, 0, 0);
    red = pixel & visual->red_mask;
    green = pixel & visual->green_mask;
    blue = pixel & visual->blue_mask;
    if (blue == 0 || red != 0 || green != 0)
        goto cleanup;

    XDestroyImage(image);
    image = NULL;
    pixmap = XCreatePixmap(display, RootWindow(display, screen), 32, 32,
                           (unsigned int) DefaultDepth(display, screen));
    glx_pixmap = glXCreatePixmap(display, configs[0], pixmap, NULL);
    if (pixmap == 0 || glx_pixmap == 0 ||
        !glXMakeCurrent(display, glx_pixmap, context))
        goto cleanup;
    glViewport(0, 0, 32, 32);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glXSwapBuffers(display, glx_pixmap);
    XSync(display, False);
    image = XGetImage(display, pixmap, 16, 16, 1, 1, AllPlanes, ZPixmap);
    if (image == NULL)
        goto cleanup;
    pixel = XGetPixel(image, 0, 0);
    red = pixel & visual->red_mask;
    green = pixel & visual->green_mask;
    blue = pixel & visual->blue_mask;
    if (red == 0 || green != 0 || blue != 0)
        goto cleanup;

    XDestroyImage(image);
    image = NULL;
    single_configs = glXChooseFBConfig(display, screen,
                                       single_config_attributes,
                                       &single_config_count);
    if (single_configs == NULL || single_config_count < 1)
        goto cleanup;
    single_context = glXCreateNewContext(display, single_configs[0],
                                         GLX_RGBA_TYPE, NULL, True);
    single_pixmap = XCreatePixmap(display, RootWindow(display, screen),
                                  32, 32,
                                  (unsigned int) DefaultDepth(display, screen));
    single_glx_pixmap = glXCreatePixmap(display, single_configs[0],
                                        single_pixmap, NULL);
    if (single_context == NULL || single_pixmap == 0 ||
        single_glx_pixmap == 0 ||
        !glXMakeCurrent(display, single_glx_pixmap, single_context))
        goto cleanup;
    glViewport(0, 0, 32, 32);
    glClearColor(0.0F, 1.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    XSync(display, False);
    image = XGetImage(display, single_pixmap, 16, 16, 1, 1, AllPlanes,
                      ZPixmap);
    if (image == NULL)
        goto cleanup;
    pixel = XGetPixel(image, 0, 0);
    red = pixel & visual->red_mask;
    green = pixel & visual->green_mask;
    blue = pixel & visual->blue_mask;
    if (green == 0 || red != 0 || blue != 0)
        goto cleanup;
    result = 0;

cleanup:
    if (image != NULL)
        XDestroyImage(image);
    if (context != NULL || single_context != NULL)
        glXMakeCurrent(display, 0, NULL);
    if (single_context != NULL)
        glXDestroyContext(display, single_context);
    if (context != NULL)
        glXDestroyContext(display, context);
    if (single_glx_pixmap != 0)
        glXDestroyPixmap(display, single_glx_pixmap);
    if (single_pixmap != 0)
        XFreePixmap(display, single_pixmap);
    if (glx_pixmap != 0)
        glXDestroyPixmap(display, glx_pixmap);
    if (pixmap != 0)
        XFreePixmap(display, pixmap);
    if (glx_window != 0)
        glXDestroyWindow(display, glx_window);
    if (window != 0)
        XDestroyWindow(display, window);
    if (configs != NULL)
        XFree(configs);
    if (single_configs != NULL)
        XFree(single_configs);
    if (visual != NULL)
        XFree(visual);
    XCloseDisplay(display);
    return result;
}
