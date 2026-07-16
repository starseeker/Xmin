# Portable X server bookkeeping used by later dispatch, rendering, and DDX layers.
# Keeping this in a static archive lets focused tests pull individual upstream
# translation units without requiring the not-yet-imported server as a whole.
xmin_add_component(dix
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/atom.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/colormap.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/cursor.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/devices.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/dispatch.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/dixfonts.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/dixutils.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/enterleave.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/eventconvert.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/events.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/extension.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/gc.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/gestures.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/getevents.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/globals.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/glyphcurs.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/grabs.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/initatoms.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/inpututils.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/main.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/pixmap.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/privates.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/property.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/ptrveloc.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/region.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/registry.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/resource.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/selection.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/swaprep.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/swapreq.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/tables.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/touch.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/window.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES Xmin::xserver_headers Xmin::xfont2
)
