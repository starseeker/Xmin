# Qt Xmin integration patch

`qt_xmin.patch` adds the explicit `-xmin-x11` Qt configure option. In this
mode Qt consumes `Xmin::QtX11` instead of discovering the conventional XCB,
Xlib, xcb-util, and xkbcommon libraries. Configuration is unchanged when the
option is absent.

The option supports two profiles:

- raster qxcb uses the existing no-Xlib path with `-no-opengl`;
- desktop OpenGL uses `Xmin::GL` and the added `xcb_xmin` GL integration.
  Rendering stays in Xmin's OSMesa-backed client `libGL` and pixels are
  presented over qxcb's existing XCB connection. It does not enable Xlib,
  server-side GLX, EGL, DRI, DRM, GBM, Vulkan, or hardware rendering.

The patch also corrects qxcb's shutdown helper to use `CopyFromParent` as the
visual for its `InputOnly` window, as required by the core X11 protocol. That
one-line correction is neutral and can be submitted to Qt independently.

## Validated source

The patch was made and build-tested on 2026-07-17 against this `qtbase` tree:

- Qt module version: 6.10.2 (`alpha1` prerelease segment)
- commit: `0c0a85eb92267bbd7e8c3cbd18590735871dc687`
- tree: `399d2c3e70c329d9ad59a98110234c232a4b16ef`

Apply it from an unmodified copied `qtbase` root:

```sh
patch -p1 < /path/to/Xmin/patches/qt/qt_xmin.patch
```

Forward and reverse dry runs should be checked when moving to a new Qt
snapshot. The Xmin-specific code uses qxcb private interfaces and therefore
must be rebuilt for each Qt version.

## Xmin SDK

Build and install the Xmin client SDK before configuring Qt:

```sh
cmake -S /path/to/Xmin -B /path/to/xmin-sdk-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/xmin-sdk \
  -DXMIN_BUILD_QT_CLIENT=ON \
  -DXMIN_BUILD_TESTS=OFF
cmake --build /path/to/xmin-sdk-build --parallel
cmake --install /path/to/xmin-sdk-build
```

`Xmin::QtX11` is Xmin's focused C++17 implementation of the XCB/xkbcommon
surface qxcb actually uses. It does not link the system XCB, xkbcommon, Xau,
or Xlib libraries. The installed standard headers define the public ABI; they
do not bring the upstream implementations into the build.

## Qt configuration

The common tested profile is:

```text
-release -qpa xcb -default-qpa xcb -xcb -xmin-x11
-no-xcb-xlib -no-feature-xlib -no-sm
-no-egl -no-eglfs -no-vulkan
-no-feature-gbm -no-feature-kms
-no-feature-wayland -no-feature-wayland-client -no-feature-wayland-server
-no-fontconfig -qt-freetype -qt-harfbuzz
-qt-zlib -qt-libpng -qt-libjpeg
-no-dbus -no-glib -nomake examples -nomake tests
```

Add `-no-opengl` for the raster-only build or `-opengl desktop` for Xmin
software OpenGL. After `--`, pass the installed SDK with
`-DCMAKE_PREFIX_PATH=/path/to/xmin-sdk`.

Both profiles were built successfully. Runtime acceptance verified qxcb
backing-store rendering and capture for raster, plus an OpenGL 2.0 GLSL draw,
readback, swap, and X-window capture for OpenGL. The resulting Qt GUI/qxcb
libraries had no dependency on a host Xlib, XCB, xkbcommon, GLX, or EGL DSO.
