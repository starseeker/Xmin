# pixman is the sole third-party dependency of the server. Optional
# client-side OpenGL is assembled independently in src/client/glx.
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
