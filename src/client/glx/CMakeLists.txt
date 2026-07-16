set(xmin_client_gl_include "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Keep the project-owned ABI adapter under the normal warning policy. The
# renderer sources themselves are an upstream target below.
add_library(xmin_glx_client_api OBJECT xmin_glx_client.c)
set_target_properties(xmin_glx_client_api PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_compile_features(xmin_glx_client_api PRIVATE c_std_11)
target_compile_definitions(xmin_glx_client_api PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_glx_client_api PRIVATE
  "${xmin_client_gl_include}"
  "${PROJECT_BINARY_DIR}/generated"
)
target_include_directories(xmin_glx_client_api SYSTEM PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/include"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/glapi"
)
target_link_libraries(xmin_glx_client_api PRIVATE Threads::Threads)
xmin_enable_warnings(xmin_glx_client_api)

# A complete libGL ABI must export ordinary gl* symbols. The server's OSMesa
# archive is deliberately namespaced mgl*, so compile the same pinned source
# set once more, without USE_MGL_NAMESPACE, into this separately shipped DSO.
include("${PROJECT_SOURCE_DIR}/third_party/osmesa/osmesa-sources.cmake")
list(TRANSFORM xmin_osmesa_source_files
  PREPEND "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/"
)

add_library(xmin_gl_client SHARED
  $<TARGET_OBJECTS:xmin_glx_client_api>
  ${xmin_osmesa_source_files}
)
add_library(Xmin::gl_client ALIAS xmin_gl_client)
set_target_properties(xmin_gl_client PROPERTIES
  OUTPUT_NAME GL
  VERSION 1.0.0
  SOVERSION 1
  UNITY_BUILD ON
)
target_compile_features(xmin_gl_client PRIVATE c_std_11 cxx_std_17)
target_compile_definitions(xmin_gl_client PRIVATE
  LIBRARYBUILD
  GL_DLL_EXPORTS
  PTHREADS
)
target_include_directories(xmin_gl_client
  PUBLIC
    "$<BUILD_INTERFACE:${xmin_client_gl_include}>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/osmesa/include>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/glapi"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/shader"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/drivers/common"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/tnl"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast_setup"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/shader/grammar"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/shader/slang"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/fxaa"
)
target_link_libraries(xmin_gl_client PRIVATE Threads::Threads ${XMIN_M_LIBRARY})
if(CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
  target_compile_definitions(xmin_gl_client PRIVATE UTIL_ARCH_LITTLE_ENDIAN=1)
else()
  target_compile_definitions(xmin_gl_client PRIVATE UTIL_ARCH_BIG_ENDIAN=1)
endif()

# These old Mesa translation units collide when combined in a unity source.
set_source_files_properties(
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/shader/grammar/grammar_mesa.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/drivers/osmesa/osmesa.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/math/m_translate.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/math/m_xform.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast/s_triangle.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast/s_points.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast/s_lines.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast/s_aaline.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast/s_aatriangle.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/swrast_setup/ss_triangle.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main/texformat.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main/vtxfmt.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/main/dispatch.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/glapi/glapi.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/vbo/vbo_exec_api.c"
  "${PROJECT_SOURCE_DIR}/third_party/osmesa/src/vbo/vbo_save_api.c"
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)

if(APPLE)
  # Xlib is supplied by the application using the GLX ABI. Keep it out of
  # Xmin's build and installed dependency graph.
  target_link_options(xmin_gl_client PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()

if(XMIN_ENABLE_INSTALL)
  install(TARGETS xmin_gl_client
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/xmin"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
  install(FILES
    "${xmin_client_gl_include}/GL/gl.h"
    "${xmin_client_gl_include}/GL/glext.h"
    "${xmin_client_gl_include}/GL/glx.h"
    "${xmin_client_gl_include}/GL/glxext.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/GL"
    COMPONENT Development
  )
  install(FILES
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/include/OSMesa/gl.h"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/include/OSMesa/glext.h"
    "${PROJECT_SOURCE_DIR}/third_party/osmesa/include/OSMesa/osmesa.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/OSMesa"
    COMPONENT Development
  )
endif()
