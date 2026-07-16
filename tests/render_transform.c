#include <dix-config.h>

#include "picturestr.h"

#include <string.h>

int
main(void)
{
    xRenderTransform wire = {
        .matrix11 = pixman_int_to_fixed(1),
        .matrix12 = 0,
        .matrix13 = pixman_int_to_fixed(3),
        .matrix21 = 0,
        .matrix22 = pixman_int_to_fixed(1),
        .matrix23 = pixman_int_to_fixed(-2),
        .matrix31 = 0,
        .matrix32 = 0,
        .matrix33 = pixman_int_to_fixed(1),
    };
    xRenderTransform round_trip;
    PictTransform transform;
    PictVector point = {
        .vector = {
            pixman_int_to_fixed(5),
            pixman_int_to_fixed(7),
            pixman_int_to_fixed(1),
        }
    };

    PictTransform_from_xRenderTransform(&transform, &wire);
    memset(&round_trip, 0, sizeof(round_trip));
    xRenderTransform_from_PictTransform(&round_trip, &transform);
    if (memcmp(&wire, &round_trip, sizeof(wire)) != 0)
        return 1;

    if (!PictureTransformPoint(&transform, &point))
        return 2;
    if (point.vector[0] != pixman_int_to_fixed(8) ||
        point.vector[1] != pixman_int_to_fixed(5) ||
        point.vector[2] != pixman_int_to_fixed(1)) {
        return 3;
    }

    return 0;
}
