/* This focused test intentionally uses assert for both calls and checks. */
#undef NDEBUG
#include <assert.h>

#include <dix-config.h>
#include "inputstr.h"
#include "xkbsrv.h"

int
main(void)
{
    XkbDescPtr xkb;
    unsigned int i;

    assert(XkbAllocClientMap(NULL, XkbAllClientInfoMask,
                             XkbNumRequiredTypes) == BadValue);

    xkb = XkbAllocKeyboard();
    assert(xkb != NULL);
    assert(xkb->device_spec == XkbUseCoreKbd);

    xkb->min_key_code = 8;
    xkb->max_key_code = 255;

    assert(XkbAllocClientMap(xkb, XkbAllClientInfoMask,
                             XkbNumRequiredTypes) == Success);
    assert(xkb->map != NULL);
    assert(xkb->map->size_types == XkbNumRequiredTypes);
    assert(xkb->map->num_syms == 1);
    assert(xkb->map->syms[0] == NoSymbol);
    assert(xkb->map->key_sym_map != NULL);
    assert(xkb->map->modmap != NULL);

    assert(XkbAllocServerMap(xkb, XkbAllServerInfoMask, 4) == Success);
    assert(xkb->server != NULL);
    assert(xkb->server->size_acts >= 5);
    for (i = 0; i < XkbNumVirtualMods; ++i)
        assert(xkb->server->vmods[i] == XkbNoModifierMask);

    assert(XkbAllocCompatMap(xkb, XkbAllCompatMask, 2) == Success);
    assert(xkb->compat != NULL);
    assert(xkb->compat->size_si == 2);

    assert(XkbAllocNames(xkb, XkbKeyNamesMask, 0, 0) == Success);
    assert(xkb->names != NULL);
    assert(xkb->names->keys != NULL);

    XkbFreeKeyboard(xkb, XkbAllComponentsMask, TRUE);
    return 0;
}
