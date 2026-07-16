# One installed DSO contains the conventional APIs qxcb consumes. Component
# object targets retain the audited upstream boundaries without advertising
# canonical libxcb/libxkbcommon SONAMEs that this experiment does not provide.
set(xmin_qt_xcb_dir "${CMAKE_CURRENT_SOURCE_DIR}/libxcb")
set(xmin_qt_xcb_sources
  src/xcb_conn.c
  src/xcb_out.c
  src/xcb_in.c
  src/xcb_ext.c
  src/xcb_xid.c
  src/xcb_list.c
  src/xcb_util.c
  src/xcb_auth.c
  src/xproto.c
  src/bigreq.c
  src/xc_misc.c
  src/render.c
  src/randr.c
  src/shape.c
  src/shm.c
  src/sync.c
  src/xfixes.c
  src/xkb.c
)
list(TRANSFORM xmin_qt_xcb_sources PREPEND "${xmin_qt_xcb_dir}/")
xmin_add_component(qt_xcb_core
  UPSTREAM
  SOURCES ${xmin_qt_xcb_sources}
  PRIVATE_DEFINITIONS HAVE_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS
    "${xmin_qt_xcb_dir}/include/xcb"
    "${xmin_qt_xcb_dir}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/libXau/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorgproto/include"
    "${PROJECT_BINARY_DIR}/generated/libxcb"
  PUBLIC_LIBRARIES Threads::Threads
)

set(xmin_qt_xau_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuDispose.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuFileName.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuGetBest.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libXau/AuRead.c"
)
xmin_add_component(qt_xau
  UPSTREAM
  SOURCES ${xmin_qt_xau_sources}
  PRIVATE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/libXau/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorgproto/include"
)

set(xmin_qt_xcb_util_dir "${CMAKE_CURRENT_SOURCE_DIR}/xcb-util")
set(xmin_qt_xcb_util_sources
  atoms.c
  event.c
  xcb_aux.c
  xcb_image.c
  xcb_keysyms.c
  renderutil_cache.c
  renderutil_util.c
  renderutil_glyph.c
  cursor_cursor.c
  cursor_shape_to_id.c
  cursor_load_cursor.c
  cursor_parse_cursor_file.c
  xcb_icccm.c
)
list(TRANSFORM xmin_qt_xcb_util_sources
  PREPEND "${xmin_qt_xcb_util_dir}/src/")
xmin_add_component(qt_xcb_util
  UPSTREAM
  SOURCES ${xmin_qt_xcb_util_sources}
  PRIVATE_DEFINITIONS XCURSORPATH=""
  PRIVATE_INCLUDE_DIRS
    "${xmin_qt_xcb_dir}/include"
    "${xmin_qt_xcb_util_dir}/include"
    "${xmin_qt_xcb_util_dir}/include/xcb"
    "${xmin_qt_xcb_util_dir}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/xorgproto/include"
)

set(xmin_qt_xkb_dir "${CMAKE_CURRENT_SOURCE_DIR}/libxkbcommon")
set(xmin_qt_xkb_sources
  src/compose/parser.c
  src/compose/paths.c
  src/compose/state.c
  src/compose/table.c
  src/xkbcomp/action.c
  src/xkbcomp/ast-build.c
  src/xkbcomp/compat.c
  src/xkbcomp/expr.c
  src/xkbcomp/include.c
  src/xkbcomp/keycodes.c
  src/xkbcomp/keymap.c
  src/xkbcomp/keymap-dump.c
  src/xkbcomp/keywords.c
  src/xkbcomp/parser.c
  src/xkbcomp/rules.c
  src/xkbcomp/scanner.c
  src/xkbcomp/symbols.c
  src/xkbcomp/types.c
  src/xkbcomp/vmod.c
  src/xkbcomp/xkbcomp.c
  src/atom.c
  src/context.c
  src/context-priv.c
  src/keysym.c
  src/keysym-case-mappings.c
  src/keysym-utf.c
  src/keymap.c
  src/keymap-compare.c
  src/keymap-priv.c
  src/rmlvo.c
  src/scanner-utils.c
  src/state.c
  src/text.c
  src/utf8.c
  src/utf8-decoding.c
  src/utils.c
  src/utils-paths.c
  src/x11/keymap.c
  src/x11/state.c
  src/x11/util.c
)
list(TRANSFORM xmin_qt_xkb_sources PREPEND "${xmin_qt_xkb_dir}/")
xmin_add_component(qt_xkbcommon
  UPSTREAM
  SOURCES ${xmin_qt_xkb_sources}
  PRIVATE_INCLUDE_DIRS
    "${xmin_qt_xkb_dir}"
    "${xmin_qt_xkb_dir}/include"
    "${xmin_qt_xkb_dir}/src"
    "${xmin_qt_xcb_dir}/include"
    "${PROJECT_BINARY_DIR}/generated/libxkbcommon"
)

