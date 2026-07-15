# Xmin: minimal self-contained X11 server design

Status: initial architecture and implementation inventory, July 2026.

## 1. Objective

This project will provide a portable, CPU-only X11 server for automated graphical
application testing.  A release must run on a machine with no installed X server,
font stack, OpenGL implementation, device drivers, or graphics hardware.

The normal release is a relocatable package containing:

1. `Xmin`, with the X11 server, modern server extensions, software 2D renderer,
   fonts, keyboard map, and authentication support compiled in; and
2. if OpenGL 2.0 support is enabled, a companion client-side GLX software bridge
   using the project's OSMesa implementation.

The second artifact is necessary because direct OpenGL calls execute in the client
process.  It is still part of this project and requires no system installation.

The server does **not** need `libX11`, `libxcb`, or the client libraries named after
X extensions.  Applications already use those libraries to encode X11 requests.
This project must instead compile the corresponding **server-side protocol
implementations** into `Xmin`.

### Goals

- X11 wire compatibility, including opposite-endian clients.
- Enough modern extensions for Qt and comparable X11 toolkits.
- Software-only core, RENDER, and GL rendering.
- No runtime discovery or loading of host Xorg, Mesa, font, or input modules.
- An offline CMake build from the checked-out source tree.
- C11 for the X server and C++17 where OSMesa requires C++.
- Upstream-derived code retained in recognizable modules so security fixes can be
  imported.
- POSIX systems first: Linux/glibc, Linux/musl, the BSDs, and macOS.  A Windows
  transport can be a later platform port.

### Non-goals

- Physical displays, GPUs, DRM devices, hardware acceleration, or hot-plug input.
- The Xorg DDX/module/driver framework, `xorg.conf`, udev, logind, DBus, or PCI.
- A desktop environment, compositor, display manager, or window manager.
- Server-side TrueType/OpenType discovery and rasterization.
- XDMCP, remote display management, or TCP listening by default.
- Vulkan, EGL, OpenGL ES, or OpenGL newer than 2.0.
- Exact behavioral equivalence to a hardware Xorg server where a feature is outside
  the declared protocol profile.

## 2. Runtime architecture

```text
 application and its X11 client libraries
       |                         |
       | X11 / extension wire    | optional GL/GLX calls
       v                         v
 Unix-domain socket       bundled GLX + OSMesa bridge
       |                         |
       +------------+------------+
                    v
  +----------------------------------------------------+
  | Xmin process                                       |
  |                                                    |
  | transport/auth -> DIX resource & event core        |
  |                         |                           |
  |        protocol extensions and virtual input       |
  |                         |                           |
  |          MI/fb + pixman software rendering         |
  |                         |                           |
  |          vfb DDX -> in-memory framebuffer          |
  +----------------------------------------------------+
```

The DIX layer owns clients, resources, windows, drawables, properties, selections,
events, grabs, and core protocol dispatch.  The MI and `fb` layers implement
device-independent drawing over a linear memory framebuffer, with pixman providing
regions and compositing.  The vfb DDX creates screens and virtual input devices and
connects them to those layers.

The initial screen is one connected output, one CRTC, and one mode selected at
startup, defaulting to `1280x1024x24`.  Depth 1 pixmaps and a 24-bit TrueColor visual
stored in 32 bits per pixel are mandatory.  The existing vfb 8, 15, 16, and 30-bit
startup modes can remain if they add little code.  Multiple X screens are useful but
secondary to making screen 0 reliable.

RANDR must accurately report the virtual output.  Dynamic resize should not be
advertised beyond what the framebuffer implementation safely supports.  The
upstream vfb path changes screen dimensions without fully reallocating all backing
state, so launch-time sizing is the first milestone; safe resize is a separate task.

## 3. Extension profile

Qt's XCB platform uses RENDER, SHAPE, RANDR, XFIXES, XKB, SYNC, and MIT-SHM client
bindings.  The client libraries are not server dependencies, but their protocols
must be implemented or the client must have a valid fallback.

### Included in the normal server

