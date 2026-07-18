#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 /path/to/libX11-1.8.13 /path/to/xorgproto-2025.1 /path/to/libXrender-0.9.12 /path/to/libXft-2.3.9" >&2
    exit 2
fi

libx11=$1
xorgproto=$2
libxrender=$3
libxft=$4
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
destination=$root/src/client/x11/include/X11

if [ ! -f "$libx11/include/X11/Xlib.h" ] || \
   [ ! -f "$xorgproto/include/X11/X.h" ] || \
   [ ! -f "$libxrender/include/X11/extensions/Xrender.h" ] || \
   [ ! -f "$libxft/include/X11/Xft/Xft.h" ]; then
    echo "one or more X11 public-header source trees are invalid" >&2
    exit 2
fi

copy_header()
{
    source_root=$1
    relative=$2
    install -d "$(dirname -- "$destination/$relative")"
    cp -p "$source_root/$relative" "$destination/$relative"
}

for header in \
    Xlib.h Xutil.h Xlocale.h Xregion.h Xresource.h XKBlib.h cursorfont.h; do
    copy_header "$libx11/include/X11" "$header"
done

for header in \
    X.h Xatom.h Xdefs.h Xfuncproto.h Xmd.h Xosdefs.h Xproto.h Xprotostr.h \
    keysym.h keysymdef.h extensions/render.h extensions/XKB.h \
    extensions/XKBgeom.h extensions/XKBstr.h; do
    copy_header "$xorgproto/include/X11" "$header"
done

copy_header "$libxrender/include/X11" extensions/Xrender.h
copy_header "$libxft/include/X11" Xft/Xft.h
copy_header "$libxft/include/X11" Xft/XftCompat.h
patch --no-backup-if-mismatch "$destination/Xft/Xft.h" \
    < "$root/patches/toolkit-client/0001-xft-drop-freetype-header.patch"

install -d "$root/LICENSES/libX11" "$root/LICENSES/xorgproto" \
    "$root/LICENSES/libXrender" "$root/LICENSES/libXft"
cp -p "$libx11/COPYING" "$root/LICENSES/libX11/COPYING"
cp -p "$xorgproto/COPYING-x11proto" "$root/LICENSES/xorgproto/COPYING-x11proto"
cp -p "$xorgproto/COPYING-renderproto" "$root/LICENSES/xorgproto/COPYING-renderproto"
cp -p "$xorgproto/COPYING-kbproto" "$root/LICENSES/xorgproto/COPYING-kbproto"
cp -p "$libxrender/COPYING" "$root/LICENSES/libXrender/COPYING"
cp -p "$libxft/COPYING" "$root/LICENSES/libXft/COPYING"

echo "Imported the pinned Xlib/XRender/Xft public-header allowlist."
