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
