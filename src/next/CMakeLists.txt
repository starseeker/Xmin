add_library(xmin_next_core STATIC
  atom_table.cpp
  clock.cpp
  color.cpp
  connection.cpp
  composite_protocol.cpp
  cursor_protocol.cpp
  damage_protocol.cpp
  display_socket.cpp
  font.cpp
  font_protocol.cpp
  present_protocol.cpp
  property_data.cpp
  randr_protocol.cpp
  render.cpp
  render_protocol.cpp
  region.cpp
  server.cpp
  server_state.cpp
  shared_memory.cpp
  shm_protocol.cpp
  surface.cpp
  wire.cpp
  xauthority.cpp
  xkb_protocol.cpp
  xinput_protocol.cpp
  xfixes_protocol.cpp
)
add_library(Xmin::next_core ALIAS xmin_next_core)
target_compile_features(xmin_next_core PUBLIC cxx_std_17)
target_compile_definitions(xmin_next_core PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_next_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
)
xmin_enable_warnings(xmin_next_core)
target_link_libraries(xmin_next_core PUBLIC Xmin::pixman)

add_executable(xmin_next main.cpp)
add_executable(Xmin::next_server ALIAS xmin_next)
set_target_properties(xmin_next PROPERTIES OUTPUT_NAME Xmin-next)
target_compile_features(xmin_next PRIVATE cxx_std_17)
target_compile_definitions(xmin_next PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(xmin_next PRIVATE Xmin::next_core)
xmin_enable_warnings(xmin_next)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xmin_next
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
