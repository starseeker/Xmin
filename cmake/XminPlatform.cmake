include_guard(GLOBAL)

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)

# Resolve the optional host viewer before adding GLFW. GLFW's own X11 checks
# are intentionally strict and fatal, which is appropriate for an explicitly
# requested viewer but made the default build fail on headless and partially
# provisioned hosts. Keep the complete dependency policy here so AUTO can
# omit only the viewer without weakening GLFW or adding host-X dependencies to
# the core product graph.
function(xmin_configure_viewer)
  if(XMIN_BUILD_VIEWER_REQUEST STREQUAL "OFF")
    set(XMIN_BUILD_VIEWER OFF PARENT_SCOPE)
    return()
  endif()
  if(XMIN_BUILD_VIEWER_REQUEST STREQUAL "AUTO" AND
      (NOT UNIX OR APPLE))
    set(XMIN_BUILD_VIEWER OFF PARENT_SCOPE)
    return()
  endif()

  set(xmin_viewer_missing)
  find_package(OpenGL QUIET)
  if(NOT TARGET OpenGL::GL)
    list(APPEND xmin_viewer_missing "OpenGL")
  endif()
  find_package(Threads QUIET)
  if(NOT TARGET Threads::Threads)
    list(APPEND xmin_viewer_missing "threads")
  endif()

  if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(NOT X11_FOUND)
      list(APPEND xmin_viewer_missing "X11")
    else()
      foreach(xmin_x11_requirement IN ITEMS
          X11_Xrandr_INCLUDE_PATH
          X11_Xinerama_INCLUDE_PATH
          X11_Xkb_INCLUDE_PATH
          X11_Xcursor_INCLUDE_PATH
          X11_Xi_INCLUDE_PATH
          X11_Xshape_INCLUDE_PATH)
        if(NOT ${xmin_x11_requirement})
          string(REGEX REPLACE "^X11_(.*)_INCLUDE_PATH$" "\\1 headers"
            xmin_x11_description "${xmin_x11_requirement}")
          list(APPEND xmin_viewer_missing "${xmin_x11_description}")
        endif()
      endforeach()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XMIN_VIEWER_XCB QUIET IMPORTED_TARGET xcb xcb-xtest)
    endif()
    if(NOT TARGET PkgConfig::XMIN_VIEWER_XCB)
      list(APPEND xmin_viewer_missing "xcb and xcb-xtest")
    endif()
  endif()

  if(xmin_viewer_missing)
    list(JOIN xmin_viewer_missing ", " xmin_viewer_missing_text)
    if(XMIN_BUILD_VIEWER_REQUEST STREQUAL "ON")
      message(FATAL_ERROR
        "XMIN_BUILD_VIEWER=ON, but host viewer dependencies are missing: "
        "${xmin_viewer_missing_text}")
    endif()
    message(STATUS
      "Host viewer disabled (missing: ${xmin_viewer_missing_text})")
    set(XMIN_BUILD_VIEWER OFF PARENT_SCOPE)
  else()
    set(XMIN_BUILD_VIEWER ON PARENT_SCOPE)
  endif()
endfunction()

# Probe only facilities used by Xmin itself or by its retained pixman build.
# Every result exported from this function feeds a generated configuration
# header; there are deliberately no compatibility variables for Xorg/xtrans.
function(xmin_configure_platform)
  set(CMAKE_REQUIRED_QUIET TRUE)

  check_symbol_exists(AF_UNIX
    "sys/types.h;sys/socket.h" XMIN_HAVE_UNIX_SOCKETS)
  if(NOT XMIN_HAVE_UNIX_SOCKETS)
    message(FATAL_ERROR "Xmin requires Unix-domain sockets.")
  endif()

  check_include_files(
    "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SYSV_SHM_HEADERS)
  if(XMIN_HAVE_SYSV_SHM_HEADERS)
    check_symbol_exists(shmat
      "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SHMAT)
  else()
    set(XMIN_HAVE_SHMAT FALSE)
  endif()

  set(xmin_platform_has_mitshm FALSE)
  if(XMIN_HAVE_SYSV_SHM_HEADERS AND XMIN_HAVE_SHMAT)
    set(xmin_platform_has_mitshm TRUE)
  endif()
  if(XMIN_ENABLE_MITSHM STREQUAL "ON" AND NOT xmin_platform_has_mitshm)
    message(FATAL_ERROR
      "XMIN_ENABLE_MITSHM=ON, but SysV shared memory is unavailable.")
  elseif(XMIN_ENABLE_MITSHM STREQUAL "OFF")
    set(XMIN_HAVE_MITSHM FALSE)
  else()
    set(XMIN_HAVE_MITSHM ${xmin_platform_has_mitshm})
  endif()

  check_symbol_exists(getrandom "sys/random.h" XMIN_HAVE_GETRANDOM)
  check_symbol_exists(getentropy "unistd.h" XMIN_HAVE_GETENTROPY)
  check_symbol_exists(arc4random_buf "stdlib.h" XMIN_HAVE_ARC4RANDOM_BUF)
  check_symbol_exists(SCM_RIGHTS
    "sys/types.h;sys/socket.h" XMIN_HAVE_SCM_RIGHTS)

  # Configuration consumed by the retained portable pixman source set.
  check_symbol_exists(__builtin_clz "" HAVE_BUILTIN_CLZ)
  check_type_size("long" SIZEOF_LONG LANGUAGE C)
  test_big_endian(XMIN_IS_BIG_ENDIAN)
  set(WORDS_BIGENDIAN ${XMIN_IS_BIG_ENDIAN})

  foreach(variable IN ITEMS
      XMIN_HAVE_UNIX_SOCKETS
      XMIN_HAVE_MITSHM
      XMIN_HAVE_GETRANDOM
      XMIN_HAVE_GETENTROPY
      XMIN_HAVE_ARC4RANDOM_BUF
      XMIN_HAVE_SCM_RIGHTS
      XMIN_IS_BIG_ENDIAN
      HAVE_BUILTIN_CLZ
      SIZEOF_LONG
      WORDS_BIGENDIAN)
    set(${variable} "${${variable}}" PARENT_SCOPE)
  endforeach()
endfunction()
