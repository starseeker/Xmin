# Building Xmin

Xmin now builds a functional local-socket X11 server from selected, embedded upstream
sources: X11 protocol headers, generic pixman, portable Xorg libc fallbacks where the
host needs them, and Xorg's complete DIX source list. This
includes core object lifecycle, request dispatch, swapped-client handling, input and
events, font requests, extension registration, and server lifecycle. The complete
machine-independent drawing and linear framebuffer implementations also compile.
The selected Xorg OS layer, Unix-domain xtrans backend, authority-file reader, and
MIT-MAGIC-COOKIE-1 implementation compile without host X11 libraries. Focused tests
exercise atoms, callbacks, resources, protocol byte swapping, Xorg regions, pixman
regions, framebuffer solid fills, authority records, cookies, and a real local socket
listener. Complete server-side RENDER, RANDR, Present, COMPOSITE, DAMAGE, XFIXES,
DOUBLE-BUFFER, and MIT-SCREEN-SAVER targets are also present along with the complete
XInput 1.x/XI2 server and virtual-server DDX hooks.
Tests cover picture transforms, RANDR transforms, damage-region reporting, XFixes
disconnect policy, XI property conversion, and Xmin's internal SHA-1 implementation
used by the RENDER glyph cache. BIG-REQUESTS, Generic Event, SHAPE, SYNC, XC-MISC,
XTEST, conditional MIT-SHM, and the MI static extension registry also compile.
Focused tests cover software-fence transitions and Present MSC scheduling. Present
uses the timer-backed fake-vblank path and has no DRI/DRM dependency. Complete XKB server
logic also compiles with a pinned US map loaded from an embedded XKM byte stream;
tests cover allocation and actual symbols/modifiers without invoking `xkbcomp`.
The retained built-in subset of libXfont2 reads uncompressed `fixed` and `cursor`
PCFs from committed arrays; a test opens both fonts and validates an actual glyph.
RANDR's compact Xinerama adapter reports its single virtual layout without importing
the multi-screen PanoramiX implementation. An integration test starts the actual
server on a dynamically allocated display, completes the raw X11 setup exchange,
queries the selected extension profile, repeats setup/extension/geometry requests as
an opposite-endian client, and checks clean signal-driven shutdown. When test-only
XCB is available, another authenticated integration test queries the complete
extension profile, lists/opens/queries/renders the embedded core fonts, and performs
window drawing/readback through the same client API used by Qt's xcb platform
plugin. It also requires the exact configured normal/minimal extension list. A
companion XCB test negotiates BIG-REQUESTS, Generic Event 1.0, XC-MISC 1.1, and
exercises
RENDER formats, opaque and premultiplied-alpha compositing, A8 trapezoids and glyph
sets with pixel readback, RANDR output/CRTC/mode and output-property state, XFIXES
regions/selection/cursor events, SHAPE bounds, SYNC counters, single-screen Xinerama
layout, SCREEN-SAVER state/event selection, XI2 master devices, reversible XKB lock
state, and pointer plus two-key US-map XTEST injection,
COMPOSITE redirection/DAMAGE notification, fake-vblank PresentPixmap
complete/idle/future-MSC notifications and readback,
and a DOUBLE-BUFFER back-buffer swap/readback. When MIT-SHM is enabled it also
performs a real SysV attach, checked shared-memory image upload, readback, and detach;
the forced-disabled profile verifies the extension is absent.
An additional authenticated core-XCB gate covers properties, window hierarchy and
configuration, geometry/translation, pixmap copy/readback, selection ownership,
named colors, and synthetic events. The raw opposite-endian handshake test also
requires `BadLength` for a malformed core request and then successfully continues.
The authenticated `xmin-run` launcher and embedded OSMesa-backed indirect GLX server
are implemented. Integration tests cover cookie rejection/acceptance, simultaneous
dynamic display allocation, stale-lock recovery, and signal-driven cleanup of the
supervised command, authority state, lock, and socket. GLX tests cover native and
opposite-endian GLX 1.4 setup plus malformed-length rejection, indirect fixed-function rendering, swap,
different-sized indirect draw/read windows, GL readback, single-buffered flush
presentation, pbuffer render/read/release, and X11 pixel readback. A separately
covered GLXPixmap path copies context state, renders with that state, synchronizes
through `WaitGL`, and reads the core X pixmap. A separately installed `libGL.so.1` also implements the software-direct
client path. Tests cover direct 2.0 context sharing, GLSL 1.10
compile/link, drawable-owned pbuffer color storage across shared-context switches,
different-sized draw/read pbuffer bindings, context-state copying, modern GLX
windows, resize without rebinding, GLXPixmap presentation, double-buffered swap, and single-buffered
flush/readback. Both GLX paths select BGRA or ARGB OSMesa storage to match the host X
image byte order. Broader toolkit and application acceptance remains.

