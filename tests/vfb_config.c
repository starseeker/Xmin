#include <dix-config.h>

#ifndef XMIN_TEST_WIDTH
#error "Expected vfb width was not provided by CMake"
#endif

#if XMIN_DEFAULT_WIDTH != XMIN_TEST_WIDTH
#error "The generated vfb width does not match the CMake cache"
#endif
#if XMIN_DEFAULT_HEIGHT != XMIN_TEST_HEIGHT
#error "The generated vfb height does not match the CMake cache"
#endif
#if XMIN_DEFAULT_DEPTH != XMIN_TEST_DEPTH
#error "The generated vfb depth does not match the CMake cache"
#endif
#if XMIN_DEFAULT_DPI != XMIN_TEST_DPI
#error "The generated vfb DPI does not match the CMake cache"
#endif
#if INPUTTHREAD
#error "Xmin's virtual DDX must not enable the hardware input thread"
#endif

#if !BIGREQS || !COMPOSITE || !DAMAGE || !DBE || !PRESENT || !RANDR || \
    !RENDER || !SCREENSAVER || !SHAPE || !XCMISC || !XFIXES || !XINERAMA || \
    !XSYNC || !XTEST
#error "Xmin's required toolkit-facing extension baseline is incomplete"
#endif

int
main(void)
{
    return XMIN_DEFAULT_WIDTH > 0 && XMIN_DEFAULT_HEIGHT > 0 &&
        XMIN_DEFAULT_DPI > 0 ? 0 : 1;
}
