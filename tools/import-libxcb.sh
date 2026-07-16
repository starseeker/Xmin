#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/libxcb-1.17.0 /path/to/xcb-proto-1.17.0" >&2
    exit 2
fi

libxcb=$1
xcbproto=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -f "$libxcb/src/xcb_conn.c" ] || \
   [ ! -f "$libxcb/src/c_client.py" ]; then
    echo "not an extracted libxcb 1.17.0 tree: $libxcb" >&2
    exit 2
fi
if [ ! -f "$xcbproto/src/xproto.xml" ] || \
   [ ! -f "$xcbproto/xcbgen/state.py" ]; then
    echo "not an extracted xcb-proto 1.17.0 tree: $xcbproto" >&2
    exit 2
fi

generated=$(mktemp -d "${TMPDIR:-/tmp}/xmin-libxcb.XXXXXX")
trap 'rm -rf "$generated"' EXIT HUP INT TERM

protocols="xproto bigreq xc_misc render shape xfixes damage composite xtest"
profile_protocols="xproto bigreq ge shape xinput xtest sync xkb xc_misc xfixes render randr composite damage present shm dbe screensaver xinerama glx"
(
    cd "$generated"
    for protocol in $protocols; do
        PYTHONDONTWRITEBYTECODE=1 python3 "$libxcb/src/c_client.py" \
            -c "libxcb 1.17.0" -l X -s 3 -p "$xcbproto" \
            "$xcbproto/src/$protocol.xml"
    done
)

install -d "$root/third_party/libxcb/include/xcb"
for header in xcb.h xcbext.h; do
    cp -p "$libxcb/src/$header" \
        "$root/third_party/libxcb/include/xcb/$header"
done
for protocol in $protocols; do
    cp -p "$generated/$protocol.h" \
        "$root/third_party/libxcb/include/xcb/$protocol.h"
done

install -d "$root/third_party/libxcb/src"
for source in xcb_conn.c xcb_out.c xcb_in.c xcb_ext.c xcb_xid.c xcb_list.c; do
    cp -p "$libxcb/src/$source" "$root/third_party/libxcb/src/$source"
done
cp -p "$libxcb/src/xcbint.h" "$root/third_party/libxcb/src/xcbint.h"
for protocol in $protocols; do
    cp -p "$generated/$protocol.c" "$root/third_party/libxcb/src/$protocol.c"
done

install -d "$root/LICENSES/libxcb" "$root/LICENSES/xcb-proto"
cp -p "$libxcb/COPYING" "$root/LICENSES/libxcb/COPYING"
cp -p "$xcbproto/COPYING" "$root/LICENSES/xcb-proto/COPYING"

# Retain the declarative wire descriptions used by Xmin-next's narrow
# generator and support-profile checker.  Generated files remain committed;
# the normal product build does not need Python or xcb-proto.
install -d "$root/protocol/xcb"
for protocol in $profile_protocols; do
    cp -p "$xcbproto/src/$protocol.xml" "$root/protocol/xcb/$protocol.xml"
done

echo "Imported the private libxcb/xcb-proto 1.17.0 automation subset."
