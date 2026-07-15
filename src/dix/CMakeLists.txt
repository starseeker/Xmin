# X server clients, resources, windows, events, devices, and dispatch.
# Atom handling is the first compiled tranche; more DIX units will be added here.
xmin_add_component(dix
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/atom.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/dix/initatoms.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES Xmin::xserver_headers
)
