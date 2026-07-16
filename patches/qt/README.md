# Qt Xmin patch

`qt_xmin.patch` adds the explicit `-xmin-x11` Qt configure option. In this
mode Qt consumes the installed `Xmin::QtX11` target instead of discovering and
linking the conventional XCB, Xlib, and xkbcommon packages. Ordinary Qt
configuration is unchanged when the option is absent.

The same option supports two deliberately separate profiles:

- raster Qt uses qxcb's existing no-Xlib path and `-no-opengl`;
- desktop OpenGL uses `Xmin::GL` plus the added `xcb_xmin` GL integration.
  That integration hands Qt's existing XCB connection and window IDs to Xmin's
  software-direct GL bridge. It does not enable Qt's Xlib/GLX plugin, EGL,
  DRI, DRM, GBM, Vulkan, or hardware rendering.

The patch was made and build-tested against the BRL-CAD bext Qt source at
'~/bext/qt/qt` on 2026-07-16:

- Qt version: 6.11.1
- top-level commit: `692cacdb1d6eea560daac0339ca8c45a89ae7c37`
- `qtbase` tree: `eaedabb16aee6fc1d71441c9be92bc7dd5055a50`

Do not patch that bext source in place. Copy `qtbase` to an ignored work
directory and apply the patch from the copied `qtbase` root:

```sh
patch -p1 < /path/to/Xmin_qt/patches/qt/qt_xmin.patch
```

The common tested profile uses:

```text
-qpa xcb -default-qpa xcb -xcb -xmin-x11
-no-xcb-xlib -no-feature-xlib -no-sm
-no-egl -no-eglfs -no-vulkan
-no-feature-gbm -no-feature-kms
-no-feature-wayland -no-feature-wayland-client -no-feature-wayland-server
-no-fontconfig -qt-freetype -qt-harfbuzz
-qt-zlib -qt-libpng -qt-libjpeg
-no-dbus -no-glib -nomake examples -nomake tests
```

Add `-no-opengl` for raster Qt or `-opengl desktop` for software OpenGL.
Pass the Xmin client SDK prefix after `--` with `-DCMAKE_PREFIX_PATH=...`.
On FreeBSD also use `-no-feature-inotify`: a host `libinotify` installation can
otherwise make this Qt snapshot select its inotify source even though QtCore's
FreeBSD dispatch path requires kqueue.

The raster branch changes no qxcb C++ runtime source. The OpenGL branch adds a
small Qt plugin because the stock `xcb_glx` plugin is coupled to Xlib and
`Xlibint.h`. The Xmin plugin is selected first automatically when it is built;
no `QT_XCB_GL_INTEGRATION` override is required.

Forward and reverse dry runs were validated on a fresh copy, and the applied
copy was byte-compared with the build-tested source. See `qt_building.txt` for
the exact build, runtime, dependency-audit, and measurement results.