| Facility | Reason and required behavior |
| --- | --- |
| Core X11 | All core requests, resources, events, visuals, colormaps, and byte swapping. |
| BIG-REQUESTS | Common client libraries use it for requests larger than the core limit. |
| XC-MISC | XID allocation compatibility used inside client libraries. |
| Generic Event | Foundation for XInput2 events. |
| RENDER | Alpha compositing, glyph upload, transforms, and modern 2D drawing. |
| RANDR | At least one connected output/CRTC/mode with correct geometry and DPI. |
| XFIXES | Regions, cursor notifications, selection notifications, and toolkit compatibility. |
| SHAPE | Non-rectangular input and bounding regions. |
| SYNC | Counters, alarms, and fences that do not depend on DRM. |
| XKB | A complete, queryable US PC105 map and normal XKB controls/events. |
| XInput 2 | Virtual core pointer/keyboard hierarchy and query/event compatibility. |
| XTEST | Essential for automated pointer and keyboard injection. |
| MIT-SHM | Local fast image transfer when the platform supplies SysV shared memory; otherwise omit the advertised extension and test client fallback. |
| COMPOSITE | Inexpensive compatibility for clients expecting redirected drawable semantics. |
| DAMAGE | Required companion to COMPOSITE and useful for tracking changed regions. |
| DOUBLE-BUFFER | Small compatibility extension for older applications. |
| Present | Use the X server's software/fake-vblank path; useful for presentation semantics without a GPU. |
| XINERAMA | Legacy screen-layout queries, implemented from the single RANDR layout. |
| MIT-SCREEN-SAVER | Small and commonly queried; no physical power-management behavior. |
| GLX | Included only when its OSMesa path passes the acceptance tests in section 10. |

COMPOSITE, DAMAGE, DOUBLE-BUFFER, Present, XINERAMA, and MIT-SCREEN-SAVER are not all
strictly necessary for a basic Qt Widgets process, but their server implementations
are modest and prevent otherwise avoidable compatibility failures.

### Deliberately excluded

- DRI1, DRI2, DRI3, DRM, GBM, EGL, glamor, and hardware Present paths.
- XVideo and XvMC.
- DGA, VidMode, DPMS, PCI/VGA/int10, and other physical-device facilities.
- SELinux/XACE policy integration, SECURITY, X-Resource, RECORD, and XFree86-Bigfont.
- XDMCP, XDM-AUTH-1, SUN-DES-1/secure RPC, and remote font servers.

Compile-time feature switches are useful during development, but the published
extension list must be a tested profile.  It is better not to advertise an extension
than to return fabricated success for behavior the server does not implement.

## 4. Rendering and fonts

### 2D

Use the upstream `fb` renderer and MI fallbacks.  Vendor pixman and compile its
portable C implementation into the server.  Architecture-specific pixman SIMD is an
optional optimization; it should be enabled only where CMake can build and test it.
No cairo dependency is needed.

RENDER does not require server-side font discovery.  Modern applications select and
rasterize fonts in their own process and upload glyphs to the server.  Therefore the
server does not need Fontconfig, FreeType, `struetype`, or system font directories.

The core X11 font requests still need a valid implementation.  Retain a narrow
subset of libXfont2 containing its embedded `6x13`/`fixed` and `cursor` PCF fonts,
font-path-element machinery, and PCF reader.  Do not include FreeType, BDF/SNF,
external font directories, font servers, `fontenc`, or compression libraries.
Regenerate the two embedded PCF resources uncompressed and read them directly from
memory; this removes libXfont2's otherwise mandatory zlib dependency.

### OpenGL and the DRI boundary

