#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 /path/to/xorgproto-2024.1 /path/to/xorg-server-21.1.23 /path/to/pixman-0.46.2" >&2
    exit 2
fi

xorgproto=$1
xserver=$2
pixman=$3
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -f "$xorgproto/include/X11/Xproto.h" ]; then
    echo "not an extracted xorgproto 2024.1 tree: $xorgproto" >&2
    exit 2
fi
if [ ! -f "$xserver/dix/atom.c" ]; then
    echo "not an extracted xorg-server 21.1.23 tree: $xserver" >&2
    exit 2
fi
if [ ! -f "$pixman/pixman/pixman.c" ]; then
    echo "not an extracted pixman 0.46.2 tree: $pixman" >&2
    exit 2
fi

copy_files()
{
    source_dir=$1
    destination_dir=$2
    shift 2
    install -d "$destination_dir"
    for file in "$@"; do
        cp -p "$source_dir/$file" "$destination_dir/$file"
    done
}

core_headers="
DECkeysym.h HPkeysym.h Sunkeysym.h X.h XF86keysym.h XWDFile.h Xalloca.h
Xarch.h Xatom.h Xdefs.h Xfuncproto.h Xfuncs.h Xmd.h Xos.h Xos_r.h
Xosdefs.h Xpoll.h.in Xproto.h Xprotostr.h Xthreads.h Xw32defs.h Xwindows.h
Xwinsock.h ap_keysym.h keysym.h keysymdef.h
"

extension_headers="
bigreqsproto.h bigreqstr.h composite.h compositeproto.h damageproto.h
damagewire.h dbe.h dbeproto.h dpmsconst.h dri3proto.h ge.h geproto.h panoramiXproto.h
presentproto.h presenttokens.h randr.h randrproto.h render.h renderproto.h
saver.h saverproto.h shapeconst.h shapeproto.h shapestr.h shm.h shmproto.h
shmstr.h syncconst.h syncproto.h syncstr.h xcmiscproto.h xcmiscstr.h
xfixesproto.h xfixeswire.h XI.h XIproto.h XI2.h XI2proto.h XKB.h XKBproto.h
XKBsrv.h XKBstr.h xtestconst.h xtestproto.h
"

font_headers="font.h fontproto.h fontstruct.h fsmasks.h"
gl_headers="glxint.h glxmd.h glxproto.h glxtokens.h"

# shellcheck disable=SC2086
copy_files "$xorgproto/include/X11" \
    "$root/third_party/xorgproto/include/X11" $core_headers
# shellcheck disable=SC2086
copy_files "$xorgproto/include/X11/extensions" \
    "$root/third_party/xorgproto/include/X11/extensions" $extension_headers
# shellcheck disable=SC2086
copy_files "$xorgproto/include/X11/fonts" \
    "$root/third_party/xorgproto/include/X11/fonts" $font_headers
# shellcheck disable=SC2086
copy_files "$xorgproto/include/GL" \
    "$root/third_party/xorgproto/include/GL" $gl_headers
copy_files "$xorgproto/include/GL/internal" \
    "$root/third_party/xorgproto/include/GL/internal" glcore.h

install -d "$root/LICENSES/xorgproto"
for license in "$xorgproto"/COPYING-*; do
    cp -p "$license" "$root/LICENSES/xorgproto/$(basename -- "$license")"
done

# Private server headers are highly interconnected and have no runtime cost.
# Keep this boundary intact for the initial port; trim it only after all retained
# server components compile and an include-dependency audit can prove a header dead.
install -d "$root/third_party/xorg-server/include"
for header in "$xserver"/include/*.h; do
    cp -p "$header" "$root/third_party/xorg-server/include/$(basename -- "$header")"
done

copy_files "$xserver/Xext" "$root/third_party/xorg-server/Xext" geext.h
copy_files "$xserver/dix" "$root/third_party/xorg-server/dix" atom.c initatoms.c
copy_files "$xserver/os" "$root/third_party/xorg-server/os" \
    reallocarray.c strcasecmp.c strcasestr.c strlcat.c strlcpy.c strndup.c \
    timingsafe_memcmp.c

install -d "$root/LICENSES/xorg-server"
cp -p "$xserver/COPYING" "$root/LICENSES/xorg-server/COPYING"

pixman_sources="
pixman.c pixman-access.c pixman-access-accessors.c pixman-arm.c
pixman-bits-image.c pixman-combine32.c pixman-combine-float.c
pixman-conical-gradient.c pixman-edge.c pixman-edge-accessors.c
pixman-fast-path.c pixman-filter.c pixman-glyph.c pixman-general.c
pixman-gradient-walker.c pixman-image.c pixman-implementation.c
pixman-linear-gradient.c pixman-matrix.c pixman-mips.c pixman-noop.c
pixman-ppc.c pixman-radial-gradient.c pixman-region.c pixman-region16.c
pixman-region32.c pixman-region64f.c pixman-riscv.c pixman-solid-fill.c
pixman-timer.c pixman-trap.c pixman-utils.c pixman-x86.c
"

install -d "$root/third_party/pixman/pixman"
for header in "$pixman"/pixman/*.h; do
    cp -p "$header" "$root/third_party/pixman/pixman/$(basename -- "$header")"
done
# shellcheck disable=SC2086
copy_files "$pixman/pixman" "$root/third_party/pixman/pixman" $pixman_sources
copy_files "$pixman/pixman" "$root/third_party/pixman/pixman" pixman-version.h.in
copy_files "$pixman/pixman/dither" \
    "$root/third_party/pixman/pixman/dither" blue-noise-64x64.h

install -d "$root/LICENSES/pixman"
cp -p "$pixman/COPYING" "$root/LICENSES/pixman/COPYING"

echo "Imported xorgproto 2024.1, the initial xorg-server 21.1.23 tranche, and generic pixman 0.46.2."