The installed `xminctl` client completes the baseline headless automation path. It
uses a private static subset of libxcb, opens Xmin's local socket with the launcher's
MIT cookie, discovers named windows recursively, controls focus/geometry/mapping,
injects pointer and US-keymap keyboard input with XTEST, waits for rendering to
quiesce with DAMAGE, and captures root or individual window pixels as P6 PPM. Its
end-to-end test uses no host X11 client library or X11 utility.

Qt 5/6 Gui and GTK 3 are optional test-only dependencies. If their development
packages are installed, configuration automatically adds `xmin.qt5-raster`,
`xmin.qt6-raster`, `xmin.qt5-opengl`, `xmin.qt6-opengl`, and/or
`xmin.gtk3-raster` as applicable. Their absence does not change the server or
installed package. Release/CI configurations can set
`XMIN_REQUIRE_TOOLKIT_TESTS=ON` to fail rather than silently omit both a Qt
acceptance target and the GTK 3 target.

The vfb DDX input and output sources also compile. Xmin's patch uses the configured
screen defaults, omits GLX vendor registration until GLX is present, labels virtual
devices as Xmin devices, and cleans up mmap files and SysV shared-memory segments.

## Requirements

- CMake 3.21 or newer;
- a C11 compiler;
- a C++17 compiler when `XMIN_BUILD_GLX=ON`;
- a platform thread implementation supported by CMake; and
- a POSIX libc with `fmemopen` for the embedded XKB stream.

No X11, Mesa, zlib, FreeType, Fontconfig, font-data, or input development packages
are used by the normal build.

## Quick start

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/src/server/Xmin --version
```

Without presets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix ./install
```

To run a command on a private authenticated display:

```sh
./build/src/launcher/xmin-run --server ./build/src/server/Xmin \
  -- ./your-graphical-test
```

An installed `xmin-run` finds the adjacent installed `Xmin` without `--server`.
When GLX is enabled it also prepends the package-relative `lib/xmin` directory to the
launched process's loader path, selecting the bundled `libGL.so.1` without modifying
the host installation.

For applications that load the host's separate `libGLX.so.0`, use the repository
helper to preload Xmin's complete GL/GLX client bridge into the application child:

```sh
./xminlaunch.sh --build-dir ./.build --screen 1280x800x24 -- ./your-opengl-app
```

The helper delegates authenticated display allocation and cleanup to `xmin-run`, but
applies `LD_PRELOAD` and `LD_LIBRARY_PATH` only to the application. To inspect a live
display, drive an application, and capture its desktop plus named windows, launch a
shell and use the bundled controller:

```sh
./xminlaunch.sh --build-dir ./.build --screen 1280x800x24 -- bash
# Start one or more graphical programs in this shell.
"$XMIN_CONTROL" wait-window "Application title"
"$XMIN_CONTROL" activate "Application title"
"$XMIN_CONTROL" click --delay 50 "Application title" 100 80
"$XMIN_CONTROL" key-down ctrl
"$XMIN_CONTROL" mouse-drag --steps 8 --delay 20 \
  "Application title" 100 80 160 120
"$XMIN_CONTROL" key-up ctrl
"$XMIN_CONTROL" type --delay 10 "hello"
"$XMIN_CONTROL" key ctrl+s
"$XMIN_CONTROL" wait-stable "Application title"
"$XMIN_CONTROL" capture-window "Application title" window.ppm
"$XMIN_CONTROL" capture-root desktop.ppm
./xmincapture.sh ./xmin-capture
```

