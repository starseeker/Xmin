# Tests

The suite is layered so protocol results are not trusted solely because the
server and client share code.

`xmin.foundation` exercises checked arithmetic, wire byte order, typed
resources, atoms, regions, surfaces, rendering, state transitions, event
transactions, clocks, input, and extension models directly. `xmin.platform`
covers display reservation, stale state, sockets, authority files, random
cookies, ownership, and shared-memory facilities.

The raw C++ handshake and standalone clients cover native and opposite-endian
setup, authentication rejection, fragmented and malformed input, bounded
output, multiple clients, server grabs, teardown, dynamic display allocation,
signals, sockets, and locks without using XCB.

Independent host-XCB programs cover:

- core windows, resources, properties, selections, events, fonts, images,
  GCs, drawing, clipping, copy operations, and readback;
- exact extension advertisement and general extension framing;
- XTEST input/focus/grab/crossing behavior;
- XFIXES, RANDR, DAMAGE, Composite, Present, XKB, XI2, and MIT-SHM state;
- RENDER raster operators, pictures, glyphs, geometry, and pixel readback; and
- an xkbcommon-x11 consumer over the fixed server keymap.

When `xvfb-run` is available, `xmin.qt-client-core-xvfb` and
`xmin.toolkit-xlib-xvfb` run Xmin's own XCB/Xlib compatibility binaries
against Xvfb as a reference server. This opposite-direction check prevents a
matching client and server mistake from appearing correct. It has already
caught lifecycle event-order differences such as the required
`CreateNotify` before `MapNotify`.

Launcher tests verify authenticated startup, simultaneous display allocation,
signal forwarding, and cleanup. Controller automation launches a real target,
discovers and manipulates its window, injects input, waits for DAMAGE
stability, and validates PPM capture.

When client GL is enabled, the public `libGL` tests compile and execute GLSL
1.10, context sharing/copying, draw/read split, pbuffer and window surfaces,
resize, single-buffer flush, double-buffer swap, and X readback where test
Xlib is available. Optional Qt 5/6 and GTK 3 tests add toolkit raster,
clipboard, cursor, resize, screenshot, and OpenGL acceptance.

`xmin.install-relocation-dependencies` stages the package under an arbitrary
prefix, runs the installed launcher/server/controller workflow, checks GL
development files when applicable, and audits every installed executable/DSO
with `ldd` or `otool` for forbidden host graphics, font, crypto, and DRM
dependencies.

Run the normal, optimized no-GL, and instrumented gates with:

```sh
ctest --preset dev
ctest --preset minimal
ctest --preset sanitizer
```

Set `XMIN_REQUIRE_TOOLKIT_TESTS=ON` in the release job intended to enforce Qt
and GTK availability. Toolkit libraries remain test-only.

## Seeded desktop stress

The interactive stack has a reproducible lifecycle workload which starts an
authenticated Xmin session, JWM, st, and the separate GLFW viewer. By default
it mixes guest moves, resizes, map/unmap cycles, stacking changes, concurrently
resizes the GLFW host window, creates and closes temporary terminals, detaches
and reattaches the viewer, injects a final terminal command, and captures the
composed root. Run both capture transports when changing the viewer, event
ordering, window lifecycle, geometry, or image paths:

```sh
xvfb-run -a tests/desktop_stress.sh \
  --build-dir .build --iterations 500 --seed 1
xvfb-run -a tests/desktop_stress.sh \
  --build-dir .build --iterations 500 --seed 1 --no-shm
```

The first form exercises MIT-SHM capture. The second exercises the portable
tiled `GetImage` fallback retained for hosts without Unix shared memory. The
workload is intentionally not a default CTest because it needs a host display
and is substantially longer than the protocol tests. Use `--resize-only` to
reproduce the original resize workload, or tune `--host-iterations`,
`--lifecycle`, and `--reattach` to isolate a layer. Within an existing Xmin
session, `xminctl stress-window --iterations N --seed N WINDOW` runs the mixed
guest workload; `stress-resize` retains the resize-only form.

## Upstream conformance material

X.Org's X Test Suite (XTS) is useful as an assertion catalog, not as a harness
to vendor. Its old TET runner is Artistic-licensed; individual XTS test
definitions generally carry permissive MIT/X grants plus historical notices.
Before reusing any concrete code or data, audit that source file and retain
all of its notices. When only protocol behavior is relevant, implement the
assertion independently from the published X11 specification in Xmin's raw
wire or host-XCB tests. This keeps the normal suite small and makes failures
specific to Xmin's implementation.

The practical workflow is:

1. Map each supported core request and extension operation to focused success,
   exact-error, byte-order, event-order, and resource-lifecycle assertions.
2. Run the same independent client against Xmin and Xvfb/Xorg when a behavior
   needs a differential reference.
3. Use X.Org `rendercheck` as an optional RENDER diagnostic, then translate
   relevant failures into fast native regressions. For example:

   ```sh
   .build/bin/xmin-run --server .build/bin/Xmin --screen 64x64x24 -- \
     rendercheck --minimalrendering --sync -t fill,blend
   ```

`x11perf` is a performance workload rather than a conformance authority, and
Piglit is useful only for the GLX/OpenGL boundary. Xorg's internal DIX tests
are coupled to Xorg implementation details and are not appropriate to import.

References: [X.Org testing](https://www.x.org/XorgTesting/),
[XTS release and license](https://wiki.freedesktop.org/xorg/Other/Press/XTSReleased/),
and [rendercheck](https://www.x.org/releases/X11R7.5/doc/man/man1/rendercheck.1.html).
