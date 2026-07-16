#include "regionstr.h"

#include <stdarg.h>
#include <stdio.h>

/* RegionPrint() is in the same upstream translation unit as the region core. */
void
ErrorF(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

static int
box_is(const BoxRec *box, int x1, int y1, int x2, int y2)
{
    return box->x1 == x1 && box->y1 == y1 &&
           box->x2 == x2 && box->y2 == y2;
}

int
main(void)
{
    BoxRec left_box = { 0, 0, 10, 10 };
    BoxRec right_box = { 5, 5, 15, 15 };
    RegionPtr left;
    RegionPtr right;
    RegionPtr overlap;
    RegionPtr duplicate;

    InitRegions();
    left = RegionCreate(&left_box, 1);
    right = RegionCreate(&right_box, 1);
    overlap = RegionCreate(NULL, 1);
    if (RegionBroken(left) || RegionBroken(right) || RegionBroken(overlap))
        return 1;

    if (!RegionIntersect(overlap, left, right) ||
        RegionNumRects(overlap) != 1 ||
        !box_is(RegionExtents(overlap), 5, 5, 10, 10))
        return 2;

    duplicate = RegionDuplicate(overlap);
    if (!duplicate || RegionBroken(duplicate) || !RegionEqual(duplicate, overlap))
        return 3;

    RegionTranslate(duplicate, 3, -2);
    if (!box_is(RegionExtents(duplicate), 8, 3, 13, 8) ||
        !RegionContainsPoint(duplicate, 9, 4, NULL) ||
        RegionContainsPoint(duplicate, 5, 5, NULL))
        return 4;

    if (!RegionUnion(overlap, left, right) ||
        !box_is(RegionExtents(overlap), 0, 0, 15, 15) ||
        RegionNumRects(overlap) != 3)
        return 5;

    RegionDestroy(duplicate);
    RegionDestroy(overlap);
    RegionDestroy(right);
    RegionDestroy(left);
    return 0;
}
