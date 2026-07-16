#!/bin/sh

set -eu

if [ "$#" -ne 8 ]; then
    echo "usage: $0 /path/to/xorgproto-2024.1 /path/to/xorg-server-21.1.23 /path/to/pixman-0.46.2 /path/to/libXfont2-2.0.8 /path/to/xtrans-1.6.0 /path/to/libXau-1.0.12 /path/to/libxkbfile-1.2.0 /path/to/xkeyboard-config-2.47" >&2
    exit 2
fi

xorgproto=$1
xserver=$2
pixman=$3
libxfont2=$4
xtrans=$5
libxau=$6
libxkbfile=$7
xkeyboard_config=$8
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
if [ ! -f "$libxfont2/include/X11/fonts/libxfont2.h" ]; then
    echo "not an extracted libXfont2 2.0.8 tree: $libxfont2" >&2
    exit 2
fi
if [ ! -f "$xtrans/transport.c" ]; then
    echo "not an extracted xtrans 1.6.0 tree: $xtrans" >&2
    exit 2
fi
if [ ! -f "$libxau/AuRead.c" ]; then
    echo "not an extracted libXau 1.0.12 tree: $libxau" >&2
    exit 2
fi
if [ ! -f "$libxkbfile/include/X11/extensions/XKMformat.h" ]; then
    echo "not an extracted libxkbfile 1.2.0 tree: $libxkbfile" >&2
    exit 2
fi
if [ ! -f "$xkeyboard_config/keycodes/evdev" ] || \
   [ ! -f "$xkeyboard_config/symbols/us" ]; then
    echo "not an extracted xkeyboard-config 2.47 tree: $xkeyboard_config" >&2
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

font_headers="FS.h FSproto.h font.h fontproto.h fontstruct.h fsmasks.h"
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

xext_sources="
bigreq.c geext.c hashtable.c saver.c shape.c sleepuntil.c sync.c xcmisc.c xtest.c shm.c
"
xext_headers="
geext.h geint.h hashtable.h shmint.h sleepuntil.h syncsdk.h syncsrv.h xace.h
"
# MIT-SHM is a build-time option, but retain its source in the reproducible
# import so changing profiles never requires a second upstream tree.
# shellcheck disable=SC2086
copy_files "$xserver/Xext" "$root/third_party/xorg-server/Xext" \
    $xext_sources $xext_headers
xi_sources="
allowev.c chgdctl.c chgfctl.c chgkbd.c chgkmap.c chgprop.c chgptr.c
closedev.c devbell.c exevents.c extinit.c getbmap.c getdctl.c getfctl.c
getfocus.c getkmap.c getmmap.c getprop.c getselev.c getvers.c grabdev.c
grabdevb.c grabdevk.c gtmotion.c listdev.c opendev.c queryst.c selectev.c
sendexev.c setbmap.c setdval.c setfocus.c setmmap.c setmode.c stubs.c
ungrdev.c ungrdevb.c ungrdevk.c xiallowev.c xibarriers.c xichangecursor.c
xichangehierarchy.c xigetclientpointer.c xigrabdev.c xipassivegrab.c
xiproperty.c xiquerydevice.c xiquerypointer.c xiqueryversion.c xiselectev.c
xisetclientpointer.c xisetdevfocus.c xiwarppointer.c
"
xi_headers="
allowev.h chgdctl.h chgfctl.h chgkbd.h chgkmap.h chgprop.h chgptr.h
closedev.h devbell.h exglobals.h getbmap.h getdctl.h getfctl.h getfocus.h
getkmap.h getmmap.h getprop.h getselev.h getvers.h grabdev.h grabdevb.h
grabdevk.h gtmotion.h listdev.h opendev.h queryst.h selectev.h sendexev.h
setbmap.h setdval.h setfocus.h setmmap.h setmode.h ungrdev.h ungrdevb.h
ungrdevk.h xiallowev.h xibarriers.h xichangecursor.h xichangehierarchy.h
xigetclientpointer.h xigrabdev.h xipassivegrab.h xiproperty.h xiquerydevice.h
xiquerypointer.h xiqueryversion.h xiselectev.h xisetclientpointer.h
xisetdevfocus.h xiwarppointer.h
"
# shellcheck disable=SC2086
copy_files "$xserver/Xi" "$root/third_party/xorg-server/Xi" \
    $xi_sources $xi_headers
xkb_sources="
ddxBeep.c ddxCtrls.c ddxLEDs.c ddxLoad.c maprules.c xkmread.c xkbtext.c
xkbfmisc.c xkbout.c xkb.c xkbUtils.c xkbEvents.c xkbAccessX.c xkbSwap.c
xkbLEDs.c xkbInit.c xkbActions.c xkbPrKeyEv.c XKBMisc.c XKBAlloc.c
XKBGAlloc.c XKBMAlloc.c ddxKillSrv.c ddxPrivate.c ddxVT.c
"
xkb_headers="xkb.h xkbDflts.h xkbgeom.h"
# shellcheck disable=SC2086
copy_files "$xserver/xkb" "$root/third_party/xorg-server/xkb" \
    $xkb_sources $xkb_headers
