#include <dix-config.h>

#include "rrtransform.h"

#include <stdint.h>

int
main(void)
{
    RRTransformRec first;
    RRTransformRec second;
    PictTransform computed;
    xFixed parameters[] = {
        pixman_int_to_fixed(2),
        pixman_int_to_fixed(3),
    };

    RRTransformInit(&first);
    RRTransformInit(&second);
    if (!RRTransformEqual(NULL, &first))
        return 1;

    if (RRTransformCompute(11, 17, 640, 480, RR_Rotate_0, NULL,
                           &computed, NULL, NULL)) {
        return 2;
    }
    if (computed.matrix[0][2] != pixman_int_to_fixed(11) ||
        computed.matrix[1][2] != pixman_int_to_fixed(17)) {
        return 3;
    }

    if (!RRTransformSetFilter(&first, (PictFilterPtr) (uintptr_t) 1,
                              parameters, 2, 2, 1)) {
        return 4;
    }
    if (!RRTransformCopy(&second, &first) || !RRTransformEqual(&first, &second))
        return 5;
    if (first.params == second.params)
        return 6;

    if (!RRTransformCompute(0, 0, 640, 480, RR_Rotate_90, NULL,
                            &computed, NULL, NULL)) {
        return 7;
    }

    RRTransformFini(&second);
    RRTransformFini(&first);
    return 0;
}
