#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/pixman-0.46.2" >&2
    exit 2
fi

source_tree=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest=$root/tools/pixman-files.txt

if [ ! -f "$source_tree/pixman/pixman.h" ] || \
   [ ! -f "$source_tree/COPYING" ]; then
    echo "not a pixman source tree: $source_tree" >&2
    exit 2
fi

while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    source_file=$source_tree/pixman/$relative
    destination=$root/third_party/pixman/pixman/$relative
    if [ ! -f "$source_file" ]; then
        echo "missing allowlisted pixman file: $relative" >&2
        exit 2
    fi
    install -d "$(dirname -- "$destination")"
    cp -p "$source_file" "$destination"
done < "$manifest"

install -d "$root/LICENSES/pixman"
cp -p "$source_tree/COPYING" "$root/LICENSES/pixman/COPYING"

echo "Imported the exact pixman 0.46.2 allowlist."
