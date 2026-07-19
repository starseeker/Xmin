# Third-party code

The server has one bundled dependency: pixman, used behind Xmin's Surface,
Region, and Renderer types. Optional client-side OpenGL adds one independent
OSMesa copy to `libGL`; OSMesa is never linked into the X server. Optional
minimalist toolkit clients use the header-only struetype rasterizer and eight
embedded Go font faces; they do not search for or load host fonts.

The optional Unix desktop builds the imported dash 0.5.13.4 sources as
`xmin-sh`. Its exact import is recorded in `tools/dash-files.txt`. The one
upstream GPL helper is not retained: `tools/import-dash.sh` substitutes Xmin's
independently written 0BSD signal-name generator before the source enters this
directory.

Pixman is compiled as a generic, single-threaded C raster kernel. Xmin owns
all access on its server thread, so pixman's process-local fast-path cache
needs no pthread or Win32 TLS backend. The optional GL bridge is C++17 and
uses standard synchronization; OSMesa retains its own platform abstraction
inside the pinned C dependency.

Both source sets are pinned in `UPSTREAM.toml`. `tools/import-pixman.sh` uses
an exact file allowlist. `tools/import-osmesa.sh` records its source manifest;
the pinned upstream includes the separate draw/read color-surface API used by
the GLX bridge. Xmin's OSMesa adapter keeps all Mesa-private types inside one
dependency-side translation unit.

The optional Qt client is different: its XCB, xcb-util, and xkbcommon
behavior is implemented in project-owned C++17 under `src/client/x11`.
Standard public headers are retained there only to expose the ABI expected by
qxcb; no upstream XCB, xcb-util, xkbcommon, Xau, or Xlib implementation source
is compiled. Header versions, checksums, licenses, and the two C++ keyword
renames in the generated XKB header are recorded in `UPSTREAM.toml`.

The normal configure and build are offline and never substitute host graphics
libraries for these sources.
