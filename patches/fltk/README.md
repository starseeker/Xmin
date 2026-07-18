# FLTK client patch

`fltk-1.4.5-xmin.patch` is based on the upstream FLTK 1.4.5 release:

- commit: `a9b1113516ffd15fc7602a6d425a317df30f4720`
- tree: `9df53c591a408c38ca6bb3ed1a338cfaae5bfe9c`

Apply it at the root of an FLTK checkout with `patch -p1`, then configure
with `FLTK_USE_XMIN=ON`, `FLTK_USE_XFT=ON`, and `Xmin_DIR` pointing to an
installed Xmin CMake package. The opt-in replaces X11, Xft, Fontconfig, and
FreeType linkage with `Xmin::ToolkitX11`; normal FLTK builds are unchanged.

The acceptance build used these additional settings to isolate the core
X11 backend: `FLTK_BACKEND_WAYLAND=OFF`, `FLTK_BUILD_GL=OFF`, and
`FLTK_OPTION_PRINT_SUPPORT=OFF`.
