set(xmin_xorg_glx_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/glx")
set(xmin_xorg_glx_sources
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/Xext/hashtable.c"
  indirect_dispatch.c
  indirect_dispatch_swap.c
  indirect_reqsize.c
  indirect_size_get.c
  indirect_table.c
  clientinfo.c
  createcontext.c
  extension_string.c
  indirect_util.c
  indirect_program.c
  indirect_texture_compression.c
  glxcmds.c
  glxcmdsswap.c
  glxext.c
  glxscreens.c
  render2.c
  render2swap.c
  renderpix.c
  renderpixswap.c
  rensize.c
  single2.c
  single2swap.c
  singlepix.c
  singlepixswap.c
  singlesize.c
  swap_interval.c
  xfont.c
  vndcmds.c
  vndext.c
  vndservermapping.c
  vndservervendor.c
)
list(TRANSFORM xmin_xorg_glx_sources PREPEND "${xmin_xorg_glx_dir}/"
  REGEX "^[^/]"
)

# The protocol dispatcher is upstream Xorg code. The provider is deliberately
# separate so project warning policy continues to apply to the Xmin-owned
# drawable/context bridge.
xmin_add_component(glx_protocol
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xorg_glx_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${xmin_xorg_glx_dir}"
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::xfont2
    Xmin::osmesa
)

xmin_add_component(glx
  SOURCES xmin_glx_osmesa.c
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${xmin_xorg_glx_dir}"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/glapi"
  PUBLIC_LIBRARIES
    Xmin::glx_protocol
    Xmin::xserver_headers
    Xmin::osmesa
)
