# Preserve the broad upstream X server boundaries. These subdirectories expose
# stable Xmin:: component targets linked into the server executable.
add_subdirectory(dix)
add_subdirectory(os)
add_subdirectory(mi)
add_subdirectory(fb)
add_subdirectory(xkb)
add_subdirectory(extensions)
add_subdirectory(hw/vfb)
add_subdirectory(support)
add_subdirectory(control)

if(XMIN_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

if(XMIN_BUILD_GLX)
  add_subdirectory(glx)
  add_subdirectory(client/glx)
endif()

# The server is last so every selected component target already exists.
add_subdirectory(server)
