# Tk client patch

`tk-9.0.4-xmin.patch` is based on the upstream Tk 9.0.4 release:

- commit: `584f8fcf62c320d7c341e77171188cb4d79c3725`
- tree: `0b9b05e1ee257661d374b08a9560b4263582aedb`

Apply it at the root of a Tk checkout with `patch -p1`. Build and install Xmin
with `XMIN_BUILD_TOOLKIT_CLIENT=ON`, then configure Tk with the installed SDK:

```sh
cd unix
./configure --with-tcl=/path/to/tcl/lib \
  --with-xmin=/path/to/xmin-prefix --enable-xft --disable-libcups
make
```

The explicit `--with-xmin` option replaces Xlib, Xft, Fontconfig, FreeType,
and XScreenSaver linkage with Xmin's focused client facade and embedded fonts.
Normal Tk builds are unchanged. The patch updates both `configure.ac` and the
release's generated `configure`, so Autoconf is not required to use it.
