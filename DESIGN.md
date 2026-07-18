# Xmin architecture

## Product boundary

Xmin is a fixed-profile X11 server for deterministic headless software
rendering. The wire protocol is the compatibility boundary. Xorg source ABI,
DDX hooks, server generations, modules, and implementation structure are not.

The product has one virtual screen, root, output, CRTC, mode, pointer, and
keyboard. Root storage is XRGB8888 with a 24-bit TrueColor visual. The only
transport is a local Unix-domain socket and the normal authentication method
is MIT-MAGIC-COOKIE-1.

`profiles/protocol.json` is the product contract. Its generated report
enumerates every core slot and every request in each declared extension,
including compatibility, platform-conditional, and deliberately unsupported
requests. The server's compiled extension registry has stable opcodes, event
bases, error bases, versions, and handler identities.

## Dependency direction

```text
Unix socket -> Connection/wire codec -> semantic request handlers
                                              |
                                              v
                                         ServerState
                         +--------------------+-------------------+
                         |                    |                   |
                  typed resources      window/input        extensions
                         |                    |                   |
                         +------------- surfaces/regions --------+
                                              |
                                        renderer -> pixman
```

`src/server` contains the platform, protocol, state, graphics, input, and
extension implementation. `src/launcher` and `src/control` depend only on the
narrow platform/authentication surface and their own command logic. Product
code never includes an Xorg server header.

Pixman is an internal C raster engine behind Xmin value types. Optional OSMesa
exists only in the separately installed client `libGL`. Its private context
and dispatch types are confined to `xmin_osmesa_adapter.c`; the GLX bridge
is C++17 internally and uses that adapter through an opaque `extern "C"` API.
The server implements the standard GLX 1.4 direct-context control plane, but
never executes an indirect OpenGL command stream.

Pixman remains separate deliberately. It supplies the mature CPU algorithms
for region algebra, format conversion, Porter–Duff operators, masks and
component alpha, transformed/filtered images, gradients, trapezoids, and
glyphs. Replacing it would mean writing and validating a new pixel engine,
not merely deleting an abstraction layer. Xmin therefore owns the X11 raster
semantics and exposes no pixman API, while the pinned static library owns that
algorithmic kernel. This boundary also makes a future no-pixman size
experiment possible without disturbing protocol or server state code.

Pixman is also the more portable part of a future Windows port: the pinned
code has explicit MSVC/MinGW handling and this build selects generic C raster
paths rather than platform assembly. Because the renderer is an internal part
of the deliberately single-threaded server, pixman is built with
`PIXMAN_NO_TLS`: its fast-path cache is process-local and the server acquires
no pthread or Win32-thread dependency. This does not disable parallel raster
work—pixman does not create workers—but it means the renderer must remain on
the server thread. Xmin's current Windows blockers are above that boundary:
Winsock/AF_UNIX adaptation, poll and signal wakeups, process supervision,
temporary-file permissions, and the SysV-SHM policy. The platform library
keeps those concerns out of protocol and graphics code so they can be
replaced with Win32 implementations later.

## Ownership and lifetime

One `ServerState` owns all mutable server state. A `Server` owns the event
loop and connections. Each `Connection` owns its file descriptor, bounded
input/output queues, byte order, sequence, resource range, authentication
state, subscriptions, and scheduling state.

Resources live in a registry keyed by XID and represented by a closed typed
variant. Lookups validate type and return the exact protocol error. The owner
index makes disconnect teardown deterministic. Shared surface ownership is
explicit only where the protocol requires it, such as Composite named pixmaps
or pictures that outlive an XID.

File descriptors, display reservations, sockets, shared-memory attachments,
pixman objects, and temporary launcher state use RAII. Construction commits
state only after all validation and allocation succeeds. There is no reset
generation and no mutable process-wide server singleton.

## Wire boundary

`WireReader` and `WireWriter` read and write fixed-width values without
casting network memory to C structs. Client byte order is handled once at the
boundary; native and swapped clients use the same semantic handlers.

Generated declarations from xcb-proto XML provide opcode identity and
coverage metadata. Handwritten handlers own policy and validation. Checked
arithmetic guards request lengths, list sizes, strides, resource counts,
surface bytes, properties, glyphs, and output queues before allocation.
Malformed requests produce the specified error or a bounded disconnect; they
do not abort the server.

