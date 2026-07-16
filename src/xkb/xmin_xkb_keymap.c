#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <X11/X.h>
#include <X11/extensions/XKM.h>

#include "os.h"
#include "xkbsrv.h"
#include "xmin_xkb_keymap.h"
#include "xmin-us-map.h"

#define XMIN_EMBEDDED_XKM_MASK                                               \
    (XkmTypesMask | XkmCompatMapMask | XkmSymbolsMask | XkmIndicatorsMask | \
     XkmKeyNamesMask | XkmVirtualModsMask)

XkbDescPtr
XminXkbCreateEmbeddedMap(void)
{
    XkbDescPtr result = NULL;
    FILE *stream;
    unsigned int missing;

    stream = fmemopen(xmin_us_xkm, sizeof(xmin_us_xkm), "rb");
    if (!stream)
        return NULL;

    missing = XkmReadFile(stream, XMIN_EMBEDDED_XKM_MASK,
                          XMIN_EMBEDDED_XKM_MASK, &result);
    fclose(stream);
    if (!result || (missing & XMIN_EMBEDDED_XKM_MASK) != 0) {
        if (result)
            XkbFreeKeyboard(result, XkbAllComponentsMask, TRUE);
        return NULL;
    }

    result->device_spec = XkbUseCoreKbd;
    return result;
}

static Bool
xmin_component_supported(const char *requested, const char *embedded)
{
    return requested == NULL || strcmp(requested, "%") == 0 ||
           strcmp(requested, embedded) == 0;
}

unsigned int
XkbDDXLoadKeymapByNames(DeviceIntPtr keybd, XkbComponentNamesPtr names,
                        unsigned int want, unsigned int need,
                        XkbDescPtr *xkb_return, char *name_return,
                        int name_return_length)
{
    const unsigned int available = XMIN_EMBEDDED_XKM_MASK;

    (void) keybd;
    if (!xkb_return)
        return 0;
    *xkb_return = NULL;

    if (!names || (need & ~available) != 0 ||
        !xmin_component_supported(names->keycodes, "evdev+aliases(qwerty)") ||
        !xmin_component_supported(names->types, "complete") ||
        !xmin_component_supported(names->compat, "complete") ||
        !xmin_component_supported(names->symbols, "pc+us") ||
        !xmin_component_supported(names->geometry, "xmin"))
        return 0;

    *xkb_return = XminXkbCreateEmbeddedMap();
    if (!*xkb_return)
        return 0;

    if (name_return && name_return_length > 0)
        (void) snprintf(name_return, (size_t) name_return_length, "%s", "xmin-us");
    return available & (want | need);
}

Bool
XkbDDXNamesFromRules(DeviceIntPtr keybd, const char *rules_name,
                     XkbRF_VarDefsPtr defs, XkbComponentNamesPtr names)
{
    (void) keybd;
    (void) defs;
    if (!rules_name || !names)
        return FALSE;

    memset(names, 0, sizeof(*names));
    names->keycodes = Xstrdup("evdev+aliases(qwerty)");
    names->types = Xstrdup("complete");
    names->compat = Xstrdup("complete");
    names->symbols = Xstrdup("pc+us");
    if (!names->keycodes || !names->types || !names->compat || !names->symbols) {
        XkbFreeComponentNames(names, FALSE);
        return FALSE;
    }
    return TRUE;
}

XkbDescPtr
XkbCompileKeymap(DeviceIntPtr dev, XkbRMLVOSet *rmlvo)
{
    if (!dev || !rmlvo) {
        LogMessage(X_ERROR, "XKB: no device or RMLVO specified\n");
        return NULL;
    }
    return XminXkbCreateEmbeddedMap();
}

XkbDescPtr
XkbCompileKeymapFromString(DeviceIntPtr dev, const char *keymap,
                           int keymap_length)
{
    if (!dev || !keymap || keymap_length < 0) {
        LogMessage(X_ERROR, "XKB: no device or keymap specified\n");
        return NULL;
    }

    LogMessage(X_WARNING,
               "XKB: custom textual maps are unavailable; using Xmin's "
               "embedded US map\n");
    return XminXkbCreateEmbeddedMap();
}
