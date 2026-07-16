#include <OSMesa/osmesa.h>
#include <xmin/config.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
check_x_pixel_format(void)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };
    uint32_t pixel = 0;
    OSMesaContext context;

#if XMIN_IS_BIG_ENDIAN
    context = OSMesaCreateContextExt(OSMESA_ARGB, 0, 0, 0, NULL);
#else
    context = OSMesaCreateContextExt(OSMESA_BGRA, 0, 0, 0, NULL);
#endif
    if (context == NULL ||
        !OSMesaMakeCurrent(context, bytes, GL_UNSIGNED_BYTE, 1, 1)) {
        OSMesaDestroyContext(context);
        return 0;
    }
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    memcpy(&pixel, bytes, sizeof(pixel));
    OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
    OSMesaDestroyContext(context);
    return (pixel & UINT32_C(0x00ffffff)) == UINT32_C(0x00ff0000);
}

int
main(void)
{
    enum { width = 16, height = 16, draw_width = 8, draw_height = 12 };
    unsigned char *pixels = calloc((size_t) width * height, 4);
    unsigned char *draw_pixels =
        calloc((size_t) draw_width * draw_height, 4);
    unsigned char read_pixel[4] = { 0 };
    OSMesaContext context;
    const char *version;
    size_t center;
    size_t draw_center;
    int result = 1;

    if (pixels == NULL || draw_pixels == NULL || !check_x_pixel_format())
        return 2;
    context = OSMesaCreateContextExt(OSMESA_RGBA, 16, 8, 0, NULL);
    if (context == NULL)
        goto cleanup;
    if (!OSMesaMakeCurrent(context, pixels, GL_UNSIGNED_BYTE, width, height))
        goto destroy;

    OSMesaPixelStore(OSMESA_Y_UP, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glFinish();

    version = (const char *) glGetString(GL_VERSION);
    center = ((size_t) (height / 2) * width + width / 2) * 4;
    draw_center = ((size_t) (draw_height / 2) * draw_width +
                   draw_width / 2) * 4;
    if (version == NULL || strstr(version, "2.0") == NULL ||
        pixels[center] < 62 || pixels[center] > 65 ||
        pixels[center + 1] < 126 || pixels[center + 1] > 129 ||
        pixels[center + 2] < 190 || pixels[center + 2] > 193 ||
        pixels[center + 3] != 255) {
        fprintf(stderr, "unexpected OSMesa version or rendered pixel\n");
        goto destroy;
    }

    if (!OSMesaMakeCurrentSeparate(context,
                                   draw_pixels, draw_width, draw_height,
                                   pixels, width, height,
                                   GL_UNSIGNED_BYTE))
        goto destroy;
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, read_pixel);
    if (read_pixel[0] < 62 || read_pixel[0] > 65 ||
        read_pixel[1] < 126 || read_pixel[1] > 129 ||
        read_pixel[2] < 190 || read_pixel[2] > 193 ||
        draw_pixels[draw_center] != 0 ||
        draw_pixels[draw_center + 1] < 250 ||
        draw_pixels[draw_center + 2] != 0 ||
        draw_pixels[draw_center + 3] != 255) {
        fprintf(stderr,
                "separate OSMesa draw/read buffers failed: "
                "read=%u,%u,%u,%u draw=%u,%u,%u,%u\n",
                read_pixel[0], read_pixel[1], read_pixel[2], read_pixel[3],
                draw_pixels[draw_center], draw_pixels[draw_center + 1],
                draw_pixels[draw_center + 2],
                draw_pixels[draw_center + 3]);
        goto destroy;
    }
    result = 0;

destroy:
    OSMesaDestroyContext(context);
cleanup:
    free(draw_pixels);
    free(pixels);
    return result;
}
