#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>

#include <X11/X.h>
#include <X11/keysym.h>
#include <X11/extensions/XKM.h>

#include "misc.h"
#include "xkbsrv.h"

/* xkmread uses the libxkbfile classification API. Reuse the server's retained
 * case conversion instead of linking libxkbfile's unrelated text emitters. */
unsigned int
_XkbKSCheckCase(KeySym keysym)
{
    KeySym lower;
    KeySym upper;
    unsigned int result = 0;

    XkbConvertCase(keysym, &lower, &upper);
    if (lower == upper)
        return 0;
    if (keysym == lower)
        result |= _XkbKSLower;
    if (keysym == upper)
        result |= _XkbKSUpper;
    return result;
}

unsigned int
XkbConvertGetByNameComponents(Bool to_xkm, unsigned int original)
{
    static const unsigned int protocol_bits[] = {
        XkbGBN_TypesMask,
        XkbGBN_CompatMapMask,
        XkbGBN_SymbolsMask,
        XkbGBN_IndicatorMapMask,
        XkbGBN_KeyNamesMask,
        XkbGBN_GeometryMask,
    };
    static const unsigned int xkm_bits[] = {
        XkmTypesMask,
        XkmCompatMapMask,
        XkmSymbolsMask,
        XkmIndicatorsMask,
        XkmKeyNamesMask,
        XkmGeometryMask,
    };
    unsigned int result = 0;
    unsigned int i;

    for (i = 0; i < sizeof(protocol_bits) / sizeof(protocol_bits[0]); ++i) {
        unsigned int from = to_xkm ? protocol_bits[i] : xkm_bits[i];
        unsigned int to = to_xkm ? xkm_bits[i] : protocol_bits[i];

        if (original & from)
            result |= to;
    }
    if (!to_xkm && original != 0)
        result |= XkbGBN_OtherNamesMask;
    return result;
}

char *
XkbConfigText(unsigned int config, unsigned int format)
{
    static char semantics[] = "Semantics";
    static char layout[] = "Layout";
    static char keymap[] = "Keymap";
    static char geometry[] = "Geometry";
    static char types[] = "Types";
    static char compat[] = "CompatMap";
    static char symbols[] = "Symbols";
    static char indicators[] = "Indicators";
    static char key_names[] = "KeyNames";
    static char virtual_mods[] = "VirtualMods";
    static char unknown[32];

    (void) format;
    switch (config) {
    case XkmSemanticsFile:
        return semantics;
    case XkmLayoutFile:
        return layout;
    case XkmKeymapFile:
        return keymap;
    case XkmGeometryFile:
    case XkmGeometryIndex:
        return geometry;
    case XkmTypesIndex:
        return types;
    case XkmCompatMapIndex:
        return compat;
    case XkmSymbolsIndex:
        return symbols;
    case XkmIndicatorsIndex:
        return indicators;
    case XkmKeyNamesIndex:
        return key_names;
    case XkmVirtualModsIndex:
        return virtual_mods;
    default:
        (void) snprintf(unknown, sizeof(unknown), "unknown(%u)", config);
        return unknown;
    }
}
