# Third-party source imports

The first import contains the selected xorgproto 2024.1 headers, generic-C pixman
0.46.2, and a small compiling tranche of Xorg Server 21.1.23. Keep imports in
separate subdirectories using their upstream layout, and record exact revisions,
archive checksums, licenses, retained paths, and local patches in `UPSTREAM.toml`.

The initial candidates and retained responsibilities are listed in `DESIGN.md`.
Imports must build only through Xmin's CMake targets; do not make the normal build
discover equivalent libraries from the host.

`tools/import-xorg.sh` reproduces the retained file set from verified, extracted
upstream trees. It intentionally imports no DRI implementation. `dri3proto.h` is
present only because the current Present protocol header uses its sync-object wire
type.
