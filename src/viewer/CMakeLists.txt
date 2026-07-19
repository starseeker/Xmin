find_package(OpenGL REQUIRED)

add_executable(xmin_viewer
  main.cpp
)
add_executable(Xmin::viewer ALIAS xmin_viewer)
set_target_properties(xmin_viewer PROPERTIES OUTPUT_NAME xmin-viewer)
target_compile_features(xmin_viewer PRIVATE cxx_std_17)
target_link_libraries(xmin_viewer PRIVATE glfw OpenGL::GL)

if(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(XMIN_VIEWER_XCB REQUIRED IMPORTED_TARGET xcb xcb-xtest)
  pkg_check_modules(XMIN_VIEWER_SHM QUIET IMPORTED_TARGET xcb-shm)
  pkg_check_modules(XMIN_VIEWER_DAMAGE QUIET IMPORTED_TARGET xcb-damage)
  target_sources(xmin_viewer PRIVATE viewer_xcb.cpp)
  target_link_libraries(xmin_viewer PRIVATE PkgConfig::XMIN_VIEWER_XCB)
  if(TARGET PkgConfig::XMIN_VIEWER_SHM)
    target_compile_definitions(xmin_viewer PRIVATE XMIN_VIEWER_HAVE_SHM=1)
    target_link_libraries(xmin_viewer PRIVATE PkgConfig::XMIN_VIEWER_SHM)
  else()
    target_compile_definitions(xmin_viewer PRIVATE XMIN_VIEWER_HAVE_SHM=0)
  endif()
  if(TARGET PkgConfig::XMIN_VIEWER_DAMAGE)
    target_compile_definitions(xmin_viewer PRIVATE XMIN_VIEWER_HAVE_DAMAGE=1)
    target_link_libraries(xmin_viewer PRIVATE PkgConfig::XMIN_VIEWER_DAMAGE)
  else()
    target_compile_definitions(xmin_viewer PRIVATE XMIN_VIEWER_HAVE_DAMAGE=0)
  endif()
else()
  target_sources(xmin_viewer PRIVATE viewer_stub.cpp)
endif()

xmin_enable_warnings(xmin_viewer)

if(XMIN_ENABLE_INSTALL)
  install(TARGETS xmin_viewer
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Viewer
  )
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSES/glfw/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/glfw"
    COMPONENT Viewer
  )
endif()
