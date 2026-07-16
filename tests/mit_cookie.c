#include <dix-config.h>

#include <X11/X.h>

#include "dixstruct.h"
#include "osdep.h"

#include <string.h>

/* MitGenerateCookie shares this hook with auth.c.  The test exercises only
 * stored-cookie behavior, so keep the rest of the server OS archive unpulled. */
void
GenerateRandomData(int length, char *data)
{
    memset(data, 0, (size_t) length);
}

int
main(void)
{
    static const char cookie[] = "0123456789abcdef";
    static const char wrong[] = "0123456789abcdeg";
    const XID id = 0x10203040;
    const char *reason = NULL;
    unsigned short length = 0;
    char *data = NULL;

    MitResetCookie();
    if (!MitAddCookie(sizeof(cookie) - 1, cookie, id))
        return 1;
    if (MitCheckCookie(sizeof(cookie) - 1, cookie, NULL, &reason) != id)
        return 2;
    if (MitCheckCookie(sizeof(wrong) - 1, wrong, NULL, &reason) != (XID) -1 ||
        reason == NULL || strcmp(reason, "Invalid MIT-MAGIC-COOKIE-1 key") != 0)
        return 3;
    if (!MitFromID(id, &length, &data) || length != sizeof(cookie) - 1 ||
        memcmp(data, cookie, length) != 0)
        return 4;
    if (!MitRemoveCookie(sizeof(cookie) - 1, cookie))
        return 5;
    if (MitCheckCookie(sizeof(cookie) - 1, cookie, NULL, &reason) != (XID) -1)
        return 6;
    return MitResetCookie();
}