The renderer is [starseeker/osmesa](https://github.com/starseeker/osmesa), statically
built with symbol mangling so it cannot collide with a host OpenGL library.  It
provides CPU rendering through OpenGL 2.0 and has no graphics-system dependency.

There are two distinct GL paths:

1. **Server-side indirect GLX.**  A new Xorg GLX provider can map GLX contexts and
   drawables to OSMesa contexts and buffers.  This supports ordinary GLX context,
   FBConfig, pixmap/pbuffer, and swap behavior in one executable.  It is suitable for
   legacy fixed-function GL.  Xorg's indirect GLX command protocol does not carry the
   complete OpenGL 2.0/GLSL API, so the renderer's internal version does not make
   arbitrary OpenGL 2.0 clients work through this path.
2. **Bundled software-direct GLX.**  To support unmodified OpenGL 2.0 applications,
   GL calls must execute in the client process.  Build a companion GLX/GL dispatch
   library that adapts GLX contexts and drawables to OSMesa and presents the CPU
   color buffer with MIT-SHM, falling back to core image upload.  This is analogous
   to Mesa's software DRI path, but is shipped and versioned with this project.

The second path is the preferred OpenGL 2.0 design.  It does not require Xorg's
DRI2/DRI3 extensions or a DRM device.  Enabling those server extensions alone would
not help: normal client loaders expect DRM/GEM objects and a version-matched Mesa DRI
driver, while this OSMesa project is a renderer rather than such a driver.

The feasibility implementation must decide whether the companion is a GLVND vendor
module, a complete relocatable `libGL`/`libGLX` pair, or both.  The choice must be
made by testing the actual Qt and application client libraries; it must not rely on
the unstable internal ABI of an arbitrary host Mesa installation.

The GLX/OSMesa provider needs:

- GLX 1.4 visual and FBConfig descriptions matching the X framebuffer;
- single- and double-buffered RGBA8 configurations, with supported depth/stencil
  sizes reported exactly;
- context creation, destruction, sharing, make-current, lose-current, and error
  handling;
- window, GLXPixmap, and pbuffer drawable storage and resize/lifetime hooks;
- OSMesa row orientation and channel layout matched to the X `fb` layout;
- swap/copy into the X drawable followed by DAMAGE notification;
- no multisample, stereo, texture-from-pixmap, or create-context profile extension
  unless the implementation genuinely supplies it.

## 5. Input and XKB

Create only a virtual core pointer and virtual core keyboard, using the vfb DDX input
procedures.  There is no OS device enumeration or input driver.  XTEST supplies test
events, while normal pointer warps and XSendEvent behavior remain available.

Runtime `xkbcomp` and a full `xkeyboard-config` installation are too large for the
goal.  During source maintenance, generate a complete US PC105/evdev map with a
pinned xkbcomp and xkeyboard-config release using xkbcomp's `-C` output.  Commit the
generated arrays and a small adapter that constructs the initial `XkbDescRec`.
Keep the X server's XKB protocol, state, controls, actions, and event code, but remove
the runtime rules parser, subprocess invocation, XKM temporary-file path, and
libxkbfile dependency where link analysis permits.

The initial product supports one documented layout (`us`).  Additional layouts can
later be generated as separate embedded maps selected by a command-line option;
shipping the complete keyboard database is not required for protocol compliance.

## 6. Transport, authentication, and process behavior

- Use xtrans's local/Unix transport subset for `DISPLAY=:N` compatibility.
- Create the normal display lock and Unix socket, detecting stale locks safely.
- Disable TCP at build and runtime by default.  A loopback-only TCP option may be
  added for platforms without Unix-domain X sockets, with explicit authentication.
- Keep MIT-MAGIC-COOKIE-1 and the `-auth` file interface.  Internalize the small
  libXau record-reader subset instead of linking libXau.
- Retain `-ac` for isolated test containers, but do not silently make a network
  listener unauthenticated.
- Use the OS cryptographic random source when a convenience launcher generates a
  cookie.  Do not substitute a predictable PRNG.
- Preserve the familiar Xvfb options: display number, `-screen`, `-pixdepths`,
  `-dpi`, `-fbdir`, `-shmem`, extension controls, and signal-driven shutdown.
- Add an optional launcher mode that chooses a free display, creates a private
  authority file, sets `DISPLAY` and `XAUTHORITY`, runs a child, and cleans up.  This
  removes the runtime dependency on the shell `xvfb-run` and `xauth` utilities.

The server is single-threaded except for code internal to OSMesa.  Disable the Xorg
input thread because there is no asynchronous hardware input source.

## 7. Source inventory

Candidate upstream pins for the first import are listed here.  Every import must be
recorded with an exact tag/commit, archive checksum, license, retained paths, and
local patch list in a machine-readable manifest.

| Source | Candidate pin | Retained purpose |
| --- | --- | --- |
| [xorg-server](https://www.x.org/releases/individual/xserver/) | 21.1.23 | X server core and vfb implementation. |
| [xorgproto](https://www.x.org/releases/individual/proto/) | 2024.1 | X11 and extension wire headers. |
| [pixman](https://www.cairographics.org/releases/) | 0.46.2 | Regions and CPU compositing. |
| [xtrans](https://www.x.org/releases/individual/lib/) | 1.6.0 | Local X socket transport. |
| [libXfont2](https://www.x.org/releases/individual/lib/) | 2.0.8 | Minimal built-in core fonts only. |
| [libXau](https://www.x.org/releases/individual/lib/) | 1.0.12 | Small authority-file parser subset only. |
| [libxkbfile](https://www.x.org/releases/individual/lib/) | 1.2.0 | Headers/code only if still needed after embedded-map refactoring. |
| [xkeyboard-config](https://www.x.org/releases/individual/data/xkeyboard-config/) | 2.47 | Source-maintenance input used to generate the embedded US map, not a runtime component. |
| [starseeker/osmesa](https://github.com/starseeker/osmesa) | `986a9ce0a4fa9a0ee3a79c821b293aa47d6cab6c` | Static OpenGL 2.0 software renderer. |

### xorg-server directories and files to retain

| Area | Content |
| --- | --- |
| `dix/` | Atom, resource, client dispatch, windows, pixmaps, GCs, properties, selections, events, grabs, devices, fonts, byte-swapped requests/replies, and server lifecycle.  Start with the whole DIX source list and trim only with tests. |
| `os/` | Wait/poll loop, clients, connections, I/O, local access control, MIT auth, color names, logging, allocation/portability helpers, and xtrans glue.  Exclude XDMCP, secure RPC, dynamic modules, and platform backtraces unless needed. |
| `mi/` | Device-independent drawing, exposure, sprite/cursor, pointer, colormap, window, and extension initialization. |
| `fb/` | Linear framebuffer implementation and RENDER picture hooks. |
| `miext/damage/` | Internal damage tracking. |
| `miext/sync/` | Software synchronization; exclude shared DRM fence code. |
| `render/` | RENDER protocol and software implementation. |
| `randr/` | RANDR protocol, output/CRTC/mode/property state, and XINERAMA adapter. |
| `xfixes/`, `damageext/`, `composite/`, `dbe/`, `present/` | The named compatibility extensions and fake-vblank Present path. |
| `Xext/` | BIG-REQUESTS, Generic Event, SHAPE, SYNC, XC-MISC, XTEST, MIT-SHM, screen saver, and XINERAMA support selected explicitly. |
| `Xi/` | XInput 1/2 protocol and virtual device hierarchy; use the vfb stubs instead of loadable drivers. |
| `xkb/` | XKB protocol/state plus allocation helpers and vfb stubs, modified to initialize the embedded map. |
| `hw/vfb/` | `InitInput.c`, `InitOutput.c`, and the Xvfb manual, with local fixes for RANDR, resource cleanup, and embedded defaults. |
| `glx/` | GLX protocol dispatch, context/drawable core, generated request code, and a new OSMesa provider.  Exclude DRI/DRM providers and GLVND dynamic vendor loading from the server where possible. |
| `include/` | Required private server headers plus CMake-generated `dix-config.h`, version, XKB, and platform configuration headers. |

### Project-owned code

- Top-level and component CMake files; feature probes and generated config headers.
- Upstream import manifest, license aggregation, and patch records.
- Portable libc fallbacks (`reallocarray`, `strlcpy`, `strlcat`,
  `timingsafe_memcmp`, and related functions selected by CMake probes).
- Embedded XKB map generator input, committed output, and XKB initialization adapter.
- Uncompressed built-in font resources and minimal memory reader.
- Authority/cookie convenience code and optional child launcher.
- OSMesa-backed GLX server provider.
- The client-side GLX/OSMesa software-direct bridge needed for OpenGL 2.0.
- Protocol, integration, application, portability, and security tests.

### Build-only and test-only tools

CMake and a C/C++ compiler are the only normal build prerequisites.  Source refresh
may use xkbcomp and scripts, but their generated results are committed.  Xlib/XCB,
`xdpyinfo`, `xrandr`, Qt, and GL diagnostic programs are test dependencies only and
must never leak into the installed server's link interface.

## 8. Proposed repository layout

```text
CMakeLists.txt
cmake/                    feature probes and config templates
src/
  dix/ fb/ mi/ os/        imported X server core
  extensions/             selected imported extension directories
  hw/vfb/                 vfb DDX and local adaptations
  glx/                    GLX core and OSMesa provider
  support/                auth, portability, launcher
include/                  private and generated server headers
data/
  fonts/                  fixed/cursor generated C resources
  xkb/                    source recipe and generated US map
third_party/
  xorgproto/ pixman/ xtrans/ xfont2/ osmesa/
tools/                    audited upstream refresh/generation helpers
tests/
  unit/ protocol/ integration/ qt/ glx/
UPSTREAM.toml             pins, checksums, licenses, retained paths, patches
LICENSES/                 complete third-party notices
```

Keep imported files close to their upstream layout.  Put behavioral adaptations in
small project-owned files or clearly recorded patches instead of rewriting the core.

## 9. CMake design

Build component object/static libraries and link them into `Xmin`; do not install
them as public libraries.  The normal build performs no `find_package(X11)`,
pkg-config lookup, or implicit host Mesa/font lookup.

Suggested controls:

- `XMIN_BUILD_GLX=ON|OFF` -- OSMesa server GLX and companion bridge.
- `XMIN_ENABLE_MITSHM=AUTO|ON|OFF` -- based on genuine platform support.
- `XMIN_ENABLE_TCP=OFF` -- explicit opt-in only.
- `XMIN_PIXMAN_SIMD=AUTO|ON|OFF` -- generic C is always available.
- `XMIN_BUILD_LAUNCHER=ON` -- integrated authenticated child runner.
- `XMIN_BUILD_TESTS=ON|OFF` -- may discover external test clients/toolkits.

CMake must probe headers, functions, endianness, type sizes, Unix sockets, peer
credentials, polling APIs, shared memory, secure randomness, and thread support.
Generate a single authoritative configuration header rather than carrying results
from an upstream Meson or Autoconf build.

The final POSIX executable should normally depend only on the C/C++ runtimes,
`libm`, and the platform thread library.  Verify this in CI with `ldd`/`otool` and
by running in a container without X11, Mesa, Fontconfig, or FreeType packages.

## 10. Verification and acceptance criteria

### Protocol and 2D

- Connect with Xlib and XCB clients, including an emulated opposite-endian client.
- Exercise every core request family and malformed length/error cases.
- Verify the exact extension list and versions with `xdpyinfo`.
- Exercise RENDER alpha, transforms, trapezoids, glyph upload, and readback against
  reference images.
- Query RANDR output/CRTC/mode/property state and test only supported resizing.
- Exercise SHAPE, XFIXES regions/selections/cursors, SYNC, COMPOSITE, DAMAGE, DBE,
  Present, and MIT-SHM plus its forced fallback path.
- Query and mutate XKB state; inject US key and pointer sequences with XTEST.
- Open/list/render the embedded `fixed` and `cursor` fonts.
- Verify MIT-MAGIC-COOKIE-1 success and rejection, local-only defaults, stale-lock
  handling, signal cleanup, and concurrent display allocation.

### Toolkit applications

- Qt 5 and Qt 6 `QGuiApplication` startup.
- QWidget painting, clipboard/selections, cursor, dialogs, backing-store resize,
  high-DPI query, and deterministic screenshot comparison.
- A representative GTK 3 application and selected project applications.
- Tests that need window-manager focus/reparenting semantics must launch a test WM;
  that behavior is not supplied by the X server itself.

### OpenGL gate

Test with the client libraries the target applications actually deploy:

- GLX version/visual/FBConfig queries and context error handling.
- Single/double-buffered window, GLXPixmap, and pbuffer rendering.
- Swap, resize, context sharing, readback, and X/GL ordering.
- A fixed-function triangle through server-side indirect GLX.
- An OpenGL 2.0 context running a GLSL 1.10 vertex and fragment shader through the
  bundled software-direct bridge.
- Qt `QOpenGLContext` requesting 2.0 and a minimal Qt Quick scene if Qt Quick is in
  scope.

Do not advertise or document OpenGL 2.0 until the GLSL test works with unmodified
target clients.  A successful `glGetString(GL_VERSION)` by itself is insufficient.

### Portability and hardening

- GCC and Clang warning-clean C11/C++17 builds.
- ASan, UBSan, and fuzzing for untrusted core/extension/GLX request lengths.
- Linux glibc and musl first, then FreeBSD/OpenBSD/NetBSD and macOS.
- Little- and big-endian protocol tests under emulation.
- Repeated startup/shutdown and client churn under leak/resource diagnostics.
- Offline reproducible builds and a complete license/source provenance report.

## 11. Implementation sequence

1. **Feasibility and reference baseline**
   - Pin sources and establish the upstream Xvfb behavior as a reference.
   - Build the selected X server modules with CMake and vendored xorgproto/pixman.
   - Prove Qt raster startup and the required extension list.
   - In parallel with architectural trimming, prototype the OSMesa GLX provider and
     the client-side OpenGL 2.0 path.  Decide the GLVND/full-libGL packaging model
     before claiming GL 2.0.
2. **Self-contained modern 2D server**
   - Vendor the local xtrans subset.
   - Embed fonts and XKB; remove zlib, FreeType, Fontconfig, xkbcomp,
     xkeyboard-config, libxkbfile, and libXau runtime requirements.
   - Implement auth, cleanup, local socket defaults, and the optional launcher.
   - Pass core, extension, Qt, and screenshot tests in an empty container.
3. **OpenGL 2.0 package**
   - Finish the server OSMesa GLX provider for legacy indirect clients.
   - Finish and package the software-direct companion bridge.
   - Pass fixed-function, GLSL 1.10, Qt OpenGL, and target-application tests.
4. **Portability, trimming, and release hardening**
   - Add platform backends and libc fallbacks.
   - Remove unreachable code based on link maps and coverage, never merely by file
     name.
   - Fuzz protocol surfaces, run sanitizers, audit licenses, and document supported
     limitations.

## 12. Principal risks and decisions

1. **OpenGL client integration is the largest risk.**  OSMesa solves rendering but
   not the GLX client ABI or the missing OpenGL 2.0 indirect wire commands.  Resolve
   this with an early end-to-end target-client prototype.
2. **Security fixes must remain importable.**  X servers parse untrusted binary
   protocols.  Pinning and trimming do not remove the need to track xorg-server,
   pixman, font, and GLX advisories.
3. **RANDR resize must match storage.**  A query-only fixed launch mode is preferable
   to unsafe or false resize support.
4. **MIT-SHM is platform-specific.**  Its absence must be represented by omitting the
   extension, with client fallback tested.
5. **One embedded keyboard layout is intentionally limited.**  It is sufficient for
   deterministic test input, but users needing international input require generated
   additional maps.
6. **A window manager is separate.**  Rendering tests generally do not need one, but
   focus, decorations, and some modal behavior do.  Keep a test WM outside the server
   rather than confusing server and ICCCM responsibilities.
7. **Application dependencies remain application dependencies.**  A Qt process may
   still need its own fonts, platform plugin, image plugins, or DBus services.  This
   project guarantees the X display service, not the application's entire runtime.

The first releasable milestone is the self-contained modern 2D server.  The complete
project target additionally includes the verified OpenGL 2.0 companion path; the 2D
milestone gives that work a stable X11 and drawable foundation without making an
unsupported GL claim.