## State and event transactions

Window geometry, mapping, stacking, properties, selections, focus, grabs,
pointer/keyboard state, XKB state, extension subscriptions, and scheduled
work are ordinary typed C++ values. State-changing requests plan their
events, verify recipient queue capacity, and then commit state and events as
one transaction.

All input—including XTEST and `xminctl` injection—uses one focus, propagation,
crossing, grab, and state path. XI2 and XI1 are views of that state rather
than independent device engines.

An injected monotonic `Clock` drives key repeat, saver state, SYNC waits, and
the 60 Hz software Present MSC. Foundation tests advance a fake clock rather
than sleeping. The poll loop computes the next deadline and remains
single-threaded.

## Graphics model

`Surface` owns checked dimensions, stride, format, pixel storage, and its
pixman image. It can also retain a checked writable shared-memory mapping for
a 24/32-bit MIT-SHM pixmap. Supported internal formats are A1, A8, XRGB8888,
and ARGB8888.
`Region` is the common algebra for clipping, SHAPE, XFIXES, DAMAGE, Composite,
Present, and scene invalidation.

Windows retain independent surfaces. The root scene compositor resolves
mapping, stacking, geometry, borders, shapes, children, and Composite
redirection lazily for dirty regions. Pixmaps and retained named-pixmap
surfaces use the same drawable model.

Every drawable mutation takes one path that applies raster semantics, records
damage, invalidates composition, and wakes dependent Present work. Core GCs
are values rather than operation tables. Pixman handles composition and image
operations; compact project raster code handles the X-specific line, arc,
polygon, dash, tile, and stipple rules.

The fixed and cursor fonts are compiled constexpr glyph tables. XKB uses a
canonical constexpr US map. There is no runtime PCF/XKM parser, font search,
rules database, subprocess, or temporary keymap.

## Extension views

- RENDER uses typed pictures, glyph sets, transforms, clips, alpha maps, and
  the shared renderer.
- SHAPE and XFIXES use the canonical region type.
- DAMAGE subscribes to the central drawable mutation path.
- Composite redirects window surfaces and gives named pixmaps explicit shared
  lifetime.
- RANDR exposes one software output/CRTC/mode with bounded property, monitor,
  transform, panning, and gamma state.
- Present uses software MSC deadlines, surfaces, regions, DAMAGE, and SYNC
  fences; GPU flips, DRM leases, and DRI3 sync objects are rejected.
- MIT-SHM shared pixmaps can back client OSMesa output. Present copies each
  submitted frame once into server-owned window storage before the client may
  reuse the buffer; writable client mappings are never aliased as live window
  storage. Platforms without descriptor passing use the ordinary image-upload
  path.
- GLX exposes version/config/context/drawable management for direct OSMesa
  clients and rejects Render/RenderLarge indirect streams.
- XKB, XI2, XI1, and XTEST share the single input engine.
- DBE, screen saver, and XINERAMA are small compatibility views rather than
  retained subsystems.

## Commands and packaging

Thin executables were chosen from measured size and responsibility:

- `Xmin` owns server configuration and serving;
- `xmin-run` owns credentials, display allocation, supervision, and cleanup;
- `xminctl` owns automation and PPM capture.

The split avoids a mode-dispatch binary while allowing the launcher and
controller to remain independently useful. The optional GL DSO is packaged
under `lib/xmin`, outside the server dependency graph.

## Deliberate non-goals

Xmin does not provide TCP, multiple screens, hardware input, device hotplug,
DDX/modules, DRI/DRM, GPU Present, indirect GLX, EGL/Vulkan, XVideo, DGA,
VidMode, DPMS, XDMCP, XACE/SELinux, SECURITY, RECORD, arbitrary root depths,
runtime font paths, runtime keymap compilation, or server resets.

Adding one of these is a product decision requiring a manifest change,
independent tests, a security/resource-limit review, and measured complexity.

## Verification invariants

- all 127 core slots and all 383 declared extension requests remain covered;
- raw native and opposite-endian clients share semantics;
- independent host XCB/toolkit tests do not reuse the server codec;
- resource and queue limits are exercised, including malformed lengths;
- sanitizer, optimized no-GL, optional GL, install relocation, and dependency
  audits are first-class gates; and
- installed binaries load no host X11/XCB/GL/font/crypto/DRM stack.
