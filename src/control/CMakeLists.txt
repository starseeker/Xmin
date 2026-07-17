add_executable(xminctl
  xminctl.cpp
  xmin_client.cpp
)
add_executable(Xmin::ctl ALIAS xminctl)
set_target_properties(xminctl PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
)
target_include_directories(xminctl PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_BINARY_DIR}/generated"
)
target_compile_definitions(xminctl PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(xminctl PRIVATE
  Xmin::platform
)
xmin_enable_warnings(xminctl)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xminctl
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
