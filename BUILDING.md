# Building Xmin

## Requirements

The core product build requires CMake 3.21 or newer, a C11 compiler, a C++17
compiler, Unix-domain sockets, and the standard C/C++ and math libraries.
Optional client OpenGL additionally needs the platform thread backend selected
by CMake. All product sources, pixman, protocol inputs, fonts, and optional
OSMesa sources are in the repository; configure and build are offline.

The tested primary hosts are Linux and FreeBSD. The platform layer uses POSIX
sockets, `poll`, signals, process APIs, and optional SysV shared memory. CMake
checks the small feature set actually used instead of emulating Xorg's
portability configuration.

Windows is a planned platform rather than a current supported target. The
single-threaded pixman build uses generic C and needs no pthread or Win32 TLS;
the optional GL bridge uses C++17 synchronization and OSMesa selects its
existing Win32 thread backend. A Windows server port must supply the narrow
platform layer with Winsock local transport, poll/wakeup, process supervision,
secure temporary state, and a non-SysV SHM policy; protocol, server state,
controller codec, and raster semantics should not require redesign.
The client GL bridge does not require shared memory: where descriptor passing,
file mappings, MIT-SHM shared pixmaps, or Present are unavailable (including an
initial Windows port), it retains its heap buffer and `PutImage`/`XPutImage`
presentation path.

The optional Unix viewer uses the host window system through GLFW and requires
host OpenGL, XCB, and XTEST. XCB-SHM enables shared capture, and XCB-DAMAGE
enables disjoint partial-frame redraws; both retain portable fallbacks when
unavailable.
The optional bundled shell build also requires Autoconf, Automake, and Make.
Host xkbcommon-x11, Qt, GTK, and Xlib are otherwise test-only discoveries.
Missing test packages reduce the applicable acceptance set but do not affect
the core product graph. `XMIN_BUILD_QT_CLIENT` and
`XMIN_BUILD_TOOLKIT_CLIENT` build Xmin's own narrow client ABIs and do not
discover or link those host X libraries.

## Presets

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

All build-tree executables, including test tools, are written to
`build/dev/bin`; shared and static libraries and archives are written to
`build/dev/lib`. The same `bin`/`lib` layout is used for other build trees and
multi-configuration generators.

Four checked-in presets cover normal workflows:

| Preset | Purpose |
| --- | --- |
| `dev` | Debug build, tests, optional client GL, compilation database. |
| `release` | Optimized full package with tests. |
| `minimal` | Optimized server package with client GL, viewer, and desktop disabled. |
| `sanitizer` | Debug core ASan/UBSan gate with warnings as errors and optional applications disabled. |

Use `--fresh` when validating source deletion or platform detection:

```sh
cmake --preset minimal --fresh
```

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `XMIN_BUILD_CLIENT_GL` | `ON` | Build the independent software-direct `libGL`. |
| `XMIN_BUILD_QT_CLIENT` | `OFF` | Build and install the Xmin-native C++17 XCB/xkbcommon client SDK used by patched Qt qxcb. |
| `XMIN_BUILD_TOOLKIT_CLIENT` | `OFF` | Add the focused Xlib/Xft/Fontconfig facade and embedded Go fonts used by patched FLTK and Tk. |
| `XMIN_BUILD_LAUNCHER` | `ON` | Build authenticated process supervisor `xmin-run`. |
| `XMIN_BUILD_VIEWER` | Unix except macOS: `ON` | Build the separate GLFW host viewer and guest-input bridge. |
| `XMIN_BUILD_DESKTOP` | Unix except macOS: `ON` | Build the JWM/st desktop and `xmin-session` supervisor. |
| `XMIN_BUILD_BUNDLED_SHELL` | Unix: `ON` | Build the imported dash sources as `xmin-sh` for desktop sessions. |
| `XMIN_BUILD_TESTS` | top-level `ON` | Build the self-tests and independent client gates. |
| `XMIN_REQUIRE_TOOLKIT_TESTS` | `OFF` | Require Qt 5/6 and GTK 3 acceptance targets. |
| `XMIN_ENABLE_INSTALL` | top-level `ON` | Generate install and relocation rules. |
| `XMIN_WARNINGS_AS_ERRORS` | `OFF` | Promote project-owned warnings to errors. |
| `XMIN_ENABLE_SANITIZERS` | `OFF` | Enable ASan and UBSan with Clang/GCC. |
| `XMIN_ENABLE_MITSHM` | `AUTO` | `AUTO`, `ON`, or `OFF` for SysV MIT-SHM. |
| `XMIN_DEFAULT_WIDTH` | `1280` | Default root width. |
| `XMIN_DEFAULT_HEIGHT` | `1024` | Default root height. |

The root depth is deliberately fixed at 24. The server provides the GLX 1.4
context, FBConfig, drawable, and query control plane for direct contexts; GLX
indirect rendering commands are explicitly rejected. TCP, indirect GLX,
alternate DDX drivers, and runtime extension selection are not build options.

## Running

Prefer the launcher for normal work:

```sh
./build/dev/bin/xmin-run \
  --server ./build/dev/bin/Xmin \
  --screen 1280x1024x24 -- your-program arguments...
```

Run the server directly only when managing authentication yourself:

```sh
./build/dev/bin/Xmin :99 --auth /path/to/Xauthority \
  --screen 1280x1024 --max-clients 64
```

`--no-auth` is an explicit unsafe test mode. `--client-fd` and `--display-fd`
support controlled embedding and launcher coordination. `--runtime-root`
exists for isolated tests that must redirect `/tmp/.X11-unix` and lock files.