copy_files "$xserver/dix" "$root/third_party/xorg-server/dix" \
    atom.c colormap.c cursor.c devices.c dispatch.c dispatch.h dixfonts.c \
    dixutils.c enterleave.c enterleave.h eventconvert.c events.c extension.c \
    gc.c gestures.c getevents.c globals.c glyphcurs.c grabs.c initatoms.c \
    inpututils.c main.c pixmap.c privates.c property.c ptrveloc.c region.c \
    registry.c resource.c selection.c swaprep.c swapreq.c tables.c touch.c \
    window.c
copy_files "$xserver/miext/damage" \
    "$root/third_party/xorg-server/miext/damage" damage.c damage.h damagestr.h
copy_files "$xserver/miext/sync" "$root/third_party/xorg-server/miext/sync" \
    misync.c misyncfd.c misync.h misyncfd.h misyncstr.h
copy_files "$xserver/mi" "$root/third_party/xorg-server/mi" \
    miarc.c mibitblt.c micmap.c micmap.h micoord.h micopy.c midash.c \
    midispcur.c mieq.c miexpose.c mifillarc.c mifillarc.h mifillrct.c \
    mifpoly.h migc.c migc.h miglblt.c mi.h miline.h mioverlay.c mioverlay.h \
    mipointer.c mipointer.h mipointrst.h mipoly.c mipoly.h mipolypnt.c \
    mipolyrect.c mipolyseg.c mipolytext.c mipushpxl.c miscanfill.h \
    miinitext.c miinitext.h miscrinit.c misprite.c misprite.h mistruct.h mivalidate.h mivaltree.c \
    miwideline.c miwideline.h miwindow.c mizerarc.c mizerarc.h mizerclip.c \
    mizerline.c
copy_files "$xserver/fb" "$root/third_party/xorg-server/fb" \
    fballpriv.c fbarc.c fbbits.c fbbits.h fbblt.c fbbltone.c fbcmap_mi.c \
    fbcopy.c fbfill.c fbfillrect.c fbfillsp.c fbgc.c fbgetsp.c fbglyph.c \
    fb.h fbimage.c fbline.c fboverlay.c fboverlay.h fbpict.c fbpict.h \
    fbpixmap.c fbpoint.c fbpush.c fbrop.h fbscreen.c fbseg.c fbsetsp.c \
    fbsolid.c fbtrap.c fbutil.c fbwindow.c wfbrename.h
copy_files "$xserver/randr" "$root/third_party/xorg-server/randr" \
    randr.c randrstr.h rrcrtc.c rrdispatch.c rrinfo.c rrlease.c rrmode.c \
    rrmonitor.c rroutput.c rrpointer.c rrproperty.c rrprovider.c \
    rrproviderproperty.c rrscreen.c rrsdispatch.c rrtransform.c rrtransform.h \
    rrxinerama.c
copy_files "$xserver/render" "$root/third_party/xorg-server/render" \
    animcur.c filter.c glyph.c glyphstr.h matrix.c miindex.c mipict.c mipict.h \
    mirect.c mitrap.c mitri.c picture.c picture.h picturestr.h render.c
copy_files "$xserver/present" "$root/third_party/xorg-server/present" \
    present.c present_event.c present_execute.c present_fake.c present_fence.c \
    present_notify.c present_request.c present_scmd.c present_screen.c \
    present_vblank.c present.h presentext.h present_priv.h
copy_files "$xserver/composite" "$root/third_party/xorg-server/composite" \
    compalloc.c compext.c compinit.c compoverlay.c compwindow.c compint.h \
    compositeext.h
copy_files "$xserver/dbe" "$root/third_party/xorg-server/dbe" \
    dbe.c dbestruct.h midbe.c midbe.h
copy_files "$xserver/xfixes" "$root/third_party/xorg-server/xfixes" \
    cursor.c disconnect.c region.c saveset.c select.c xfixes.c \
    xfixes.h xfixesint.h
copy_files "$xserver/damageext" "$root/third_party/xorg-server/damageext" \
    damageext.c damageextint.h
glx_sources="
indirect_dispatch.c indirect_dispatch_swap.c indirect_reqsize.c
indirect_size_get.c indirect_table.c clientinfo.c createcontext.c
extension_string.c indirect_util.c indirect_program.c
indirect_texture_compression.c glxcmds.c glxcmdsswap.c glxext.c glxscreens.c
render2.c render2swap.c renderpix.c renderpixswap.c rensize.c single2.c
single2swap.c singlepix.c singlepixswap.c singlesize.c swap_interval.c xfont.c
vndcmds.c vndext.c vndservermapping.c vndservervendor.c vnd_dispatch_stubs.c
"
glx_headers="
extension_string.h glxbyteorder.h glxcontext.h glxdrawable.h glxext.h
glxscreens.h glxserver.h glxutil.h indirect_dispatch.h indirect_reqsize.h
indirect_size.h indirect_size_get.h indirect_table.h indirect_util.h
singlesize.h unpack.h vndserver.h vndservervendor.h
"
# The DRI/DRM-facing glxdriswrast.c and glxdricommon.c sources are
# intentionally excluded.  src/glx/xmin_glx_osmesa.c supplies the only
# provider and calls the embedded OSMesa renderer directly.
# shellcheck disable=SC2086
copy_files "$xserver/glx" "$root/third_party/xorg-server/glx" \
    $glx_sources $glx_headers
