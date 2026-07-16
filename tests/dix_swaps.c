#include "dixstruct.h"
#include "extnsionst.h"
#include "misc.h"
#include "swapreq.h"

#include <stdint.h>
#include <stdlib.h>

int (*ProcVector[256])(ClientPtr client);
EventSwapPtr EventSwapVector[128];

void
NotImplemented(xEvent *from, xEvent *to)
{
    (void) from;
    (void) to;
    abort();
}

int
main(void)
{
    CARD32 longs[] = {
        0x01020304U, 0x11223344U, 0xa0b0c0d0U, 0x00000000U,
        0xffffffffU, 0x89abcdefU, 0x13579bdfU, 0x2468ace0U,
        0x10203040U
    };
    CARD16 shorts[] = {
        0x0102, 0x1122, 0x3040, 0x5060, 0x7080, 0x90a0,
        0xb0c0, 0xd0e0, 0xf001, 0x1234, 0x2345, 0x3456,
        0x4567, 0x5678, 0x6789, 0x789a, 0xabcd
    };
    xColorItem color = {
        .pixel = 0x01020304U,
        .red = 0x1122,
        .green = 0x3344,
        .blue = 0x5566,
        .flags = DoRed | DoGreen,
        .pad = 0x7a
    };
    xConnClientPrefix prefix = {
        .byteOrder = 'l',
        .majorVersion = 0x0102,
        .minorVersion = 0x0304,
        .nbytesAuthProto = 0x0506,
        .nbytesAuthString = 0x0708
    };

    SwapLongs(longs, sizeof(longs) / sizeof(longs[0]));
    if (longs[0] != 0x04030201U || longs[5] != 0xefcdab89U ||
        longs[8] != 0x40302010U)
        return 1;

    SwapShorts((short *) shorts, sizeof(shorts) / sizeof(shorts[0]));
    if (shorts[0] != 0x0201 || shorts[15] != 0x9a78 ||
        shorts[16] != 0xcdab)
        return 2;

    SwapColorItem(&color);
    if (color.pixel != 0x04030201U || color.red != 0x2211 ||
        color.green != 0x4433 || color.blue != 0x6655 ||
        color.flags != (DoRed | DoGreen) || color.pad != 0x7a)
        return 3;

    SwapConnClientPrefix(&prefix);
    if (prefix.byteOrder != 'l' || prefix.majorVersion != 0x0201 ||
        prefix.minorVersion != 0x0403 || prefix.nbytesAuthProto != 0x0605 ||
        prefix.nbytesAuthString != 0x0807)
        return 4;

    return 0;
}
