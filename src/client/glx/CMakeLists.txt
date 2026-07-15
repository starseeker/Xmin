# Companion client-side GLX/OSMesa bridge. This is not linked into the Xmin server.
xmin_add_component(glx_client
  COMPILE_FEATURES c_std_11 cxx_std_17
  PUBLIC_LIBRARIES Xmin::osmesa
)

