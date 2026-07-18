# Building Xmin

## Requirements

The product build requires CMake 3.21 or newer, a C11 compiler, a C++17
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

Host XCB, xkbcommon-x11, Qt, GTK, and Xlib are test-only discoveries. Missing
test packages reduce the applicable acceptance set but do not affect the
product graph. `XMIN_BUILD_QT_CLIENT` instead builds Xmin's own narrow client
ABI and does not discover or link those host X libraries.

## Presets

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Four checked-in presets cover normal workflows:

| Preset | Purpose |
| --- | --- |
| `dev` | Debug build, tests, optional client GL, compilation database. |
| `release` | Optimized full package with tests. |
| `minimal` | Optimized server package with client GL disabled. |
| `sanitizer` | Debug ASan/UBSan, warnings as errors, client GL disabled. |

Use `--fresh` when validating source deletion or platform detection:

```sh
cmake --preset minimal --fresh
```

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `XMIN_BUILD_CLIENT_GL` | `ON` | Build the independent software-direct `libGL`. |
| `XMIN_BUILD_QT_CLIENT` | `OFF` | Build and install the Xmin-native C++17 XCB/xkbcommon client SDK used by patched Qt qxcb. |
| `XMIN_BUILD_LAUNCHER` | `ON` | Build authenticated process supervisor `xmin-run`. |
| `XMIN_BUILD_TESTS` | top-level `ON` | Build the self-tests and independent client gates. |
| `XMIN_REQUIRE_TOOLKIT_TESTS` | `OFF` | Require Qt 5/6 and GTK 3 acceptance targets. |
| `XMIN_ENABLE_INSTALL` | top-level `ON` | Generate install and relocation rules. |
| `XMIN_WARNINGS_AS_ERRORS` | `OFF` | Promote project-owned warnings to errors. |
| `XMIN_ENABLE_SANITIZERS` | `OFF` | Enable ASan and UBSan with Clang/GCC. |
| `XMIN_ENABLE_MITSHM` | `AUTO` | `AUTO`, `ON`, or `OFF` for SysV MIT-SHM. |
| `XMIN_DEFAULT_WIDTH` | `1280` | Default root width. |
| `XMIN_DEFAULT_HEIGHT` | `1024` | Default root height. |

The root depth is deliberately fixed at 24. TCP, indirect GLX, alternate DDX
drivers, and runtime extension selection are not build options.

## Running

Prefer the launcher for normal work:

```sh
./build/dev/src/launcher/xmin-run \
  --server ./build/dev/src/server/Xmin \
  --screen 1280x1024x24 -- your-program arguments...
```

Run the server directly only when managing authentication yourself:

```sh
./build/dev/src/server/Xmin :99 --auth /path/to/Xauthority \
  --screen 1280x1024 --max-clients 64
```

`--no-auth` is an explicit unsafe test mode. `--client-fd` and `--display-fd`
support controlled embedding and launcher coordination. `--runtime-root`
exists for isolated tests that must redirect `/tmp/.X11-unix` and lock files.

`xminctl --help` lists window, input, synchronization, and PPM capture
commands. It accepts `--display :N`; otherwise it uses `DISPLAY` and
`XAUTHORITY`.

## Installation and relocation

```sh
cmake --install build/release --prefix /opt/xmin
/opt/xmin/bin/xmin-run --server /opt/xmin/bin/Xmin -- your-program
```

The package installs `Xmin`, `xmin-run`, `xminctl`, documentation, and—when
enabled—`lib/xmin/libGL` plus its GL/GLX/OSMesa headers. Applications opt into
the companion GL DSO explicitly; it is not a server dependency. Enabling
`XMIN_BUILD_QT_CLIENT` also installs `Xmin::QtX11`, its standard ABI headers,
and the XCB bridge header for `Xmin::GL`. See `patches/qt/README.md` for the
validated Qt configuration.

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
```

Pixman uses `tools/pixman-files.txt` as an exact allowlist. OSMesa regenerates
its CMake source list from the pinned upstream revision, which already contains
the neutral separate draw/read-buffer API. Preserve
`third_party/osmesa/src/drivers/osmesa/xmin_osmesa_adapter.c`; it is
project-owned and confines Mesa-private types to the dependency boundary.

Protocol and font regeneration commands are in `tools/README.md`. Generated
files are reviewed and committed. No removed Xorg-server, xtrans, Xau, XCB,
Xfont, or XKB implementation import workflow remains.
