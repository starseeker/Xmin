#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/go-image-source" >&2
    exit 2
fi

source_tree=$1/font/gofont/ttfs
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest=$root/tools/go-font-files.sha256

if [ ! -f "$source_tree/Go-Regular.ttf" ] || \
   [ ! -f "$source_tree/README" ]; then
    echo "not a Go image source tree: $1" >&2
    exit 2
fi

(
    cd "$source_tree"
    sha256sum -c "$manifest"
)

install -d "$root/third_party/fonts/go"
while read -r checksum filename; do
    cp -p "$source_tree/$filename" "$root/third_party/fonts/go/$filename"
done < "$manifest"

install -d "$root/LICENSES/go-fonts"
cp -p "$source_tree/README" "$root/LICENSES/go-fonts/LICENSE"

echo "Imported the exact Go font allowlist from image commit 315273a91ff5701d7239e64fd96351539939f8e7."
