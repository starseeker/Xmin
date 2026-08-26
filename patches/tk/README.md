# Tk client patches

The patches are tied to exact Tk revisions:

- `tk-8.6.14-xmin.patch`: Tk 8.6.14 at commit
  `f195332797683d3a7c048a0d1282b9ef1781435f`, as used by bext.
- `tk-9.0.4-xmin.patch`: Tk 9.0.4 at commit
  `584f8fcf62c320d7c341e77171188cb4d79c3725`.

Apply the matching patch at the root of a Tk checkout with `patch -p1`. Build
and install Xmin with `XMIN_BUILD_TOOLKIT_CLIENT=ON`, then configure Tk with
the installed SDK. Tk 8.6 uses a matching Tcl 8.6 build:

```sh
cd unix
./configure --with-tcl=/path/to/tcl8.6-build-or-lib \
  --with-xmin=/path/to/xmin-prefix --enable-xft
make
```

Tk 9 additionally supports disabling libcups when it is not wanted:

```sh
cd unix
./configure --with-tcl=/path/to/tcl9-build-or-lib \
  --with-xmin=/path/to/xmin-prefix --enable-xft --disable-libcups
make
```

The explicit `--with-xmin` mode checks Xmin-specific markers in the selected
Xlib, XKB, Xft, and Fontconfig headers, then compiles and links the client entry
points Tk needs. It bypasses Tk's host X discovery and its later Xbsd, MIT X11,
Xft, Fontconfig, X11/XKB, XScreenSaver, and Xext library probes. Xft behavior is
supplied by Xmin's embedded-font facade. Configuration fails instead of mixing
in or falling back to system X when the selected SDK is incomplete. Normal Tk
builds are unchanged.

Both source configure inputs and the release-generated `configure` scripts are
patched, so Autoconf is not required. The Tk 8.6 generated-script changes are
kept compatible with that release's Autoconf 2.59 output.

## Building through bext

Bext supports semicolon-separated `TK_EXTRA_PATCHES` and `TK_CONFIGURE_ARGS`
cache settings. To force bundled Tcl/Tk 8.6 and apply the Xmin patch:

```sh
cmake -S /usr/home/starseeker/bext -B /path/to/bext-build \
  -DENABLE_TCL=ON \
  -DTK_EXTRA_PATCHES=/usr/home/starseeker/Xmin/patches/tk/tk-8.6.14-xmin.patch \
  -DTK_CONFIGURE_ARGS=--with-xmin=/path/to/xmin-prefix
cmake --build /path/to/bext-build --target TK_BLD-install
```

Every `TK_EXTRA_PATCHES` entry must be an absolute path. Multiple configure
arguments or patches must be passed as a quoted, semicolon-separated CMake
list.
