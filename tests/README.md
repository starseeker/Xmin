# Test organization

Keep new fast project-owned unit tests under `tests/unit`, X11 wire tests under
`tests/protocol`, process and authentication tests under `tests/integration`, toolkit
acceptance tests under `tests/qt`, and GLX/OpenGL tests under `tests/glx`. Earlier
focused tests remain directly under `tests` while the suite is reorganized. External
Xlib, XCB, Qt, GTK, and GL implementations are test-only dependencies and must not
enter Xmin's installed link interface. When Qt 5/6 Gui or GTK 3 development files
are present, CMake automatically adds their raster acceptance tests. Qt builds also
add an OpenGL 2.0/GLSL test when the bundled client bridge is enabled; every toolkit
test executes through the authenticated launcher.

The integration suite intentionally uses raw X11/GLX wire requests and the embedded
libXau subset, so its baseline has no host X client or OpenGL dependency. It covers
the server setup/extension handshake and GLX 1.4/FBConfig negotiation in native and
opposite byte orders, a swapped malformed-GLX `BadLength` response, a swapped
multi-byte core geometry request, private launcher authentication, concurrent dynamic
display allocation, stale-lock recovery, and complete signal-driven launcher cleanup
of its command, authority state, lock, and socket. It also covers an indirect
fixed-function GLX render/swap/readback. The
`xmin.control-automation` gate additionally uses the private bundled XCB subset to
start a named target window, focus it, inject precise pointer and keyboard events,
including delayed clicks and a stepped modified drag, wait for DAMAGE quiescence,
verify both per-window and root PPM pixels, send
`WM_DELETE_WINDOW`, and confirm clean application exit. It does not discover or link
host X11 client libraries. The
indirect GLX test also binds different-sized draw/read windows, verifies `glReadPixels`
uses the read window, verifies rendering and swap target the draw window, and proves
that a single-buffered indirect `glFlush` presents without a context switch. It also
creates a pbuffer through GLX 1.3, renders and reads it, unbinds, and destroys it. The
GLXPixmap case copies color-buffer state between indirect contexts, wraps a core
pixmap, renders with that state, synchronizes through `WaitGL`, validates the X
pixels, and tears down both resources. The
optional XCB integration test authenticates through `xmin-run`, checks screen and
the exact configured extension list, lists/opens/queries/renders both embedded core
fonts, then
creates, draws, and reads back a window through XCB. When
the generated extension bindings are installed, a second XCB test exercises RENDER
and negotiates BIG-REQUESTS, Generic Event 1.0, and XC-MISC 1.1 before exercising
RENDER formats, opaque and premultiplied-alpha composites, A8 trapezoids and glyph sets
with pixel readback, RANDR output/CRTC/mode and output-property state, XFIXES regions/selection/cursor notification,
SHAPE bounds, SYNC counters, single-screen Xinerama layout, SCREEN-SAVER state/event
selection, XI2 master devices, reversible XKB lock state, two-key US-map XTEST
pointer/keyboard injection, COMPOSITE redirection,
DAMAGE notification, PresentPixmap complete/idle/future-MSC notifications and
readback, and DOUBLE-BUFFER swap/readback. With
MIT-SHM enabled it additionally performs a real SysV
attach, shared-memory upload, readback, and detach; the minimal profile verifies
omission and client fallback discovery.
The core XCB companion covers properties, window hierarchy/configuration,
geometry/translation, mutable window/GC state, background clearing, pixmap
copy/readback, selection ownership, fixed TrueColor colormap lifecycle and color
queries, and synthetic events. The raw
opposite-endian setup test also checks a malformed core
request receives `BadLength` without terminating the connection. The `Xmin-next`
variant additionally exercises the same semantic handlers in both byte orders for
atoms, window lifecycle/configuration/reparenting/subwindow operations/coordinate
translation, mutable attributes and GCs, background clearing, and property mutation,
partial retrieval, listing, deletion, selection ownership, fixed TrueColor named-color
queries and colormap install/copy/free semantics, and synthetic client-message
delivery. It checks an unadvertised opcode at 255
cannot escape the 128-slot core table, and proves that one client's window is visible
to another while a 32-bit property remains correctly encoded for the opposite-endian
client after disconnect teardown.
The next-server graphics vertical slice independently covers surface allocation caps,
plane masks, overlap-safe self-copy, clipped point/line/segment/rectangle drawing, and
pixmap-to-window fill/copy/readback in native and opposite client byte orders while
respecting the setup image byte order. Raw uploads
cover 32-bit pixels and padded 1-bit scanlines in both client byte orders. Scene tests
cover root backing preservation, borders, nested clipping, map/unmap restoration, and an
independent XCB child-to-root pixel readback shared with the legacy oracle.
The OSMesa unit check confirms that the renderer itself reports OpenGL 2.0, that the
host-endian BGRA/ARGB choice produces a native X `0x00RRGGBB` pixel, and that distinct
draw/read memory buffers work. The software-direct tests go further: they
compile/link GLSL 1.10, bind different-sized draw/read pbuffers, verify shared
texture objects and drawable color persistence across context switches, and copy
attribute state with `glXCopyContext` through the public `libGL` ABI. When test-only Xlib is available they also cover modern GLX
windows, resize without context rebinding, GLXPixmap presentation, double-buffered
swap, single-buffered `glFlush`, and X image readback. The Qt tests cover xcb-platform startup, clipboard,
cursor, backing-store painting, resize, screenshot readback, and an OpenGL 2.0 GLSL
render with both GL readback and post-swap X-window screenshot validation. The GTK 3
test covers startup, drawing-area painting, and screenshot readback.
They are omitted, with no product-build impact, on hosts where those external test
packages are unavailable. Set `XMIN_REQUIRE_TOOLKIT_TESTS=ON` in a release/CI build
to make configuration fail unless at least one Qt generation and GTK 3 are present;
the configuration summary always reports which toolkit targets were enabled.
