#include <OSMesa/osmesa.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

static unsigned char *
pixel(unsigned char *buffer, int width, int x, int y)
{
    return buffer + ((size_t) y * (size_t) width + (size_t) x) * 4U;
}

static int
is_color(const unsigned char value[4], unsigned char red,
         unsigned char green, unsigned char blue)
{
    return value[0] == red && value[1] == green && value[2] == blue &&
        value[3] == 255;
}

int
main(void)
{
    unsigned char first[4 * 4 * 4];
    unsigned char second[2 * 3 * 4];
    unsigned char value[4] = { 0, 0, 0, 0 };
    GLfloat depth = 0.0F;
    OSMesaContext context;
    int x;
    int y;

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            unsigned char *current = pixel(first, 4, x, y);
            current[1] = 255;
            current[3] = 255;
        }
    }

    context = OSMesaCreateContextExt(OSMESA_RGBA, 24, 8, 0, NULL);
    if (!context ||
        OSMesaGetProcAddress("OSMesaMakeCurrentSeparate") == NULL ||
        !OSMesaMakeCurrent(context, first, GL_UNSIGNED_BYTE, 4, 4))
        return 1;

    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    glReadPixels(1, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    if (glGetError() != GL_NO_ERROR || fabsf(depth - 0.25F) > 0.001F)
        return 2;

    if (!OSMesaMakeCurrentSeparate(context,
                                   second, 2, 3,
                                   first, 4, 4,
                                   GL_UNSIGNED_BYTE))
        return 3;
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value);
    if (glGetError() != GL_NO_ERROR || !is_color(value, 0, 255, 0))
        return 4;

    glReadPixels(1, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    if (glGetError() != GL_INVALID_OPERATION)
        return 5;

    OSMesaPixelStore(OSMESA_Y_UP, 0);
    pixel(first, 4, 0, 3)[0] = 0;
    pixel(first, 4, 0, 3)[1] = 0;
    pixel(first, 4, 0, 3)[2] = 255;
    pixel(first, 4, 0, 3)[3] = 255;
    memset(value, 0, sizeof(value));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value);
    if (glGetError() != GL_NO_ERROR || !is_color(value, 0, 0, 255))
        return 6;

    OSMesaPixelStore(OSMESA_Y_UP, 1);
    if (!OSMesaMakeCurrent(context, second, GL_UNSIGNED_BYTE, 2, 3))
        return 7;
    memset(value, 0, sizeof(value));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, value);
    if (glGetError() != GL_NO_ERROR || !is_color(value, 255, 0, 0))
        return 8;

    glClearDepth(0.75);
    glClear(GL_DEPTH_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    if (glGetError() != GL_NO_ERROR || fabsf(depth - 0.75F) > 0.001F)
        return 9;

    if (!OSMesaMakeCurrentSeparate(context,
                                   second, 2, 3,
                                   second, 2, 3,
                                   GL_UNSIGNED_BYTE) ||
        !OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0))
        return 10;
    OSMesaDestroyContext(context);
    return 0;
}
