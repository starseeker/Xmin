# FLTK client patch

`fltk-1.4.5-xmin.patch` is based on the upstream FLTK 1.4.5 release:

- commit: `a9b1113516ffd15fc7602a6d425a317df30f4720`
- tree: `9df53c591a408c38ca6bb3ed1a338cfaae5bfe9c`

Apply it at the root of an FLTK checkout with `patch -p1`, then configure
with `FLTK_USE_XMIN=ON`, `FLTK_USE_XFT=ON`, and `Xmin_DIR` pointing to an
installed Xmin CMake package. The opt-in replaces X11, Xft, Fontconfig, and
FreeType linkage with `Xmin::ToolkitX11`; normal FLTK builds are unchanged.
The generated `fltk-config` also emits the Xmin library directory and
`-lXminClient`, so non-CMake FLTK consumers remain usable. The exported FLTK
CMake package resolves Xmin automatically for downstream CMake consumers.
The opt-in also disables FLTK's runtime `libXrandr` loading: a host Xrandr
library is coupled to the host libX11 `Display` ABI and cannot safely consume
Xmin's self-contained display object.

When `Xmin::GL` is present, `FLTK_BUILD_GL=ON` now resolves FLTK's ordinary
X11/GLX backend to Xmin's OSMesa-backed implementation; no FLTK GL source
changes are required. The portable raster-only profile remains available with
`FLTK_BUILD_GL=OFF`. Acceptance also disables Wayland and print support to
isolate the X11 backend.

This OpenGL profile was configured and build-tested on 2026-07-18 through the
ordinary `fltk_gl` target, including FLTK's X11 GL window driver.
