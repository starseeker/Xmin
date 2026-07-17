# Third-party code

The server has one bundled dependency: pixman, used behind Xmin's Surface,
Region, and Renderer types. Optional client-side OpenGL adds one independent
OSMesa copy to `libGL`; OSMesa is never linked into the X server.

Pixman is compiled as a generic, single-threaded C raster kernel. Xmin owns
all access on its server thread, so pixman's process-local fast-path cache
needs no pthread or Win32 TLS backend. The optional GL bridge is C++17 and
uses standard synchronization; OSMesa retains its own platform abstraction
inside the pinned C dependency.

Both source sets are pinned in `UPSTREAM.toml`. `tools/import-pixman.sh` uses
an exact file allowlist. `tools/import-osmesa.sh` records its source manifest
and applies the single draw/read-surface patch. Xmin's OSMesa adapter keeps all
Mesa-private types inside one dependency-side translation unit.

The normal configure and build are offline and never substitute host graphics
libraries for these sources.
