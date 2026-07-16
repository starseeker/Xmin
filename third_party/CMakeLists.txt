# Stable dependency targets. Each becomes an audited vendored target when its
# source is imported; none may silently resolve to a host graphics package.
xmin_add_component(xorgproto
  UPSTREAM
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/xorgproto/include"
)

xmin_add_component(xkbfile_headers
  UPSTREAM
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/libxkbfile/include"
  PUBLIC_LIBRARIES Xmin::xorgproto
)

set(xmin_pixman_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/pixman/pixman")
set(xmin_pixman_sources
  pixman.c
  pixman-access.c
  pixman-access-accessors.c
  pixman-arm.c
  pixman-bits-image.c
  pixman-combine32.c
  pixman-combine-float.c
  pixman-conical-gradient.c
  pixman-edge.c
  pixman-edge-accessors.c
  pixman-fast-path.c
  pixman-filter.c
  pixman-glyph.c
  pixman-general.c
  pixman-gradient-walker.c
  pixman-image.c
  pixman-implementation.c
  pixman-linear-gradient.c
  pixman-matrix.c
  pixman-mips.c
  pixman-noop.c
  pixman-ppc.c
  pixman-radial-gradient.c
  pixman-region16.c
  pixman-region32.c
  pixman-region64f.c
  pixman-riscv.c
  pixman-solid-fill.c
  pixman-timer.c
  pixman-trap.c
  pixman-utils.c
  pixman-x86.c
)
list(TRANSFORM xmin_pixman_sources PREPEND "${xmin_pixman_source_dir}/")

set(xmin_pixman_libraries Threads::Threads)
if(NOT WIN32)
  find_library(XMIN_M_LIBRARY m)
  if(XMIN_M_LIBRARY)
    list(APPEND xmin_pixman_libraries "${XMIN_M_LIBRARY}")
  endif()
endif()

xmin_add_component(pixman
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_pixman_sources}
  PRIVATE_DEFINITIONS HAVE_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${xmin_pixman_source_dir}"
    "${PROJECT_BINARY_DIR}/generated/pixman"
  PUBLIC_INCLUDE_DIRS
    "${xmin_pixman_source_dir}"
    "${PROJECT_BINARY_DIR}/generated/pixman"
  PUBLIC_LIBRARIES ${xmin_pixman_libraries}
)

xmin_add_component(xserver_headers
  UPSTREAM
  PUBLIC_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/Xext"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/Xi"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/composite"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/damageext"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/dbe"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/fb"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/glx"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/miext/damage"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/miext/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/mi"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/os"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/present"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/randr"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/render"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/xfixes"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorg-server/xkb"
  PUBLIC_LIBRARIES Xmin::xorgproto Xmin::xkbfile_headers Xmin::pixman
)
xmin_add_component(xtrans
  UPSTREAM
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/xtrans/include"
  PUBLIC_LIBRARIES Xmin::xorgproto
)
xmin_add_component(xau
  UPSTREAM
  TYPE STATIC
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuDispose.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuRead.c"
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/libXau/include"
  PUBLIC_LIBRARIES Xmin::xorgproto
)

# A private, static XCB subset for Xmin's own automation client.  Connection
# setup is intentionally supplied by project code, so generic TCP/display
# parsing and libXau lookup do not enter this component.
set(xmin_libxcb_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libxcb")
set(xmin_libxcb_sources
  src/xcb_conn.c
  src/xcb_out.c
  src/xcb_in.c
  src/xcb_ext.c
  src/xcb_xid.c
  src/xcb_list.c
  src/xproto.c
  src/bigreq.c
  src/xc_misc.c
  src/render.c
  src/shape.c
  src/xfixes.c
  src/damage.c
  src/composite.c
  src/xtest.c
)
list(TRANSFORM xmin_libxcb_sources PREPEND "${xmin_libxcb_source_dir}/")
xmin_add_component(xcb
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_libxcb_sources}
  PRIVATE_DEFINITIONS HAVE_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${xmin_libxcb_source_dir}/include/xcb"
    "${xmin_libxcb_source_dir}/src"
    "${PROJECT_BINARY_DIR}/generated/libxcb"
  PUBLIC_INCLUDE_DIRS "${xmin_libxcb_source_dir}/include"
  PUBLIC_LIBRARIES Threads::Threads
)

