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

option(XMIN_BUILD_GLX
  "Build embedded OSMesa, indirect server GLX, and bundled client libGL" ON)
option(XMIN_ENABLE_TCP
  "Enable the TCP transport (the normal build is local-socket only)" OFF)
option(XMIN_BUILD_LAUNCHER
  "Build the authenticated xmin-run child launcher" ON)
option(XMIN_BUILD_TESTS "Build Xmin's self-tests" ${PROJECT_IS_TOP_LEVEL})
option(XMIN_REQUIRE_TOOLKIT_TESTS
  "Fail configuration unless Qt 5/6 and GTK 3 acceptance tests are available" OFF)
option(XMIN_ENABLE_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(XMIN_WARNINGS_AS_ERRORS
  "Treat warnings in project-owned code as errors" OFF)

if(XMIN_REQUIRE_TOOLKIT_TESTS AND NOT XMIN_BUILD_TESTS)
  message(FATAL_ERROR
    "XMIN_REQUIRE_TOOLKIT_TESTS requires XMIN_BUILD_TESTS=ON."
  )
endif()

set(XMIN_ENABLE_MITSHM "AUTO" CACHE STRING
  "Build MIT-SHM support when the platform provides SysV shared memory")
set(XMIN_PIXMAN_SIMD "AUTO" CACHE STRING
  "Enable supported pixman SIMD implementations")
xmin_normalize_tristate(XMIN_ENABLE_MITSHM)
xmin_normalize_tristate(XMIN_PIXMAN_SIMD)

set(XMIN_DEFAULT_WIDTH "1280" CACHE STRING "Default screen width in pixels")
set(XMIN_DEFAULT_HEIGHT "1024" CACHE STRING "Default screen height in pixels")
set(XMIN_DEFAULT_DEPTH "24" CACHE STRING "Default screen depth")
set(XMIN_DEFAULT_DPI "96" CACHE STRING "Default screen DPI")

foreach(variable IN ITEMS
    XMIN_DEFAULT_WIDTH XMIN_DEFAULT_HEIGHT XMIN_DEFAULT_DPI)
  if(NOT "${${variable}}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${variable} must be a positive integer.")
  endif()
endforeach()

if(NOT XMIN_DEFAULT_DEPTH MATCHES "^(1|8|15|16|24|30)$")
  message(FATAL_ERROR
    "XMIN_DEFAULT_DEPTH must be one of 1, 8, 15, 16, 24, or 30."
  )
endif()
