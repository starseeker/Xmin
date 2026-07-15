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

xmin_configure_platform()

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/xmin_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/xmin/config.h"
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