set(xmin_xfont2_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libXfont2")
set(xmin_xfont2_sources
  src/stubs/atom.c
  src/stubs/libxfontstubs.c
  src/util/fontaccel.c
  src/util/fontnames.c
  src/util/fontutil.c
  src/util/fontxlfd.c
  src/util/format.c
  src/util/patcache.c
  src/util/private.c
  src/util/utilbitmap.c
  src/fontfile/bitsource.c
  src/fontfile/bufio.c
  src/fontfile/defaults.c
  src/fontfile/fontdir.c
  src/fontfile/fontfile.c
  src/fontfile/fontscale.c
  src/fontfile/renderers.c
  src/bitmap/bitmap.c
  src/bitmap/pcfread.c
  src/builtins/dir.c
  src/builtins/fpe.c
  src/builtins/render.c
)
list(TRANSFORM xmin_xfont2_sources PREPEND "${xmin_xfont2_source_dir}/")
list(APPEND xmin_xfont2_sources
  "${PROJECT_SOURCE_DIR}/data/fonts/xmin_builtin_fonts.c"
  "${PROJECT_SOURCE_DIR}/src/fonts/xmin_builtin_file.c"
  "${PROJECT_SOURCE_DIR}/src/fonts/xmin_xfont_registry.c"
)

xmin_add_component(xfont2
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xfont2_sources}
  PRIVATE_DEFINITIONS HAVE_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${PROJECT_BINARY_DIR}/generated/xfont2"
    "${xmin_xfont2_source_dir}"
    "${xmin_xfont2_source_dir}/src/builtins"
  PUBLIC_INCLUDE_DIRS "${xmin_xfont2_source_dir}/include"
  PUBLIC_LIBRARIES Xmin::xorgproto ${XMIN_M_LIBRARY}
)

if(XMIN_BUILD_GLX)
  include("${CMAKE_CURRENT_SOURCE_DIR}/osmesa/osmesa-sources.cmake")
  list(TRANSFORM xmin_osmesa_source_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/"
  )

  xmin_add_component(osmesa
    UPSTREAM
    TYPE STATIC
    SOURCES ${xmin_osmesa_source_files}
    COMPILE_FEATURES c_std_11 cxx_std_17
    PRIVATE_DEFINITIONS
      LIBRARYBUILD
      GL_DLL_EXPORTS
      PTHREADS
    PRIVATE_INCLUDE_DIRS
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/main"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/glapi"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/shader"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/drivers/common"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/tnl"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast_setup"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/shader/grammar"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/shader/slang"
      "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/fxaa"
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/include"
    PUBLIC_LIBRARIES Threads::Threads ${XMIN_M_LIBRARY}
  )
  target_compile_definitions(xmin_osmesa PUBLIC USE_MGL_NAMESPACE)
  # OSMesa's public headers intentionally preserve historical OpenGL
  # prototypes. Treat them as dependency headers in project-owned users too.
  target_include_directories(xmin_osmesa SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/include"
  )
  if(CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
    target_compile_definitions(xmin_osmesa PRIVATE UTIL_ARCH_LITTLE_ENDIAN=1)
  else()
    target_compile_definitions(xmin_osmesa PRIVATE UTIL_ARCH_BIG_ENDIAN=1)
  endif()
  set_target_properties(xmin_osmesa PROPERTIES UNITY_BUILD ON)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/shader/grammar/grammar_mesa.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/drivers/osmesa/osmesa.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/math/m_translate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/math/m_xform.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast/s_triangle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast/s_points.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast/s_lines.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast/s_aaline.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast/s_aatriangle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/swrast_setup/ss_triangle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/main/texformat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/main/vtxfmt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/main/dispatch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/glapi/glapi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/vbo/vbo_exec_api.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/osmesa/src/vbo/vbo_save_api.c"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
  )
endif()