`xminlaunch.sh` exports `XMIN_CONTROL`, so the same form works from a supervisor
program or shell. `xmincapture.sh` creates PPMs plus a `windows.tsv` inventory.
Neither command requires `xwd`, `xwininfo`, ImageMagick, Xlib, or a system libxcb.
`xminctl --help` lists the complete command set. Window selectors accept `root`, a
numeric window ID, or an exact/substr WM name; numeric IDs are preferable when titles
are ambiguous.

The configured default screen is 1280x1024x24. `--screen WIDTHxHEIGHTxDEPTH`
selects a different framebuffer for each launch without rebuilding. RANDR may shrink
the reported screen inside that initial framebuffer, but the startup size is its
maximum; start a new Xmin instance to grow it. Keep each dimension at or below 32767,
and keep the 32-bit framebuffer allocation below 2 GiB (roughly
`width * height * 4`). For example, 8192x8192x24 uses about 256 MiB and
16384x16384x24 uses about 1 GiB. These are protocol/legacy-fb safety bounds, not a
1280x900 product limit.

To start an isolated test display explicitly:

```sh
./build/src/server/Xmin :99 -screen 0 1280x1024x24 -ac
```

`-ac` is appropriate only for isolated test environments. Normal deployments should
retain access control and use `-auth` with an MIT-MAGIC-COOKIE-1 authority file. TCP
transport is absent unless explicitly enabled at configure time.

## Build controls

| Option | Default | Purpose |
| --- | --- | --- |
| `XMIN_BUILD_GLX` | `ON` | Embed OSMesa, build the non-DRI indirect GLX server provider, and ship the software-direct `libGL.so.1` client bridge. |
| `XMIN_ENABLE_MITSHM` | `AUTO` | Enable MIT-SHM only when SysV shared memory is detected; also accepts `ON` or `OFF`. |
| `XMIN_ENABLE_TCP` | `OFF` | Compile and listen on xtrans TCP sockets. This is explicit opt-in; local sockets are the normal path and authentication is required outside isolated test systems. |
| `XMIN_PIXMAN_SIMD` | `AUTO` | Control future optional pixman SIMD implementations. |
| `XMIN_BUILD_LAUNCHER` | `ON` | Build the integrated authenticated `xmin-run` child launcher. |
| `XMIN_BUILD_TESTS` | top-level `ON` | Build the unit, protocol, integration, and available application tests. |
| `XMIN_REQUIRE_TOOLKIT_TESTS` | `OFF` | Require at least one Qt 5/6 Gui acceptance target and the GTK 3 target; intended for release/CI gates. |
| `XMIN_ENABLE_INSTALL` | top-level `ON` | Generate executable and documentation install rules. |
| `XMIN_WARNINGS_AS_ERRORS` | `OFF` | Promote warnings in project-owned code to errors. |

The cache variables `XMIN_DEFAULT_WIDTH`, `XMIN_DEFAULT_HEIGHT`,
`XMIN_DEFAULT_DEPTH`, and `XMIN_DEFAULT_DPI` configure the compiled defaults.

## Source organization

```text
cmake/                  options, platform probes, target helpers, config template
src/
  dix/                  X server dispatch and resource core
  os/                   transport, authentication, I/O, and OS abstraction
  mi/                   machine-independent screen and drawing logic
  fb/                   software framebuffer renderer
  extensions/           selected X11 extension implementations
  hw/vfb/               virtual framebuffer DDX
  glx/                  OSMesa-backed server-side GLX integration
  client/glx/           separately shipped GL/GLX/OSMesa client bridge
  launcher/             integrated authenticated process launcher
  control/              self-contained window/input/capture client (`xminctl`)
  support/              Xmin-owned adapters, launcher, and portability code
  server/               thin executable wrapper around the DIX server lifecycle
data/fonts/             generated embedded core fonts
data/xkb/               generated embedded keyboard map
third_party/            isolated imports and stable dependency targets
tools/                  maintenance-time generators and import helpers
tests/                  unit, protocol, integration, toolkit, and GL tests
```

