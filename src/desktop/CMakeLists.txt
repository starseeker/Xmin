# Desktop application targets are intentionally separate from the X server.
# The session supervisor, window manager, and terminal are added below as
# individually installable executables.

find_package(Git REQUIRED)

set(xmin_jwm_build_dir "${CMAKE_CURRENT_BINARY_DIR}/jwm")
file(MAKE_DIRECTORY "${xmin_jwm_build_dir}")
file(COPY "${PROJECT_SOURCE_DIR}/third_party/jwm/src/"
  DESTINATION "${xmin_jwm_build_dir}/src"
  FILES_MATCHING PATTERN "*.c" PATTERN "*.h"
)
configure_file(jwm-config.h.in "${xmin_jwm_build_dir}/config.h" @ONLY)
file(RELATIVE_PATH xmin_jwm_patch_directory
  "${PROJECT_SOURCE_DIR}" "${xmin_jwm_build_dir}")
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --recount --unsafe-paths
    --whitespace=nowarn --directory=${xmin_jwm_patch_directory}/
    "${PROJECT_SOURCE_DIR}/patches/jwm/0001-direct-xft-without-pango.patch"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  RESULT_VARIABLE xmin_jwm_patch_result
  ERROR_VARIABLE xmin_jwm_patch_error
)
if(NOT xmin_jwm_patch_result EQUAL 0)
  message(FATAL_ERROR
    "Unable to apply the xmin-jwm font patch: ${xmin_jwm_patch_error}")
endif()

set(xmin_jwm_sources
  action.c background.c binding.c border.c button.c client.c clientlist.c
  clock.c color.c command.c confirm.c cursor.c debug.c default.c desktop.c
  dock.c event.c error.c font.c grab.c gradient.c group.c help.c hint.c
  icon.c image.c lex.c main.c match.c menu.c misc.c move.c outline.c pager.c
  parse.c place.c popup.c render.c resize.c root.c screen.c settings.c
  spacer.c status.c swallow.c taskbar.c timing.c tray.c traybutton.c winmenu.c
)
list(TRANSFORM xmin_jwm_sources PREPEND "${xmin_jwm_build_dir}/src/")

add_executable(xmin_jwm ${xmin_jwm_sources})
add_executable(Xmin::jwm ALIAS xmin_jwm)
set_target_properties(xmin_jwm PROPERTIES
  OUTPUT_NAME xmin-jwm
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
)
target_compile_definitions(xmin_jwm PRIVATE
  _XOPEN_SOURCE=600
  _XOPEN_SOURCE_EXTENDED=1
  __EXTENSIONS__=1
)
target_include_directories(xmin_jwm PRIVATE
  "${xmin_jwm_build_dir}"
  "${xmin_jwm_build_dir}/src"
)
target_link_libraries(xmin_jwm PRIVATE Xmin::ToolkitX11 ${XMIN_M_LIBRARY})
if(MSVC)
  target_compile_options(xmin_jwm PRIVATE /w)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(xmin_jwm PRIVATE -w)
endif()

