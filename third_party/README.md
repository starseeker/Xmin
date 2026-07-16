# Third-party source imports

The retained import contains selected xorgproto 2024.1 headers, generic-C pixman
0.46.2, a built-in-only libXfont2 2.0.8 subset, local-only xtrans 1.6.0, the libXau 1.0.12
authority reader, and the selected DIX, MI, fb, OS, vfb DDX, RENDER, RANDR, Present,
COMPOSITE, DAMAGE, XFIXES, DOUBLE-BUFFER, XInput/XI, core Xext, software SYNC, and
static extension-registry source lists from Xorg Server 21.1.23. It also contains the
pinned starseeker/osmesa OpenGL 2.0 software renderer and Xorg's non-DRI indirect GLX
dispatcher and in-server vendor-routing sources. The retained XKB
source set uses only libxkbfile's
two XKM format headers; its runtime loader is replaced with Xmin's embedded-map
adapter. Keep imports in separate subdirectories using their
upstream layout, and record exact revisions,
archive checksums, licenses, retained paths, and local patches in `UPSTREAM.toml`.

The initial candidates and retained responsibilities are listed in `DESIGN.md`.
Imports must build only through Xmin's CMake targets; do not make the normal build
discover equivalent libraries from the host.

`tools/import-xorg.sh` reproduces the retained file set from verified, extracted
upstream trees. It intentionally imports no DRI implementation. The GLX import
excludes `glxdriswrast.c`, `glxdricommon.c`, and `glxdri2.c`; Xmin supplies its own
direct OSMesa provider instead. `dri3proto.h` is
present only because the current Present protocol header uses its sync-object wire
type. RENDER, DAMAGE, and XFIXES are full implementation imports; RANDR includes its
single-screen Xinerama adapter. XInput/XI is complete and includes the generic
virtual-server DDX hooks. Present is complete and selects its timer-backed fake-vblank
mode because the vfb DDX supplies no hardware CRTC/flip hooks. RANDR's single-screen
Xinerama adapter is retained, while full PanoramiX is not. The core Xext import implements
BIG-REQUESTS, Generic Event, SHAPE, SYNC, XC-MISC, XTEST, MIT-SCREEN-SAVER, and
conditional MIT-SHM. The complete non-DRI software-sync
implementation supplies ordinary software fences and optional screen-installed fd-fence
hooks; the shared DRM fence implementation is excluded. The retained libXfont2 subset
implements only the built-in `fixed`/`cursor` FPE and the PCF machinery it needs. Its
source arrays are converted to uncompressed data during maintenance, so no compression,
FreeType, Fontconfig, filesystem-font, or font-server implementation is retained. MI and
fb are full implementation imports.
The selected OS implementation provides polling, local access
control, clients, connection setup, request I/O, logging, timers, allocation helpers,
MIT-MAGIC-COOKIE-1, and authority-file loading. It intentionally omits XDMCP, secure
RPC, XDM-AUTH-1, platform-specific backtraces, and an active input thread. The generic
no-op `xorg_backtrace` fallback is retained to keep the OS ABI closed portably.
The vfb DDX is retained with recorded patches for Xmin defaults, naming, optional
GLX registration, bounded XWD metadata, and framebuffer resource cleanup.
Xorg normally obtains SHA-1 from a host crypto package for its RENDER glyph cache;
Xmin instead implements the three-function `x_sha1_*` API in project-owned support
code, so it is not part of the third-party import or an external dependency.

The pinned OSMesa source list is compiled twice when GLX is enabled: a namespaced
static archive is linked into the Xmin server, while an ordinary-symbol copy is linked
into the separately shipped `libGL.so.1`. This duplicates no renderer within one
process and avoids both host-GL symbol collisions in the server and forwarding shims
in the client ABI. `tools/import-osmesa.sh` applies the recorded Xmin patch that adds
an `OSMesaMakeCurrentSeparate` entry point, separate draw/read framebuffers, and the
corresponding Mesa read-framebuffer state fix. This lets the bundled direct GLX bridge
and the in-server indirect provider bind different-sized draw and read surfaces without
importing DRI or DRM machinery.
