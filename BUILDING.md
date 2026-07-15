# Building Xmin

Xmin currently builds a deliberately non-functional server stub. The scaffold
establishes the target names, component boundaries, generated configuration,
platform probes, tests, and install rules that the real server will use.

## Requirements

- CMake 3.21 or newer;
- a C11 compiler;
- a C++17 compiler when `XMIN_BUILD_GLX=ON`; and
- a platform thread implementation supported by CMake.

No X11, Mesa, font, or input development packages are used by the normal build.

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

Running the stub without `--help` or `--version` intentionally fails. This keeps a
test harness from mistaking the placeholder for a functioning X display server.

## Build controls

| Option | Default | Purpose |
| --- | --- | --- |
| `XMIN_BUILD_GLX` | `ON` | Prepare the OSMesa GLX/server-client component boundary and enable C++17. |
| `XMIN_ENABLE_MITSHM` | `AUTO` | Enable MIT-SHM only when SysV shared memory is detected; also accepts `ON` or `OFF`. |
| `XMIN_ENABLE_TCP` | `OFF` | Enable the future TCP transport. Local sockets remain the normal path. |
| `XMIN_PIXMAN_SIMD` | `AUTO` | Control future optional pixman SIMD implementations. |
| `XMIN_BUILD_LAUNCHER` | `ON` | Build the integrated authenticated launcher when its source is added. |
| `XMIN_BUILD_TESTS` | top-level `ON` | Build the smoke and later protocol tests. |
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
  client/glx/           separately shipped GLX/OSMesa client bridge
  launcher/             integrated authenticated process launcher
  support/              Xmin-owned adapters, launcher, and portability code
  server/               final executable and temporary stub
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
