# RENDER supplies modern alpha compositing, glyphs, gradients, and transforms.
set(xmin_render_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/render")
set(xmin_render_sources
  animcur.c
  filter.c
  glyph.c
  matrix.c
  miindex.c
  mipict.c
  mirect.c
  mitrap.c
  mitri.c
  picture.c
  render.c
)
list(TRANSFORM xmin_render_sources PREPEND "${xmin_render_source_dir}/")

xmin_add_component(render
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_render_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::support
)

# RANDR provides the virtual output, CRTC, mode, monitor, and screen geometry
# model, including its compact single-layout Xinerama adapter.
set(xmin_randr_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/randr")
set(xmin_randr_sources
  randr.c
  rrcrtc.c
  rrdispatch.c
  rrinfo.c
  rrlease.c
  rrmode.c
  rrmonitor.c
  rroutput.c
  rrpointer.c
  rrproperty.c
  rrprovider.c
  rrproviderproperty.c
  rrscreen.c
  rrsdispatch.c
  rrtransform.c
  rrxinerama.c
)
list(TRANSFORM xmin_randr_sources PREPEND "${xmin_randr_source_dir}/")

xmin_add_component(randr
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_randr_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::render
)

# The machine-independent damage tracker is used internally by fb, sprites,
# redirected drawables, Present, and Composite.  damageext.c is
# the matching X DAMAGE wire-protocol implementation.
xmin_add_component(damage
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/damage/damage.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/damageext/damageext.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::render
    Xmin::xfont2
)

# Complete XInput 1.x and XI2 protocol/event implementation plus the generic
# DDX hooks used by virtual servers.  These hooks reject physical-device
# reconfiguration while preserving the virtual core device hierarchy.
set(xmin_xi_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/Xi")
set(xmin_xi_sources
  allowev.c
  chgdctl.c
  chgfctl.c
  chgkbd.c
  chgkmap.c
  chgprop.c
  chgptr.c
  closedev.c
  devbell.c
  exevents.c
  extinit.c
  getbmap.c
  getdctl.c
  getfctl.c
  getfocus.c
  getkmap.c
  getmmap.c
  getprop.c
  getselev.c
  getvers.c
  grabdev.c
  grabdevb.c
  grabdevk.c
  gtmotion.c
  listdev.c
  opendev.c
  queryst.c
  selectev.c
  sendexev.c
  setbmap.c
  setdval.c
  setfocus.c
  setmmap.c
  setmode.c
  stubs.c
  ungrdev.c
  ungrdevb.c
  ungrdevk.c
  xiallowev.c
  xibarriers.c
  xichangecursor.c
  xichangehierarchy.c
  xigetclientpointer.c
  xigrabdev.c
  xipassivegrab.c
  xiproperty.c
  xiquerydevice.c
  xiquerypointer.c
  xiqueryversion.c
  xiselectev.c
  xisetclientpointer.c
  xisetdevfocus.c
  xiwarppointer.c
)
list(TRANSFORM xmin_xi_sources PREPEND "${xmin_xi_source_dir}/")

xmin_add_component(xi
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xi_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
)

# XFIXES supplies regions, cursor/selection notification, pointer barriers,
# save-set behavior, and the client disconnect policy used by the OS layer.
set(xmin_xfixes_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/xfixes")
set(xmin_xfixes_sources
  cursor.c
  disconnect.c
  region.c
  saveset.c
  select.c
  xfixes.c
)
list(TRANSFORM xmin_xfixes_sources PREPEND "${xmin_xfixes_source_dir}/")

xmin_add_component(xfixes
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xfixes_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::render
    Xmin::xi
)

# Present uses Xorg's software-only fake-vblank backend when the DDX supplies
# no CRTC or flip hooks.  That gives clients a monotonic MSC clock and queued
# presentation events without DRI, DRM, or a hardware device.
set(xmin_present_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/present")
set(xmin_present_sources
  present.c
  present_event.c
  present_execute.c
  present_fake.c
  present_fence.c
  present_notify.c
  present_request.c
  present_scmd.c
  present_screen.c
  present_vblank.c
)
list(TRANSFORM xmin_present_sources PREPEND "${xmin_present_source_dir}/")

xmin_add_component(present
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_present_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::randr
    Xmin::xfixes
    Xmin::sync
)

# The server's compact baseline extension set.  Xext contains the wire
# implementations for BIG-REQUESTS, Generic Events, SHAPE, SYNC, XC-MISC,
# XTEST, and (when enabled) MIT-SHM.  The matching MI layer provides software
# fences; fd-backed fences remain inert until a screen explicitly installs
# handlers, so this does not imply DRI or DRM support.
xmin_add_component(sync
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/sync/misync.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/sync/misyncfd.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
)

set(xmin_xext_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/Xext")
set(xmin_xext_sources
  bigreq.c
  geext.c
  saver.c
  shape.c
  sleepuntil.c
  sync.c
  xcmisc.c
  xtest.c
)
if(XMIN_HAVE_MITSHM)
  list(APPEND xmin_xext_sources shm.c)
endif()
list(TRANSFORM xmin_xext_sources PREPEND "${xmin_xext_source_dir}/")

xmin_add_component(xext
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xext_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::sync
    Xmin::xi
    Xmin::xkb
)

# COMPOSITE redirects windows into ordinary software pixmaps and uses the
# already-retained RENDER, DAMAGE, and XFIXES implementations. No compositor,
# GPU, or DRM integration is implied by exposing the protocol.
set(xmin_composite_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/composite")
set(xmin_composite_sources
  compalloc.c
  compext.c
  compinit.c
  compoverlay.c
  compwindow.c
)
list(TRANSFORM xmin_composite_sources PREPEND "${xmin_composite_source_dir}/")

xmin_add_component(composite
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_composite_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::render
    Xmin::damage
    Xmin::xfixes
)

# DOUBLE-BUFFER uses Xorg's machine-independent pixmap backend. The vfb DDX
# installs no driver-specific allocation or swap hooks.
xmin_add_component(dbe
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dbe/dbe.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dbe/midbe.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
)

# miinitext owns the authoritative static extension list used by the server.
xmin_add_component(extension_registry
  UPSTREAM
  TYPE STATIC
  SOURCES "${PROJECT_SOURCE_DIR}/third_party/xorg-server/mi/miinitext.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::xext
    Xmin::render
    Xmin::randr
    Xmin::damage
    Xmin::xi
    Xmin::xkb
    Xmin::xfixes
    Xmin::present
    Xmin::composite
    Xmin::dbe
)

# Aggregate target used by the DDX and server executable.
xmin_add_component(extensions
  UPSTREAM
  PUBLIC_LIBRARIES
    Xmin::extension_registry
    Xmin::xext
    Xmin::sync
    Xmin::xkb
    Xmin::render
    Xmin::randr
    Xmin::damage
    Xmin::xi
    Xmin::xfixes
    Xmin::present
    Xmin::composite
    Xmin::dbe
)