Each architectural directory defines a stable `Xmin::<name>` target through
`xmin_add_component()`. An empty component is an `INTERFACE` library. Adding a
`SOURCES` list turns it into an `OBJECT` library without changing the final server's
link declaration. Use `TYPE STATIC` for isolated third-party libraries where static
archive boundaries are preferable, `COMPILE_FEATURES cxx_std_17` for C++ components,
and `UPSTREAM` so project warning policy is not imposed on imported code.

For example:

```cmake
xmin_add_component(dix
  UPSTREAM
  SOURCES
    atom.c
    dispatch.c
    resource.c
)
```

Generated headers are written under `build/generated`; the source tree is never
modified by configuration. The normal build must remain offline and must not add
`find_package(X11)`, pkg-config discovery of a host graphics stack, or runtime module
loading. Record every actual source import in `UPSTREAM.toml` before enabling it.

## Refreshing imported sources

The normal build never downloads source. To reproduce the current retained import,
download and checksum the archives listed in `UPSTREAM.toml`, extract them outside
the repository, and run:

```sh
sh tools/import-xorg.sh \
  /path/to/xorgproto-2024.1 \
  /path/to/xorg-server-21.1.23 \
  /path/to/pixman-0.46.2 \
  /path/to/libXfont2-2.0.8 \
  /path/to/xtrans-1.6.0 \
  /path/to/libXau-1.0.12 \
  /path/to/libxkbfile-1.2.0 \
  /path/to/xkeyboard-config-2.47

sh tools/import-libxcb.sh \
  /path/to/libxcb-1.17.0 \
  /path/to/xcb-proto-1.17.0
```

The script copies an explicit allowlist for protocol and C sources. Private X server
headers are temporarily retained as a unit because their type graph is tightly
interconnected. RENDER, RANDR (including its compact Xinerama adapter), Present,
COMPOSITE, DAMAGE, XFIXES, DOUBLE-BUFFER, MIT-SCREEN-SAVER, and XInput/XI are complete
source imports. The multi-screen PanoramiX layer is excluded. Present uses its software
fake-vblank mode and retains `dri3proto.h` only for a wire type. The unconditional
Xext baseline,
conditional MIT-SHM source, non-DRI MI software-sync implementation, and static
extension registry are imported as explicit allowlists. The non-DRI GLX dispatcher,
in-server vendor router, and generated request code are imported for the project-owned
OSMesa provider. The shared DRM fence and all GLX DRI providers are excluded. The
complete XKB protocol/state implementation is imported, while its
external compiler, rules parser, and text emitters are excluded from the build. The
embedded XKM reader retains only two libxkbfile format headers. The libXfont2 import is
limited to its built-in FPE, PCF reader, font matching, metrics, and cache utilities.
It omits filesystem FPE registration, compressed formats, scalable renderers, FreeType,
Fontconfig, and remote font servers. MI and fb are complete source imports. The OS
allowlist omits XDMCP, XDM-AUTH-1, secure RPC, platform-specific backtraces,
and asynchronous input-thread logic. The xtrans allowlist contains only the generic
core and Unix socket backend, while libXau contributes only its reader and disposer.
The importer also applies the patches recorded in `UPSTREAM.toml`. These headers will
be reduced after all selected server components compile. Never edit imported files in
place without recording a reproducible patch in the corresponding manifest entry.
The separate libxcb importer retains only the connection core and bindings needed by
`xminctl`; it generates the committed bindings during import, while the normal build
remains offline and has no Python requirement.

The refresh command also uses maintenance-time `xkbcomp` 1.5.0 to compile the
allowlisted `data/xkb/xmin-us.xkb` recipe against xkeyboard-config 2.47. It converts
the resulting XKM file into `data/xkb/xmin-us-map.h`. Neither tool nor source database
is consulted by a normal configure, build, or Xmin runtime.

The refresh also runs `tools/generate-builtin-fonts.py` with Python 3 to decompress
libXfont2 2.0.8's pinned built-in arrays into committed, uncompressed PCF C data.
Python and gzip/zlib are not normal build or runtime dependencies.
