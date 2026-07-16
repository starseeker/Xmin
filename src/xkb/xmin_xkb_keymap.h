#ifndef XMIN_XKB_KEYMAP_H
#define XMIN_XKB_KEYMAP_H

#include "xkbstr.h"

/* Return a fully heap-owned copy of Xmin's compiled-in US keyboard map. */
XkbDescPtr XminXkbCreateEmbeddedMap(void);

#endif
