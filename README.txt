The intent of this repository is to define a minimalist but complete, stand-alone
virtual-framebuffer X server that can provide headless X11 graphics rendering portably.

The project and executable are named Xmin ("minimal X") to distinguish the product
from upstream Xorg's Xvfb while retaining a conventional X-server-style name.

It is similar in spirit to https://github.com/starseeker/osmesa - intended to make
X11 graphics rendering universally available in a self-contained codebase with minimal
or (ideally) no external dependencies or reliance on graphics hardware.  It should remove
any code not needed for that purpose.

We want to be portable, standards compliant, use CMake for our build, and can use
C11/C++17 for our implementations.

https://gitlab.freedesktop.org/xorg will be our source for code, but we will pull
in only what we need.  Likely sources we will need subsets from include (but probably
are not limited to):

https://gitlab.freedesktop.org/xorg/lib/libx11
https://gitlab.freedesktop.org/xorg/lib/libxcb
https://gitlab.freedesktop.org/xorg/xserver/-/tree/main/hw/vfb

libX11 and libxcb are client-side libraries rather than server dependencies.  They may be
reference sources or test dependencies, but the Xmin server will implement the corresponding
wire protocols without linking to them.

Our initial target is to map out an implementation plan - we will likely need to handle a few
modern extensions like randr if we can to allow Qt to talk to us successfully, but in cases
where something would pull in a large dependency stack (worried about fonts, but probably other
cases) we should look to using things like https://github.com/starseeker/struetype or
https://github.com/starseeker/osmesa and accept a possibly slightly more limited feature set
rather than shooting for absolute matching fidelity.  I remember a concern about keyboards being
an issue - a sane minimal default should be acceptable there rather than embracing huge complexity.

The initial architecture, required component inventory, extension profile, and implementation
sequence are documented in DESIGN.md.

The repository scaffold, CMake options, build commands, and source-import workflow are
documented in BUILDING.md.  The current Xmin executable enters Xorg's real DIX lifecycle,
opens a local X11 display, and serves the selected protocol profile from an in-memory vfb
screen.  The imported foundation includes selected xorgproto headers, generic-C pixman, portable Xorg libc
fallbacks, and the complete upstream Xorg DIX source list: core objects, dispatch, input,
events, fonts, byte swapping, extensions, and server lifecycle.  The complete upstream
machine-independent drawing and software framebuffer layers are also included.  Xorg's
poll/client/I/O/logging/authentication OS layer now compiles with an internal local-only
xtrans transport and the two-function libXau authority reader; no host xtrans or libXau is
needed.  The vfb input/output DDX also compiles with Xmin-configured defaults and explicit
framebuffer cleanup.  The complete server-side RENDER, RANDR, Present, COMPOSITE,
DAMAGE, XFIXES, DOUBLE-BUFFER, and MIT-SCREEN-SAVER implementations and the complete
XInput 1.x/XI2 server now compile as separate components
and are attached to the vfb link graph.  XInput uses the upstream virtual-server DDX hooks,
so physical-device creation and reconfiguration are rejected without weakening the virtual
core pointer and keyboard protocol.
The core BIG-REQUESTS, Generic Event, SHAPE, SYNC, XC-MISC, XTEST, and conditional
MIT-SHM implementations now compile too, along with software fence support and the static
extension registry.  The fd-fence hooks require an explicit screen implementation and do
not add DRI or DRM behavior.
Present uses Xorg's timer-backed fake-vblank mode, providing queued presentation events
and a monotonic MSC clock without enabling hardware Present, DRI, or DRM paths.
Legacy Xinerama queries are served from RANDR's single-screen monitor layout; the
multi-screen PanoramiX implementation is not included.
The complete XKB protocol, state, controls, actions, and event implementation now compiles
with a pinned US PC keymap embedded as XKM data.  Xmin reads it through Xorg's own XKM
loader from a libc memory stream, so neither `xkbcomp`, an XKB database, nor temporary map
files are runtime requirements.
The core `fixed` and `cursor` PCF fonts are embedded as uncompressed C arrays and loaded
through a built-in-only libXfont2 FPE.  Xmin does not search filesystem font directories
or require zlib, FreeType, Fontconfig, or a font server. Authenticated XCB tests list,
open, query, and pixel-verify text from both embedded fonts.
Xmin supplies RENDER's small SHA-1 glyph-cache dependency internally rather than discovering
a host crypto library.  The retained core archive graph now closes using only platform C,
math, and thread APIs.  `xmin-run` safely allocates a display, creates a private
MIT-MAGIC-COOKIE-1 authority file from the OS cryptographic random source, exports
`DISPLAY` and `XAUTHORITY`, supervises a child, and cleans up.  Dynamic display
reservations are lock-protected and tested with simultaneous launchers.  Lifecycle
tests also cover stale-lock recovery and signal-driven removal of the child, private
authority state, display lock, and Unix socket.
The companion `xminctl` executable is a self-contained automation client built from
a private static libxcb subset.  It authenticates to the local display, recursively
discovers windows, controls their state and geometry, injects pointer/keyboard input
through XTEST, waits for DAMAGE quiescence, and captures root or individual-window
pixels directly as P6 PPM.  The convenience `xmincapture.sh` produces a desktop and
per-window capture set without host `xwd`, `xwininfo`, Xlib, or libxcb.
Authenticated XCB integration tests exercise real RENDER opaque/alpha composition,
A8 trapezoids and uploaded glyphs with pixel readback, RANDR topology and output
properties, exact configured extension advertisement, BIG-REQUESTS/Generic Event/
XC-MISC negotiation, regions,
selection/cursor notifications, shapes, synchronization, reversible XKB state, and
multi-key XTEST input, COMPOSITE redirection/DAMAGE events, fake-vblank PresentPixmap
completion/idle/future-MSC notification, DOUBLE-BUFFER swaps,
single-screen Xinerama/saver state, and conditional MIT-SHM image transfer.
Another core-protocol gate round-trips properties, window hierarchy/configuration,
geometry, pixmap copy/readback, selection ownership, named colors, and synthetic
events.  The opposite-endian raw client verifies malformed core `BadLength` recovery.

