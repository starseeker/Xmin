add_executable(xmin_server main.c)
add_executable(Xmin::server ALIAS xmin_server)

set_target_properties(xmin_server PROPERTIES
  OUTPUT_NAME Xmin
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)

target_include_directories(xmin_server PRIVATE
  "${PROJECT_BINARY_DIR}/generated"
)

target_link_libraries(xmin_server PRIVATE
  Xmin::dix
  Xmin::os
  Xmin::mi
  Xmin::fb
  Xmin::extensions
  Xmin::vfb
  Xmin::support
  Xmin::font_data
  Xmin::xkb_data
  Xmin::xorgproto
  Xmin::pixman
  Xmin::xtrans
  Xmin::xfont2
  Threads::Threads
)

if(TARGET Xmin::glx)
  target_link_libraries(xmin_server PRIVATE Xmin::glx)
endif()

xmin_enable_warnings(xmin_server)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xmin_server
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