`xminctl --help` lists window, input, synchronization, and PPM capture
commands. It accepts `--display :N`; otherwise it uses `DISPLAY` and
`XAUTHORITY`.

For a reproducible full-desktop lifecycle workload, run both the shared-memory
and portable capture paths:

```sh
xvfb-run -a tests/desktop_stress.sh --build-dir .build --seed 1
xvfb-run -a tests/desktop_stress.sh --build-dir .build --seed 1 --no-shm
```

This stresses guest geometry, map/unmap and stacking state, the GLFW host
window, terminal creation/destruction, viewer detach/reattach, final input,
and capture. `--resize-only` retains the narrower original workload; see
`tests/desktop_stress.sh --help` for individual iteration controls.

See `tests/README.md` for the layered test design and the policy for translating
selected XTS and rendercheck behavior into focused native regressions.

Launch the complete optional interactive desktop and its GLFW host viewer with:

```sh
./launch.sh
```

The script selects `.build` or `build/dev`, waits for the private session
descriptor, attaches the viewer, and stops the session when the viewer exits.
Use `./launch.sh --help` for build-directory, geometry, capture-rate, and
portable no-SHM overrides.

The session and viewer remain separate executables. For a persistent session
that permits viewer detach and reattach, run these commands in separate
terminals:

```sh
./build/dev/bin/xmin-session \
  --session-info /tmp/xmin-session.info
./build/dev/bin/xmin-viewer \
  --session-info /tmp/xmin-session.info
```

`xmin-session` starts Xmin and JWM, selects the sibling `xmin-sh` when present,
and keeps st available through the JWM root menu without opening a terminal
automatically. `xmin-viewer` reads the descriptor without owning the session,
so it can detach and reattach. It preserves disjoint DAMAGE rectangles and
uses MIT-SHM for partial capture when possible; `--no-shm` selects the bounded
tiled `GetImage` fallback explicitly. The default root and viewer letterbox
color is `#20252b`.

JWM gives terminal windows a six-pixel graphical resize border, and its title
bar menu includes a Resize action. In `xmin-st`, `Ctrl+Shift++` increases the
font size, `Ctrl+Shift+-` decreases it, and `Ctrl+Shift+0` restores the default;
`Ctrl` plus the mouse wheel also adjusts the size.

## Installation and relocation

```sh
cmake --install build/release --prefix /opt/xmin
/opt/xmin/bin/xmin-run --server /opt/xmin/bin/Xmin -- your-program
```

The package installs `Xmin`, `xmin-run`, `xminctl`, documentation, and—when
enabled—the viewer, desktop applications, `xmin-sh`, and `lib/xmin/libGL`
plus its GL/GLX/OSMesa headers. Applications opt into
the companion GL DSO explicitly; it is not a server dependency. Enabling
`XMIN_BUILD_QT_CLIENT` also installs `Xmin::QtX11`, its standard ABI headers,
the XCB bridge header for `Xmin::GL`, and the pinned Go font files exposed by
the package as `Xmin_FONT_FILES`. The Qt integration deploys those files into
Qt's normal no-fontconfig font directory. See `patches/qt/README.md` for the
validated Qt configuration.

On Unix-like hosts with `SCM_RIGHTS`, the XCB client bridge lets `Xmin::GL`
allocate an MIT-SHM segment, render OSMesa directly into a shared pixmap, and
submit that pixmap through Present. Xmin copies the completed pixels once into
the server-owned window surface before acknowledging the checked Present
request, so the client never owns live window storage. If any capability or
request is unavailable, the same GL implementation automatically falls back
to its portable heap buffer and image upload.

`XMIN_BUILD_TOOLKIT_CLIENT` installs `Xmin::ToolkitX11`, focused X11, Xft,
and Fontconfig public headers, and `libXminClient`. The client embeds Go Sans
and Go Mono regular, bold, italic, and bold-italic faces and resolves requests
only among those faces—there is no host font-directory search. Toolkit
integration is explicitly opt-in; see `patches/fltk/README.md` and
`patches/tk/README.md` for validated releases and build options.

Every test-enabled build contains `xmin.install-relocation-dependencies`. It
installs into a disposable arbitrary prefix, runs the installed launcher and
server with an authenticated client, checks development files, and uses
`ldd` or `otool` to reject host X11, XCB, Xau, font, GL/EGL, crypto, SSL, and
DRM dependencies.

## Metrics

```sh
cmake --build --preset minimal --target xmin-modernization-metrics
cat build/minimal/modernization-metrics.txt
```

The report separates project, server, pixman, and OSMesa source counts; for
OSMesa it also distinguishes the compiled C implementation from the complete
C/header maintenance surface. Build translation units, optimized server size,
and protocol coverage are separate measures. It is a regression alarm, not a
code-golf target.

## Dependency and generated-source refresh

Exact revisions, archive hashes, retained paths, licenses, patches, and tools
are in `UPSTREAM.toml`.

```sh
tools/import-pixman.sh /path/to/pixman-0.46.2
tools/import-osmesa.sh /path/to/osmesa-13b14a95
tools/import-dash.sh /path/to/dash-v0.5.13.4-checkout
```

Pixman uses `tools/pixman-files.txt` as an exact allowlist. OSMesa regenerates
its CMake source list from the pinned upstream revision, which already contains
the neutral separate draw/read-buffer API. Preserve
`third_party/osmesa/src/drivers/osmesa/xmin_osmesa_adapter.c`; it is
project-owned and confines Mesa-private types to the dependency boundary.

Protocol and font import/regeneration commands are in `tools/README.md`. Generated
files are reviewed and committed. No removed Xorg-server, xtrans, Xau, XCB,
Xfont, or XKB implementation import workflow remains.
