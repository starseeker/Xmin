add_library(xmin_platform STATIC
  clock.cpp
  display_socket.cpp
  shared_memory.cpp
  xauthority.cpp
)
add_library(Xmin::platform ALIAS xmin_platform)
set_target_properties(xmin_platform PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_compile_features(xmin_platform PUBLIC cxx_std_17)
target_compile_definitions(xmin_platform PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_platform
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
)
xmin_enable_warnings(xmin_platform)

add_library(xmin_server_core STATIC
  atom_table.cpp
  color.cpp
  connection.cpp
  core_protocol.cpp
  core_raster.cpp
  composite_protocol.cpp
  compatibility_protocol.cpp
  cursor_protocol.cpp
  damage_protocol.cpp
  font.cpp
  font_protocol.cpp
  present_protocol.cpp
  glx_protocol.cpp
  property_data.cpp
  randr_protocol.cpp
  raster_protocol.cpp
  render.cpp
  render_protocol.cpp
  region.cpp
  server.cpp
  server_state.cpp
  shm_protocol.cpp
  surface.cpp
  wire.cpp
  xkb_protocol.cpp
  xinput_protocol.cpp
  xinput_legacy_protocol.cpp
  xfixes_protocol.cpp
)
add_library(Xmin::server_core ALIAS xmin_server_core)
target_compile_features(xmin_server_core PUBLIC cxx_std_17)
target_compile_definitions(xmin_server_core PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_server_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
)
xmin_enable_warnings(xmin_server_core)
target_link_libraries(xmin_server_core PUBLIC Xmin::platform Xmin::pixman)

add_executable(xmin_server main.cpp)
add_executable(Xmin::server ALIAS xmin_server)
set_target_properties(xmin_server PROPERTIES
  EXPORT_NAME server
  OUTPUT_NAME Xmin
)
target_compile_features(xmin_server PRIVATE cxx_std_17)
target_compile_definitions(xmin_server PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(xmin_server PRIVATE Xmin::server_core)
xmin_enable_warnings(xmin_server)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xmin_server
    EXPORT XminTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
