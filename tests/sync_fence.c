#include <assert.h>
#include <string.h>

#include "scrnintstr.h"
#include "syncsrv.h"
#include "misync.h"

int
main(void)
{
    SyncFence fence;

    memset(&fence, 0, sizeof(fence));
    assert(!miSyncFenceCheckTriggered(&fence));

    miSyncFenceSetTriggered(&fence);
    assert(miSyncFenceCheckTriggered(&fence));

    miSyncFenceReset(&fence);
    assert(!miSyncFenceCheckTriggered(&fence));

    return 0;
}
