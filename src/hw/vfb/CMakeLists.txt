# Virtual framebuffer DDX, output initialization, and virtual input devices.
xmin_add_component(vfb
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/hw/vfb/InitInput.c"
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/hw/vfb/InitOutput.c"
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::dix
    Xmin::mi
    Xmin::fb
    Xmin::extensions
)
