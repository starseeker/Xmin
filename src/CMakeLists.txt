# The C++17 server is the product. Its source tree is independent of Xorg.
add_subdirectory(server)
add_subdirectory(control)

if(XMIN_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

if(XMIN_BUILD_CLIENT_GL)
  add_subdirectory(client/glx)
endif()