With `XMIN_BUILD_INDIRECT_GLX=ON`, Xmin embeds the pinned starseeker/osmesa renderer
and Xorg's non-DRI indirect GLX implementation.  A project-owned provider maps GLX contexts and CPU
drawables directly to OSMesa; a raw-wire integration test creates an indirect context,
renders and swaps a fixed-function triangle, binds different-sized draw/read windows,
verifies both GL readback and X11 pixel output, and proves single-buffered `glFlush`
presentation plus pbuffer render/read/release.  No DRI, DRM, host Mesa, or loadable
GL driver is used.  The raw test also wraps a core pixmap as a GLXPixmap, synchronizes
it with `WaitGL`, and reads the resulting X pixels.  This proves the legacy indirect
server path.  The
Independently, `XMIN_BUILD_CLIENT_GL=ON` builds the separately shipped
`lib/xmin/libGL.so.1`, which supplies the conventional GL/GLX client ABI
with a second, standard-symbol OSMesa build.  `xmin-run` selects it for the child
through a package-relative library path, so it requires no system installation.
Tests create direct OpenGL 2.0 contexts, share texture objects, compile and render with
GLSL 1.10, bind different-sized GLX draw and read targets, and use ordinary
Xlib-created GLX windows and pixmaps for swap/flush and Xmin pixel readback.  Direct
drawable color storage survives context switches, including switches between sharing
contexts.  Window presentation remains valid across a resize without re-creating or
re-binding the context, and single-buffered drawables present on `glFlush`.  The
OSMesa color format is selected as BGRA on little-endian hosts and ARGB on big-endian
hosts so its bytes represent the same native X pixel value.  The client DSO has no
host X11 dependency of its own: it uses weak Xlib entry points
already supplied by a GLX application's X client library and currently presents
through core `XPutImage`.  Qt/GTK and target-application acceptance, MIT-SHM upload
optimization, and additional GLX compatibility entry points remain release-hardening
work.
