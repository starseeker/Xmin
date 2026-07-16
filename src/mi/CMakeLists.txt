# Machine-independent drawing, screens, colormaps, cursors, and initialization.
set(xmin_mi_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/mi")
set(xmin_mi_sources
  miarc.c
  mibitblt.c
  micmap.c
  micopy.c
  midash.c
  midispcur.c
  mieq.c
  miexpose.c
  mifillarc.c
  mifillrct.c
  migc.c
  miglblt.c
  mioverlay.c
  mipointer.c
  mipoly.c
  mipolypnt.c
  mipolyrect.c
  mipolyseg.c
  mipolytext.c
  mipushpxl.c
  miscrinit.c
  misprite.c
  mivaltree.c
  miwideline.c
  miwindow.c
  mizerarc.c
  mizerclip.c
  mizerline.c
)
list(TRANSFORM xmin_mi_sources PREPEND "${xmin_mi_source_dir}/")

xmin_add_component(mi
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_mi_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES Xmin::xserver_headers Xmin::dix
)
