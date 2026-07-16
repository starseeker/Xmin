# Linear framebuffer renderer and RENDER picture hooks.
set(xmin_fb_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/fb")
set(xmin_fb_sources
  fballpriv.c
  fbarc.c
  fbbits.c
  fbblt.c
  fbbltone.c
  fbcmap_mi.c
  fbcopy.c
  fbfill.c
  fbfillrect.c
  fbfillsp.c
  fbgc.c
  fbgetsp.c
  fbglyph.c
  fbimage.c
  fbline.c
  fboverlay.c
  fbpict.c
  fbpixmap.c
  fbpoint.c
  fbpush.c
  fbscreen.c
  fbseg.c
  fbsetsp.c
  fbsolid.c
  fbtrap.c
  fbutil.c
  fbwindow.c
)
list(TRANSFORM xmin_fb_sources PREPEND "${xmin_fb_source_dir}/")

xmin_add_component(fb
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_fb_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES Xmin::xserver_headers Xmin::dix Xmin::mi
)
