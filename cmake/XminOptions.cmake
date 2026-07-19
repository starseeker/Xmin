include_guard(GLOBAL)

function(xmin_normalize_tristate variable)
  string(TOUPPER "${${variable}}" value)
  if(NOT value MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR
      "${variable} must be AUTO, ON, or OFF (received '${${variable}}')."
    )
  endif()
  set(${variable} "${value}" CACHE STRING "" FORCE)
  set_property(CACHE ${variable} PROPERTY STRINGS AUTO ON OFF)
endfunction()

option(XMIN_BUILD_CLIENT_GL
  "Build the bundled software-direct client libGL" ON)
option(XMIN_BUILD_QT_CLIENT
  "Build the Xmin-native XCB/xkbcommon compatibility SDK for Qt" OFF)
option(XMIN_BUILD_TOOLKIT_CLIENT
  "Build the embedded-font X11 client support for FLTK and Tk" OFF)
option(XMIN_BUILD_LAUNCHER
  "Build the authenticated xmin-run child launcher" ON)
set(xmin_interactive_default OFF)
if(UNIX AND NOT APPLE)
  set(xmin_interactive_default ON)
endif()
option(XMIN_BUILD_VIEWER
  "Build the host GLFW viewer and input bridge" ${xmin_interactive_default})
option(XMIN_BUILD_DESKTOP
  "Build the minimalist Unix JWM/st desktop session" ${xmin_interactive_default})
set(xmin_bundled_shell_default OFF)
if(UNIX)
  set(xmin_bundled_shell_default ON)
endif()
option(XMIN_BUILD_BUNDLED_SHELL
  "Build the imported dash shell for Unix desktop sessions"
  ${xmin_bundled_shell_default})
unset(xmin_interactive_default)
unset(xmin_bundled_shell_default)
option(XMIN_BUILD_TESTS "Build Xmin's self-tests" ${PROJECT_IS_TOP_LEVEL})
option(XMIN_REQUIRE_TOOLKIT_TESTS
  "Fail configuration unless Qt 5/6 and GTK 3 acceptance tests are available" OFF)
option(XMIN_ENABLE_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(XMIN_WARNINGS_AS_ERRORS
  "Treat warnings in project-owned code as errors" OFF)
option(XMIN_ENABLE_SANITIZERS
  "Instrument C and C++ code with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(XMIN_BUILD_DESKTOP AND NOT UNIX)
  message(FATAL_ERROR
    "XMIN_BUILD_DESKTOP currently requires a Unix PTY/process backend.")
endif()

if(XMIN_BUILD_DESKTOP)
  set(XMIN_BUILD_LAUNCHER ON CACHE BOOL
    "Build the authenticated launcher required by the desktop session"
    FORCE)
  set(XMIN_BUILD_TOOLKIT_CLIENT ON CACHE BOOL
    "Build the embedded-font X11 client support for FLTK, Tk, and the desktop"
    FORCE)
endif()

if(XMIN_REQUIRE_TOOLKIT_TESTS AND NOT XMIN_BUILD_TESTS)
  message(FATAL_ERROR
    "XMIN_REQUIRE_TOOLKIT_TESTS requires XMIN_BUILD_TESTS=ON."
  )
endif()

set(XMIN_ENABLE_MITSHM "AUTO" CACHE STRING
  "Build MIT-SHM support when the platform provides SysV shared memory")
xmin_normalize_tristate(XMIN_ENABLE_MITSHM)

set(XMIN_DEFAULT_WIDTH "1280" CACHE STRING "Default screen width in pixels")
set(XMIN_DEFAULT_HEIGHT "1024" CACHE STRING "Default screen height in pixels")

foreach(variable IN ITEMS
    XMIN_DEFAULT_WIDTH XMIN_DEFAULT_HEIGHT)
  if(NOT "${${variable}}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${variable} must be a positive integer.")
  endif()
endforeach()
