#!/bin/sh

set -eu

usage()
{
    echo "usage: $0 PREFIX raster|opengl [ELF ...]" >&2
    exit 2
}

[ "$#" -ge 2 ] || usage

prefix=${1%/}
mode=$2
shift 2

case "$mode" in
    raster|opengl) ;;
    *) usage ;;
esac

[ -d "$prefix" ] || {
    echo "audit-qt-bundle: prefix does not exist: $prefix" >&2
    exit 2
}

command -v readelf >/dev/null 2>&1 || {
    echo "audit-qt-bundle: readelf is required" >&2
    exit 2
}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/xmin-qt-audit.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
files="$tmpdir/files"
needed="$tmpdir/needed"
failures="$tmpdir/failures"
: > "$files"
: > "$needed"
: > "$failures"

for directory in "$prefix/lib" "$prefix/plugins" "$prefix/libexec" "$prefix/bin"; do
    if [ -d "$directory" ]; then
        find "$directory" -type f -print >> "$files"
    fi
done
for file do
    printf '%s\n' "$file" >> "$files"
done

sort -u "$files" -o "$files"

while IFS= read -r file; do
    [ -f "$file" ] || {
        echo "missing input: $file" >> "$failures"
        continue
    }
    if ! readelf -h "$file" >/dev/null 2>&1; then
        continue
    fi
    readelf -d "$file" 2>/dev/null |
        sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' |
        while IFS= read -r library; do
            printf '%s\t%s\n' "$library" "$file" >> "$needed"
        done
done < "$files"

while IFS="$(printf '\t')" read -r library file; do
    case "$library" in
        libX11.so*|libX11-xcb.so*|libXau.so*|libXdmcp.so*|libXext.so*|\
        libXrender.so*|libxcb.so*|libxcb-*.so*|libxkbcommon.so*|\
        libxkbcommon-x11.so*|libSM.so*|libICE.so*|libfontconfig.so*|\
        libEGL.so*|libGLX.so*|libOpenGL.so*|libdrm.so*|libgbm.so*|\
        libvulkan.so*|libwayland.so*|libwayland-*.so*|libglib-2.0.so*|\
        libdbus-1.so*)
            printf '%s requires forbidden %s\n' "$file" "$library" >> "$failures"
            ;;
        libGL.so*)
            if [ "$mode" = raster ]; then
                printf '%s requires forbidden %s\n' "$file" "$library" >> "$failures"
            fi
            ;;
    esac
done < "$needed"

if [ "$mode" = opengl ]; then
    if [ ! -f "$prefix/lib/libGL.so.1" ]; then
        echo "OpenGL profile is missing package-relative lib/libGL.so.1" >> "$failures"
    elif readelf -d "$prefix/lib/libGL.so.1" 2>/dev/null |
        sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' |
        grep -Eq '^(libX11|libX11-xcb|libxcb|libEGL|libGLX|libOpenGL|libdrm|libgbm|libvulkan)'; then
        echo "package-relative libGL.so.1 has a forbidden X/graphics dependency" >> "$failures"
    fi
fi

if [ -s "$failures" ]; then
    echo "Qt/Xmin dependency audit FAILED ($mode):" >&2
    sed 's/^/  /' "$failures" >&2
    exit 1
fi

echo "Qt/Xmin dependency audit passed ($mode)."
echo "Audited $(wc -l < "$files" | tr -d ' ') files; observed NEEDED names:"
cut -f1 "$needed" | sort -u | sed 's/^/  /'
