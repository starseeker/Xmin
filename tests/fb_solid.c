#include <dix-config.h>

#include "fb.h"

int
main(void)
{
    FbBits pixels[12];
    FbBits partial = 0;
    FbBits rop = 0xf0f0f0f0U;
    const FbBits untouched = 0xdeadbeefU;
    const FbBits color = 0x11223344U;
    int i;

    for (i = 0; i < 12; ++i)
        pixels[i] = untouched;

    /* Two 32-bit pixels on each of two rows, with a four-word stride. */
    fbSolid(pixels, 4, 32, 32, 64, 2, 0, color);
    for (i = 0; i < 12; ++i) {
        Bool should_be_filled = i == 1 || i == 2 || i == 5 || i == 6;

        if (pixels[i] != (should_be_filled ? color : untouched))
            return 1;
    }

    /* Exercise both edge masks using a 16-bit span inside one fb word. */
    fbSolid(&partial, 1, 8, 8, 16, 1, 0, FB_ALLONES);
    if (partial != FbBitsMask(8, 16))
        return 2;

    /* A non-copy raster operation must preserve the specified destination bits. */
    fbSolid(&rop, 1, 0, 32, 32, 1, 0x0f0f0f0fU, 0x33333333U);
    if (rop != ((0xf0f0f0f0U & 0x0f0f0f0fU) ^ 0x33333333U))
        return 3;

    return 0;
}
