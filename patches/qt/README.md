# Qt Xmin integration patch

`qt_xmin.patch` adds the explicit `-xmin-x11` Qt configure option. In this
mode Qt consumes `Xmin::QtX11` instead of discovering the conventional XCB,
Xlib, xcb-util, and xkbcommon libraries. Configuration is unchanged when the
option is absent.

The option supports two profiles:

- raster qxcb uses Xmin's focused Xlib/XCB compatibility target with
  `-no-opengl`;
- desktop OpenGL uses `Xmin::GL` and Qt's stock `xcb_glx` integration.
  Rendering stays in Xmin's OSMesa-backed client `libGL`; MIT-SHM shared
  pixmaps and Present are used when available, with the portable X image
  upload retained as a fallback. It does not enable indirect GLX rendering,
  EGL, DRI, DRM, GBM, Vulkan, or hardware rendering.

The patch also corrects qxcb's shutdown helper to use `CopyFromParent` as the
visual for its `InputOnly` window, as required by the core X11 protocol. That
one-line correction is neutral and can be submitted to Qt independently.

When Qt is built without fontconfig, the patch deploys Xmin's pinned Go font
family into Qt's standard `lib/fonts` directory. Regular, bold, italic, and
bold-italic faces (plus their monospaced variants) are therefore available in
both the Qt build tree and an installed Qt without host font discovery.

## Validated source

The patch was made and build-tested on 2026-07-18 against this `qtbase` tree:

- Qt module version: 6.10.2 (`alpha1` prerelease segment)
- commit: `0c0a85eb92267bbd7e8c3cbd18590735871dc687`
- tree: `399d2c3e70c329d9ad59a98110234c232a4b16ef`

Apply it from an unmodified copied `qtbase` root:

```sh
patch -p1 < /path/to/Xmin/patches/qt/qt_xmin.patch
```

Forward and reverse dry runs should be checked when moving to a new Qt
snapshot. The patch now changes feature detection and target selection only;
it does not add an Xmin-specific qxcb GL implementation.

The validated desktop profile builds QtGui, the stock qxcb platform plugin,
and the stock `xcb_glx` integration plugin, then creates, makes current,
clears, and swaps a `QOpenGLContext` window on a launched Xmin server.

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
-xcb-xlib -feature-xlib -no-sm
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

The resulting Qt GUI/qxcb libraries have no dependency on a host Xlib, XCB,
xkbcommon, GLX, or EGL DSO: those standard ABI calls resolve to Xmin's client
target and its OSMesa-backed `libGL`.
