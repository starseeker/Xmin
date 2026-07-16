#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/osmesa-986a9ce0" >&2
    exit 2
fi

source_tree=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -f "$source_tree/src/drivers/osmesa/osmesa.c" ] || \
   [ ! -f "$source_tree/include/OSMesa/osmesa.h" ] || \
   [ ! -f "$source_tree/LICENSE" ]; then
    echo "not an OSMesa source tree: $source_tree" >&2
    exit 2
fi

copy_tree_files()
{
    source_root=$1
    destination_root=$2
    shift 2

    find "$source_root" -type f \( "$@" \) | while IFS= read -r source_file; do
        relative=${source_file#"$source_root"/}
        destination=$destination_root/$relative
        install -d "$(dirname -- "$destination")"
        cp -p "$source_file" "$destination"
    done
}

install -d "$root/third_party/osmesa"
cp -p "$source_tree/CMakeLists.txt" "$root/third_party/osmesa/CMakeLists.upstream.txt"
cp -p "$source_tree/src/CMakeLists.txt" \
    "$root/third_party/osmesa/src-CMakeLists.upstream.txt"
cp -p "$source_tree/LICENSE" "$root/third_party/osmesa/LICENSE"
cp -p "$source_tree/README" "$root/third_party/osmesa/README"

copy_tree_files "$source_tree/src" "$root/third_party/osmesa/src" \
    -name '*.c' -o -name '*.h'
copy_tree_files "$source_tree/include" "$root/third_party/osmesa/include" \
    -name '*.h'

python3 "$root/tools/generate-osmesa-sources.py" \
    "$source_tree/src/CMakeLists.txt" \
    "$root/third_party/osmesa/osmesa-sources.cmake"

patch -s -V none -d "$root" -p1 < \
    "$root/patches/osmesa/0001-separate-draw-read-buffers.patch"

install -d "$root/LICENSES/osmesa"
cp -p "$source_tree/LICENSE" "$root/LICENSES/osmesa/LICENSE"

echo "Imported OSMesa 986a9ce0a4fa9a0ee3a79c821b293aa47d6cab6c runtime sources."
