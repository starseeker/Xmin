# Stable dependency targets. Each becomes an audited vendored target when its
# source is imported; none may silently resolve to a host graphics package.
xmin_add_component(xorgproto UPSTREAM)
xmin_add_component(pixman UPSTREAM)
xmin_add_component(xtrans UPSTREAM)
xmin_add_component(xfont2 UPSTREAM)

if(XMIN_BUILD_GLX)
  xmin_add_component(osmesa
    UPSTREAM
    COMPILE_FEATURES c_std_11 cxx_std_17
  )
endif()

