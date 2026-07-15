#include <pixman.h>

int
main(void)
{
    pixman_region32_t first;
    pixman_region32_t second;
    pixman_region32_t intersection;
    const pixman_box32_t *bounds;
    int result = 1;

    pixman_region32_init_rect(&first, 0, 0, 20, 20);
    pixman_region32_init_rect(&second, 5, 7, 20, 20);
    pixman_region32_init(&intersection);

    if (!pixman_region32_intersect(&intersection, &first, &second))
        goto done;

    bounds = pixman_region32_extents(&intersection);
    if (bounds->x1 == 5 && bounds->y1 == 7 &&
        bounds->x2 == 20 && bounds->y2 == 20)
        result = 0;

done:
    pixman_region32_fini(&intersection);
    pixman_region32_fini(&second);
    pixman_region32_fini(&first);
    return result;
}
