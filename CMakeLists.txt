cmake_minimum_required(VERSION 3.21...4.0)

project(
  Xmin
  VERSION 0.1.0
  DESCRIPTION "A minimal, self-contained, software-only X11 server"
  HOMEPAGE_URL "https://github.com/starseeker/xvfb"
  LANGUAGES C
)

if(PROJECT_IS_TOP_LEVEL AND PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported. Configure into a separate build directory."
  )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(XminOptions)
include(XminWarnings)
include(XminComponent)
include(XminPlatform)

if(XMIN_BUILD_GLX)
  enable_language(CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# glibc and musl hide a number of APIs used by the retained Xorg code when C11
# is requested without extensions.  Define the Linux feature-test macro on the
# compiler command line so it is active before every system header, including
# in CMake's platform probes.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
  list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
endif()

xmin_configure_platform()

# Xorg's retained static extension registry uses this traditional build
# define. Keep it tied to the single public Xmin option.
set(GLXEXT ${XMIN_BUILD_GLX})

if(CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
  set(XMIN_X_BYTE_ORDER X_LITTLE_ENDIAN)
elseif(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  set(XMIN_X_BYTE_ORDER X_BIG_ENDIAN)
else()
  message(FATAL_ERROR "Xmin requires a compiler with a detectable byte order")
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/xmin_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/xmin/config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/dix-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/dix-config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/xkb-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/xkb-config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/xfont-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/xfont2/config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/version-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/version-config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/third_party/xorgproto/include/X11/Xpoll.h.in"
  "${PROJECT_BINARY_DIR}/generated/X11/Xpoll.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/pixman-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/pixman/pixman-config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/libxcb-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/libxcb/config.h"
  @ONLY
)
set(PIXMAN_VERSION_MAJOR 0)
set(PIXMAN_VERSION_MINOR 46)
set(PIXMAN_VERSION_MICRO 2)
configure_file(
  "${PROJECT_SOURCE_DIR}/third_party/pixman/pixman/pixman-version.h.in"
  "${PROJECT_BINARY_DIR}/generated/pixman/pixman-version.h"
  @ONLY
)

add_subdirectory(third_party)
add_subdirectory(data)
add_subdirectory(src)

if(XMIN_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(XMIN_ENABLE_INSTALL)
  install(
    PROGRAMS xmincapture.sh
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    RENAME xmincapture
    COMPONENT Runtime
  )
  install(
    FILES README.txt DESIGN.md BUILDING.md
    TYPE DOC
    COMPONENT Documentation
  )
endif()

message(STATUS "Xmin ${PROJECT_VERSION} configuration")
message(STATUS "  OpenGL/GLX components: ${XMIN_BUILD_GLX}")
message(STATUS "  MIT-SHM support:       ${XMIN_HAVE_MITSHM}")
message(STATUS "  TCP transport:         ${XMIN_ENABLE_TCP}")
message(STATUS "  Integrated launcher:   ${XMIN_BUILD_LAUNCHER}")
message(STATUS "  Tests:                 ${XMIN_BUILD_TESTS}")
if(XMIN_BUILD_TESTS)
  message(STATUS "  Qt 5 acceptance:       ${XMIN_HAVE_QT5_TESTS}")
  message(STATUS "  Qt 6 acceptance:       ${XMIN_HAVE_QT6_TESTS}")
  message(STATUS "  GTK 3 acceptance:      ${XMIN_HAVE_GTK3_TESTS}")
  message(STATUS "  Require toolkits:      ${XMIN_REQUIRE_TOOLKIT_TESTS}")
endif()