copy_files "$xserver/hw/vfb" "$root/third_party/xorg-server/hw/vfb" \
    InitInput.c InitOutput.c
copy_files "$xserver/hw/vfb/man" "$root/third_party/xorg-server/hw/vfb/man" \
    Xvfb.man
copy_files "$xserver/os" "$root/third_party/xorg-server/os" \
    WaitFor.c access.c auth.c backtrace.c client.c connection.c inputthread.c io.c \
    log.c mitauth.c oscolor.c osdep.h osinit.c ospoll.c ospoll.h \
    reallocarray.c strcasecmp.c strcasestr.c strlcat.c strlcpy.c strndup.c \
    timingsafe_memcmp.c utils.c xprintf.c xserver_poll.c xstrans.c
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0001-omit-network-si-hosts-without-tcp.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0002-vfb-xmin-defaults-and-cleanup.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0003-vfb-xmin-device-names.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0004-randr-xinerama-without-panoramix.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0005-skip-absent-tcp-xtrans.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0006-enable-xmin-indirect-glx.patch"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/xorg-server/0007-lock-dynamic-display-allocation.patch"

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

copy_files "$libxfont2/include/X11/fonts" \
    "$root/third_party/libXfont2/include/X11/fonts" \
    bdfint.h bitmap.h bufio.h fntfil.h fntfilio.h fntfilst.h fontmisc.h \
    fontshow.h fontutil.h fontxlfd.h libxfont2.h pcf.h
copy_files "$libxfont2/include" "$root/third_party/libXfont2/include" \
    libxfontint.h
copy_files "$libxfont2/src/stubs" "$root/third_party/libXfont2/src/stubs" \
    atom.c libxfontstubs.c
copy_files "$libxfont2/src/util" "$root/third_party/libXfont2/src/util" \
    fontaccel.c fontnames.c fontutil.c fontxlfd.c format.c patcache.c \
    private.c utilbitmap.c replace.h
copy_files "$libxfont2/src/fontfile" \
    "$root/third_party/libXfont2/src/fontfile" \
    bitsource.c bufio.c defaults.c fontdir.c fontfile.c fontscale.c renderers.c
copy_files "$libxfont2/src/bitmap" "$root/third_party/libXfont2/src/bitmap" \
    bitmap.c pcfread.c
copy_files "$libxfont2/src/builtins" \
    "$root/third_party/libXfont2/src/builtins" \
    builtin.h dir.c fpe.c render.c
install -d "$root/LICENSES/libXfont2"
cp -p "$libxfont2/COPYING" "$root/LICENSES/libXfont2/COPYING"
patch -s -V none -d "$root" -p1 < \
    "$root/patches/libXfont2/0001-builtins-only-fontfile-fpe.patch"
python3 "$root/tools/generate-builtin-fonts.py" \
    "$libxfont2/src/builtins/fonts.c" \
    "$root/data/fonts/xmin_builtin_fonts.c"

copy_files "$xtrans" "$root/third_party/xtrans/include/X11/Xtrans" \
    Xtrans.c Xtrans.h Xtransint.h Xtranssock.c Xtransutil.c transport.c
install -d "$root/LICENSES/xtrans"
cp -p "$xtrans/COPYING" "$root/LICENSES/xtrans/COPYING"

copy_files "$libxau" "$root/third_party/libXau" AuDispose.c AuRead.c
copy_files "$libxau/include/X11" \
    "$root/third_party/libXau/include/X11" Xauth.h
install -d "$root/LICENSES/libXau"
cp -p "$libxau/COPYING" "$root/LICENSES/libXau/COPYING"

copy_files "$libxkbfile/include/X11/extensions" \
    "$root/third_party/libxkbfile/include/X11/extensions" XKM.h XKMformat.h
install -d "$root/LICENSES/libxkbfile"
cp -p "$libxkbfile/COPYING" "$root/LICENSES/libxkbfile/COPYING"

install -d "$root/LICENSES/xkeyboard-config"
cp -p "$xkeyboard_config/COPYING" \
    "$root/LICENSES/xkeyboard-config/COPYING"
"$root/tools/generate-xkb-map.sh" "$xkeyboard_config" \
    "$root/data/xkb/xmin-us-map.h"

echo "Imported xorgproto 2024.1, selected xorg-server 21.1.23 DIX/MI/fb/OS/vfb/Render/RandR/Present/Composite/DBE/Damage/XFixes/XInput/XKB/core-extension sources, non-DRI GLX protocol and vendor-dispatch sources, generic pixman 0.46.2, the built-in-font subset of libXfont2 2.0.8, local-only xtrans 1.6.0, the libXau 1.0.12 authority reader, libxkbfile 1.2.0 XKM headers, and generated font/XKB data."
