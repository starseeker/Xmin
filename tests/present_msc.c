#include <assert.h>
#include <stdint.h>

#include <X11/Xmd.h>
#include <X11/extensions/presentproto.h>

uint64_t present_get_target_msc(uint64_t target_msc_arg,
                                uint64_t crtc_msc,
                                uint64_t divisor,
                                uint64_t remainder,
                                uint32_t options);

int
main(void)
{
    /* An explicit future MSC always wins. */
    assert(present_get_target_msc(20, 10, 0, 0, 0) == 20);

    /* Synchronous ASAP means the next vblank; async may use the current one. */
    assert(present_get_target_msc(0, 10, 0, 0, 0) == 11);
    assert(present_get_target_msc(0, 10, 0, 0, PresentOptionAsync) == 10);

    /* Modulo scheduling chooses the next matching MSC. */
    assert(present_get_target_msc(0, 10, 4, 3, 0) == 11);
    assert(present_get_target_msc(0, 10, 4, 1, 0) == 13);
    assert(present_get_target_msc(0, 10, 4, 2, 0) == 14);
    assert(present_get_target_msc(0, 10, 4, 2, PresentOptionAsync) == 10);

    return 0;
}
