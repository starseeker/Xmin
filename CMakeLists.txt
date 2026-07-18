cmake_minimum_required(VERSION 3.21...4.0)

project(
  Xmin
  VERSION 0.1.0
  DESCRIPTION "A minimal, self-contained, software-only X11 server"
  HOMEPAGE_URL "https://github.com/starseeker/xvfb"
  LANGUAGES C CXX
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
include(XminProtocolProfile)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(XMIN_ENABLE_SANITIZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
     NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR
      "XMIN_ENABLE_SANITIZERS requires Clang or GCC for both C and C++"
    )
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Keep platform probes and the vendored C dependencies on a consistent POSIX
# feature surface.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
  list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
endif()

xmin_configure_platform()

xmin_verify_protocol_profile(
  "${PROJECT_SOURCE_DIR}/profiles/protocol.json"
  "${PROJECT_SOURCE_DIR}/profiles/protocol-coverage.json"
  "${PROJECT_SOURCE_DIR}/protocol/xcb"
  "${PROJECT_SOURCE_DIR}/src/server/include/xmin/server/generated/core_protocol.hpp"
)

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/xmin_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/xmin/config.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/pixman-config.h.in"
  "${PROJECT_BINARY_DIR}/generated/pixman/pixman-config.h"
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
add_subdirectory(src)

add_custom_target(xmin-modernization-metrics
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DBINARY_DIR=${PROJECT_BINARY_DIR}"
    "-DSERVER_BINARY=$<TARGET_FILE:xmin_server>"
    "-DOUTPUT=${PROJECT_BINARY_DIR}/modernization-metrics.txt"
    -P "${PROJECT_SOURCE_DIR}/cmake/WriteModernizationMetrics.cmake"
  DEPENDS xmin_server
  BYPRODUCTS "${PROJECT_BINARY_DIR}/modernization-metrics.txt"
  VERBATIM
)

if(XMIN_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

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
  install(EXPORT XminTargets
    FILE XminTargets.cmake
    NAMESPACE Xmin::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Xmin"
    COMPONENT Development
  )
  install(FILES
    "${PROJECT_BINARY_DIR}/XminConfig.cmake"
    "${PROJECT_BINARY_DIR}/XminConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Xmin"
    COMPONENT Development
  )
  install(
    FILES README.txt DESIGN.md BUILDING.md
    TYPE DOC
    COMPONENT Documentation
  )
endif()

message(STATUS "Xmin ${PROJECT_VERSION} configuration")
message(STATUS "  Direct client OpenGL:  ${XMIN_BUILD_CLIENT_GL}")
message(STATUS "  Qt X11 client SDK:     ${XMIN_BUILD_QT_CLIENT}")
message(STATUS "  FLTK/Tk client SDK:    ${XMIN_BUILD_TOOLKIT_CLIENT}")
message(STATUS "  MIT-SHM support:       ${XMIN_HAVE_MITSHM}")
message(STATUS "  Integrated launcher:   ${XMIN_BUILD_LAUNCHER}")
message(STATUS "  Tests:                 ${XMIN_BUILD_TESTS}")
message(STATUS "  ASan/UBSan:            ${XMIN_ENABLE_SANITIZERS}")
if(XMIN_BUILD_TESTS)
  message(STATUS "  Qt 5 acceptance:       ${XMIN_HAVE_QT5_TESTS}")
  message(STATUS "  Qt 6 acceptance:       ${XMIN_HAVE_QT6_TESTS}")
  message(STATUS "  GTK 3 acceptance:      ${XMIN_HAVE_GTK3_TESTS}")
  message(STATUS "  Require toolkits:      ${XMIN_REQUIRE_TOOLKIT_TESTS}")
endif()
