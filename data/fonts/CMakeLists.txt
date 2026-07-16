# The generated resource is compiled into Xmin::xfont2 so the built-in FPE and
# its data cannot drift apart. This interface target preserves the data-layer
# boundary for future generated resources.
xmin_add_component(font_data
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
)
