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

The modernization branch also builds `Xmin-next`, the independent C++17 replacement
described in `modernize.txt`.  It no longer needs an inherited test socket: it owns a
direct Unix-domain X11 listener, atomically reserves dynamic or explicit displays,
recovers stale locks and sockets, reads bounded binary Xauthority records, assigns a
distinct resource-ID range to each client, and multiplexes bounded nonblocking
connections through one `poll` loop.  Core server grabs mute request processing for
other clients without blocking their output, and release on owner ungrab or disconnect.
SIGINT/SIGTERM wake that loop through a pipe so
normal RAII cleanup removes the display socket and lock.  Native- and opposite-endian
raw clients cover fragmented setup/request input, authentication rejection, malformed
length recovery, concurrent display allocation, simultaneous clients, and lifecycle
cleanup.  A single shared `ServerState` now owns atoms, bounded resources, and the
window tree across clients; committed declarations generated from `xproto.xml` drive
a bounds-checked semantic handler table.  The first window/atom requests cover create,
map/unmap, destroy, attributes, geometry, hierarchy, interning, and reverse lookup,
including cross-client visibility and disconnect teardown.  Window configuration and
border-aware coordinate translation now match the legacy oracle, while bounded
properties support replace/prepend/append, partial reads, listing, deletion, and
canonical 8/16/32-bit storage across clients of opposite byte order.  Atomic
property-list rotation preserves typed payload buffers and supports signed deltas;
framebuffer-compatible best-size queries clamp cursors and round short tile/stipple
widths.  A typed input snapshot starts the pointer at the legacy-compatible screen
center, answers pointer, empty motion-history, and key-state queries, and applies
source-gated/clamped warps.  Its initial US core map is a generated constexpr value;
keyboard, modifier, repeat, pointer-button, and feedback-control queries expose the
same defaults as the legacy oracle without loading XKM at runtime.  Pointer acceleration,
core keysyms, keyboard feedback/repeat, pointer-button mapping, and modifier mapping are
mutable with atomic validation, bounded dynamic keymap widths, and protocol-compatible
busy replies.  Successful keysym, modifier, and pointer mapping changes broadcast typed
`MappingNotify` events with each recipient's enqueue-time request sequence; bell requests
validate their signed percentage while remaining deliberately silent.  Typed focus state
distinguishes the protocol's `PointerRoot` sentinel from the server-owned root XID,
honors request timestamps,
and applies parent/none/pointer-root reversion when windows become unavailable.
The first constexpr extension-registry entry advertises XTEST 2.2 at a stable opcode.
Its typed request path negotiates versions, validates cursor/grab-control requests, and
feeds immediate core key, button, and absolute/relative motion injection into the shared
input engine.  That engine hit-tests mapped windows, applies focus and do-not-propagate
rules, routes typed core events through normal selections or active/passive grabs, stamps
each recipient's sequence at enqueue time, and commits device state only after successful
delivery.  Motion and core pointer warps also share an atomic crossing-event planner for
ancestor, descendant, and nonlinear window transitions.  Normal button delivery creates
the protocol's automatic pointer grab, retains it across button chords, and generates
typed grab/ungrab crossings on first press and final release.  Explicit focus changes
atomically plan typed ancestor, descendant, nonlinear, `PointerRoot`, and `None`
notifications, including `NotifyPointer` runs along the current pointer path.  Delayed
mapping mutations now plan stationary-pointer crossings and focus reversion as one
atomic event/state transaction for `MapWindow`, `UnmapWindow`, `MapSubwindows`, and
`UnmapSubwindows`, including rollback when any recipient queue lacks capacity.  Delayed
scheduling, destruction now composes that transition with no-fail subtree erasure, and
the same external oracle verifies its focus/crossing order against both servers.
Reparenting plans Xorg's old-tree unmap and new-tree remap paths in one atomic batch,
using no-throw vector swaps to preserve stacking, geometry, mapping, and focus on
failure.  Pointer grabs whose grab or confinement window loses viewability now emit
the typed `NotifyUngrab` path before the normal crossing and release only when the
whole transition commits.  Keyboard-grab view-loss notifications and repeat timers
remain later vertical slices.
Client-owned active pointer and keyboard grabs keep typed modes, masks, confinement,
timestamps, cross-client exclusion, and disconnect/window teardown.  Passive key and
button grabs use bounded bit domains for `AnyKey`/`AnyButton` and `AnyModifier`, with
atomic wildcard subtraction, cross-client conflict checks, and lifecycle cleanup.  A
bounded four-format `Surface` value now backs windows and typed pixmap records; the first GC
path applies all 16 core raster functions and plane masks to clipped rectangle fills,
overlap-safe copies, plus bounded ZPixmap upload/readback for A1, A8, XRGB8888, and
ARGB8888 in the setup-advertised image and bitmap order.  Every GC raster path is
clipped through a typed, pixman-canonicalized region value; `SetClipRectangles` keeps
origins independent and overlapping inputs draw only once.  Window-local contents are
composed lazily into a separate root image with stacking, borders, nested ancestor
clipping, and lossless map/unmap behavior, so capture never destroys root backing pixels.
Its deliberately small
request implementation now also keeps timestamped selection ownership and bounded
recipient-neutral event queues in shared state, encoding selection-clear and synthetic
client-message events in each recipient's byte order.  The fixed TrueColor colormap
supports compact named/hexadecimal allocation and pixel queries.  An independent host-XCB
oracle passes this complete core slice against both servers.  The remaining core surface
is still a migration target;
the Xorg-backed `Xmin` remains the feature-complete server and differential oracle.

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
