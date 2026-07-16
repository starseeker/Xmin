#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 qtbase" >&2
    exit 2
fi

qtbase=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
font="$qtbase/src/3rdparty/wasm/DejaVuSans.ttf"
license="$qtbase/src/3rdparty/wasm/DEJAVU-LICENSE"

if [ ! -f "$font" ] || [ ! -f "$license" ]; then
    echo "Qt's pinned DejaVu Sans font or license is missing" >&2
    exit 2
fi

install -d "$root/data/qt-fonts" "$root/LICENSES/dejavu"
cp -p "$font" "$root/data/qt-fonts/DejaVuSans.ttf"
cp -p "$license" "$root/LICENSES/dejavu/DEJAVU-LICENSE"

echo "Imported Qt's pinned DejaVu Sans application font."