foreach(target IN ITEMS
    xmin_qt_xcb_core xmin_qt_xau xmin_qt_xcb_util xmin_qt_xkbcommon)
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

add_library(xmin_qt_x11 SHARED)
add_library(Xmin::QtX11 ALIAS xmin_qt_x11)
target_sources(xmin_qt_x11 PRIVATE
  $<TARGET_OBJECTS:xmin_qt_xcb_core>
  $<TARGET_OBJECTS:xmin_qt_xau>
  $<TARGET_OBJECTS:xmin_qt_xcb_util>
  $<TARGET_OBJECTS:xmin_qt_xkbcommon>
)
target_include_directories(xmin_qt_x11
  PUBLIC
    "$<BUILD_INTERFACE:${xmin_qt_xcb_dir}/include>"
    "$<BUILD_INTERFACE:${xmin_qt_xcb_util_dir}/include>"
    "$<BUILD_INTERFACE:${xmin_qt_xkb_dir}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(xmin_qt_x11 PRIVATE Threads::Threads)
set_target_properties(xmin_qt_x11 PROPERTIES
  EXPORT_NAME QtX11
  OUTPUT_NAME XminClient
  VERSION 0.1.0
  SOVERSION 0
)

if(XMIN_ENABLE_INSTALL)
  include(CMakePackageConfigHelpers)
  write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/XminConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
  )
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/XminConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/XminConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Xmin"
  )

  install(TARGETS xmin_qt_x11
    EXPORT XminTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      COMPONENT QtClientRuntime NAMELINK_COMPONENT QtClientDevelopment
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      COMPONENT QtClientDevelopment
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT QtClientRuntime
  )
  install(FILES
      "${xmin_qt_xcb_dir}/include/xcb/xcb.h"
      "${xmin_qt_xcb_dir}/include/xcb/xcbext.h"
      "${xmin_qt_xcb_dir}/include/xcb/xproto.h"
      "${xmin_qt_xcb_dir}/include/xcb/bigreq.h"
      "${xmin_qt_xcb_dir}/include/xcb/xc_misc.h"
      "${xmin_qt_xcb_dir}/include/xcb/render.h"
      "${xmin_qt_xcb_dir}/include/xcb/randr.h"
      "${xmin_qt_xcb_dir}/include/xcb/shape.h"
      "${xmin_qt_xcb_dir}/include/xcb/shm.h"
      "${xmin_qt_xcb_dir}/include/xcb/sync.h"
      "${xmin_qt_xcb_dir}/include/xcb/xfixes.h"
      "${xmin_qt_xcb_dir}/include/xcb/xkb.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_util.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_atom.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_aux.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_event.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_image.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_pixel.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_bitops.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_keysyms.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_renderutil.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_cursor.h"
      "${xmin_qt_xcb_util_dir}/include/xcb/xcb_icccm.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xcb"
    COMPONENT QtClientDevelopment
  )
  install(FILES
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon.h"
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon-compat.h"
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon-compose.h"
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon-keysyms.h"
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon-names.h"
      "${xmin_qt_xkb_dir}/include/xkbcommon/xkbcommon-x11.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xkbcommon"
    COMPONENT QtClientDevelopment
  )
  install(EXPORT XminTargets
    FILE XminTargets.cmake
    NAMESPACE Xmin::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Xmin"
    COMPONENT QtClientDevelopment
  )
  install(FILES
      "${PROJECT_BINARY_DIR}/XminConfig.cmake"
      "${PROJECT_BINARY_DIR}/XminConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Xmin"
    COMPONENT QtClientDevelopment
  )
endif()
