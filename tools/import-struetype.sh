#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/struetype-223c658" >&2
    exit 2
fi

source_tree=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -f "$source_tree/struetype.h" ] || \
   [ ! -f "$source_tree/LICENSE" ] || \
   [ ! -f "$source_tree/README.md" ]; then
    echo "not a struetype source tree: $source_tree" >&2
    exit 2
fi

verify_file()
{
    expected=$1
    source_file=$2
    actual=$(sha256sum "$source_file" | cut -d ' ' -f 1)
    if [ "$actual" != "$expected" ]; then
        echo "unexpected checksum for $source_file" >&2
        exit 2
    fi
}

verify_file 6e3306f8ce1c2d786c3a7ec527d2775be8f7c3e8d8fa3eef78fb429b64e524f9 \
    "$source_tree/struetype.h"
verify_file 2e144a5ed0195ba77b860d2e3868ca58768105c8ca8efd43c8f534d730e6af6b \
    "$source_tree/LICENSE"
verify_file ab8527080c93b4cbc269d473f11ef413c8b3972559eb0aeb52fcda61b89259e4 \
    "$source_tree/README.md"

install -d "$root/third_party/struetype"
cp -p "$source_tree/struetype.h" "$root/third_party/struetype/struetype.h"
cp -p "$source_tree/README.md" "$root/third_party/struetype/README.md"

install -d "$root/LICENSES/struetype"
cp -p "$source_tree/LICENSE" "$root/LICENSES/struetype/LICENSE"

echo "Imported struetype commit 223c6580907e87df3a262ca7f382df5314a1710d."
