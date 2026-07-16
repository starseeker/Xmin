/* This focused test intentionally uses assert for both calls and checks. */
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dix-config.h>
#include <X11/keysym.h>
#include <X11/extensions/XKM.h>
#include "dix.h"
#include "xkbsrv.h"
#include "xmin_xkb_keymap.h"

void
ErrorF(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void
FatalError(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

char *
Xstrdup(const char *source)
{
    size_t length = strlen(source) + 1;
    char *copy = malloc(length);

    if (copy)
        memcpy(copy, source, length);
    return copy;
}

int
main(void)
{
    XkbDescPtr first;
    XkbDescPtr second;
    XkbComponentNamesRec requested = { 0 };
    XkbDescPtr loaded = NULL;
    char map_name[16] = { 0 };
    unsigned int found;

    InitAtoms();
    assert(XkbConvertGetByNameComponents(TRUE, XkbGBN_TypesMask) ==
           XkmTypesMask);
    assert((XkbConvertGetByNameComponents(FALSE, XkmKeyNamesMask) &
            XkbGBN_KeyNamesMask) != 0);
    first = XminXkbCreateEmbeddedMap();
    second = XminXkbCreateEmbeddedMap();
    assert(first != NULL && second != NULL);

    assert(first->min_key_code == 8);
    assert(first->max_key_code == 255);
    assert(first->geom == NULL);
    assert((first->defined & XkmKeymapRequired) == XkmKeymapRequired);
    assert(first->map != NULL && first->server != NULL);
    assert(first->compat != NULL && first->indicators != NULL);
    assert(first->names != NULL && first->names->keys != NULL);
    assert(memcmp(first->names->keys[38].name, "AC01", 4) == 0);

    assert(XkbKeyNumSyms(first, 38) >= 2);
    assert(XkbKeySym(first, 38, 0) == XK_a);
    assert(XkbKeySym(first, 38, 1) == XK_A);
    assert(first->map->key_sym_map[38].kt_index[0] == XkbAlphabeticIndex);
    assert(XkbKeySym(first, 36, 0) == XK_Return);
    assert(XkbKeySym(first, 65, 0) == XK_space);
    assert(first->map->modmap[50] == ShiftMask);
    assert(first->map->modmap[37] == ControlMask);
    assert(first->map->modmap[64] == Mod1Mask);
    assert(first->map->modmap[133] == Mod4Mask);
    assert(first->names->vmods[0] != None);

    XkbKeySym(first, 38, 0) = XK_z;
    assert(XkbKeySym(second, 38, 0) == XK_a);

    requested.keycodes = "%";
    requested.types = "%";
    requested.compat = "%";
    requested.symbols = "%";
    found = XkbDDXLoadKeymapByNames(NULL, &requested, XkmKeymapLegal,
                                    XkmKeymapRequired, &loaded,
                                    map_name, sizeof(map_name));
    assert((found & XkmKeymapRequired) == XkmKeymapRequired);
    assert(loaded != NULL);
    assert(strcmp(map_name, "xmin-us") == 0);
    XkbFreeKeyboard(loaded, XkbAllComponentsMask, TRUE);

    loaded = NULL;
    found = XkbDDXLoadKeymapByNames(NULL, &requested, XkmGeometryMask,
                                    XkmGeometryMask, &loaded, NULL, 0);
    assert(found == 0 && loaded == NULL);

    XkbFreeKeyboard(first, XkbAllComponentsMask, TRUE);
    XkbFreeKeyboard(second, XkbAllComponentsMask, TRUE);
    FreeAllAtoms();
    return 0;
}