if(UNIX AND XMIN_BUILD_BUNDLED_SHELL)
  include(ExternalProject)
  set(xmin_dash_vendor_dir "${PROJECT_SOURCE_DIR}/third_party/dash")
  if(NOT EXISTS "${xmin_dash_vendor_dir}/configure.ac" OR
      NOT EXISTS "${xmin_dash_vendor_dir}/src/mksignames.c")
    message(FATAL_ERROR
      "XMIN_BUILD_BUNDLED_SHELL requires the imported third_party/dash tree")
  endif()
  file(STRINGS "${xmin_dash_vendor_dir}/src/mksignames.c"
    xmin_dash_generator_header LIMIT_COUNT 8)
  string(JOIN "\n" xmin_dash_generator_header ${xmin_dash_generator_header})
  if(NOT xmin_dash_generator_header MATCHES
      "SPDX-License-Identifier: 0BSD")
    message(FATAL_ERROR
      "third_party/dash contains the unreviewed upstream signal generator")
  endif()

  find_program(XMIN_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
  set(xmin_dash_output "${XMIN_BUILD_BINDIR}/xmin-sh")
  set(xmin_dash_source_dir "${CMAKE_CURRENT_BINARY_DIR}/dash-source")
  set(xmin_dash_prefix "${CMAKE_CURRENT_BINARY_DIR}/dash-permissive-prefix")
  # Autotools generates configure support in its source directory. Work on a
  # private copy so a normal build never modifies the imported source tree.
  file(REMOVE_RECURSE "${xmin_dash_source_dir}" "${xmin_dash_prefix}")
  file(COPY "${xmin_dash_vendor_dir}/"
    DESTINATION "${xmin_dash_source_dir}")
  ExternalProject_Add(xmin_dash_external
    SOURCE_DIR "${xmin_dash_source_dir}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PREFIX "${xmin_dash_prefix}"
    PATCH_COMMAND <SOURCE_DIR>/autogen.sh
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
      <SOURCE_DIR>/configure --disable-lineno --without-libedit
    BUILD_COMMAND "${XMIN_MAKE_EXECUTABLE}"
    INSTALL_COMMAND
      "${CMAKE_COMMAND}" -E copy <BINARY_DIR>/src/dash "${xmin_dash_output}"
    BUILD_BYPRODUCTS "${xmin_dash_output}"
  )
endif()

if(UNIX)
  add_executable(xmin_session session.cpp)
  add_executable(Xmin::session ALIAS xmin_session)
  set_target_properties(xmin_session PROPERTIES OUTPUT_NAME xmin-session)
  target_compile_features(xmin_session PRIVATE cxx_std_17)
  target_compile_definitions(xmin_session PRIVATE
    XMIN_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
    XMIN_DESKTOP_BUILD_CONFIG="${PROJECT_SOURCE_DIR}/config/desktop/jwmrc"
    XMIN_BUNDLED_SHELL_NAME="xmin-sh"
    XMIN_RUN_BUILD_PATH="$<TARGET_FILE:xmin_run>"
    XMIN_SERVER_BUILD_PATH="$<TARGET_FILE:xmin_server>"
  )
  if(TARGET xmin_dash_external)
    add_dependencies(xmin_session xmin_dash_external)
  endif()
  xmin_enable_warnings(xmin_session)
endif()

if(UNIX)
  set(xmin_st_build_dir "${CMAKE_CURRENT_BINARY_DIR}/st")
  file(REMOVE_RECURSE "${xmin_st_build_dir}")
  file(MAKE_DIRECTORY "${xmin_st_build_dir}")
  foreach(xmin_st_file IN ITEMS st.c x.c st.h win.h arg.h)
    configure_file(
      "${PROJECT_SOURCE_DIR}/third_party/st/${xmin_st_file}"
      "${xmin_st_build_dir}/${xmin_st_file}"
      COPYONLY
    )
  endforeach()
  file(READ "${PROJECT_SOURCE_DIR}/third_party/st/config.def.h" xmin_st_config)
  string(REPLACE
    "Liberation Mono:pixelsize=12:antialias=true:autohint=true"
    "Go Mono:pixelsize=14:antialias=true"
    xmin_st_config "${xmin_st_config}"
  )
  string(REPLACE
    "char *termname = \"st-256color\";"
    "char *termname = \"xterm-256color\";"
    xmin_st_config "${xmin_st_config}"
  )
  file(WRITE "${xmin_st_build_dir}/config.h" "${xmin_st_config}")

  file(RELATIVE_PATH xmin_st_patch_directory
    "${PROJECT_SOURCE_DIR}" "${xmin_st_build_dir}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --recount --unsafe-paths
      --whitespace=nowarn --directory=${xmin_st_patch_directory}/
      "${PROJECT_SOURCE_DIR}/patches/st/0001-external-process-backend.patch"
      "${PROJECT_SOURCE_DIR}/patches/st/0002-font-size-shortcuts.patch"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE xmin_st_patch_result
    ERROR_VARIABLE xmin_st_patch_error
  )
  if(NOT xmin_st_patch_result EQUAL 0)
    message(FATAL_ERROR "Unable to apply the xmin-st backend patch: ${xmin_st_patch_error}")
  endif()

  set(xmin_st_upstream_sources
    "${xmin_st_build_dir}/st.c"
    "${xmin_st_build_dir}/x.c"
  )
  add_executable(xmin_st
    ${xmin_st_upstream_sources}
    terminal/pty_unix.c
  )
  add_executable(Xmin::st ALIAS xmin_st)
  set_target_properties(xmin_st PROPERTIES
    OUTPUT_NAME xmin-st
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
  )
  target_compile_definitions(xmin_st PRIVATE
    XMIN_EXTERNAL_PTY_BACKEND=1
    VERSION="0.9.3-xmin"
    _XOPEN_SOURCE=600
  )
  target_include_directories(xmin_st PRIVATE
    "${xmin_st_build_dir}"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminal"
  )
  target_link_libraries(xmin_st PRIVATE
    Xmin::ToolkitX11
    ${XMIN_M_LIBRARY}
    util
  )
  if(MSVC)
    set_source_files_properties(${xmin_st_upstream_sources}
      PROPERTIES COMPILE_OPTIONS /w)
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(${xmin_st_upstream_sources}
      PROPERTIES COMPILE_OPTIONS -w)
  endif()
endif()

if(APPLE)
  set(xmin_desktop_install_rpath "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set(xmin_desktop_install_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
if(DEFINED xmin_desktop_install_rpath)
  set_target_properties(xmin_jwm PROPERTIES
    INSTALL_RPATH "${xmin_desktop_install_rpath}")
  if(TARGET xmin_st)
    set_target_properties(xmin_st PROPERTIES
      INSTALL_RPATH "${xmin_desktop_install_rpath}")
  endif()
endif()

if(XMIN_ENABLE_INSTALL)
  install(TARGETS xmin_jwm
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Desktop
  )
  install(FILES "${PROJECT_SOURCE_DIR}/config/desktop/jwmrc"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Xmin/desktop"
    COMPONENT Desktop
  )
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/jwm/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/jwm"
    COMPONENT Desktop
  )
  if(TARGET xmin_st)
    install(TARGETS xmin_st
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT Desktop
    )
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/st/st.1"
      RENAME xmin-st.1
      DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
      COMPONENT Desktop
    )
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/st/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/st"
      COMPONENT Desktop
    )
  endif()
  if(TARGET xmin_session)
    install(TARGETS xmin_session
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT Desktop
    )
  endif()
  if(TARGET xmin_dash_external)
    install(PROGRAMS "${xmin_dash_output}"
      RENAME xmin-sh
      TYPE BIN
      COMPONENT Desktop
    )
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/dash/src/dash.1"
      RENAME xmin-sh.1
      DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
      COMPONENT Desktop
    )
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/dash/COPYING"
      DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/dash"
      COMPONENT Desktop
    )
  endif()
endif()
