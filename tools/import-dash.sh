#!/bin/sh

set -eu

revision=6a1533f24b646364ad514c542292b5c85b2adabf
tree=6120591a4759404605aa6a04e686db521ef3fb55

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/dash-v0.5.13.4-checkout" >&2
    exit 2
fi

source_tree=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest=$root/tools/dash-files.txt
destination=$root/third_party/dash
temporary_root=${TMPDIR:-/tmp}
actual_files=$(mktemp "$temporary_root/xmin-dash-files-XXXXXX")
trap 'rm -f "$actual_files"' EXIT HUP INT TERM

if [ ! -d "$source_tree/.git" ] ||
   [ ! -f "$source_tree/configure.ac" ] ||
   [ ! -f "$source_tree/src/main.c" ] ||
   [ ! -f "$source_tree/src/mksignames.c" ] ||
   [ ! -f "$source_tree/COPYING" ]; then
    echo "not an official dash Git checkout: $source_tree" >&2
    exit 2
fi

actual_revision=$(git -C "$source_tree" rev-parse HEAD)
if [ "$actual_revision" != "$revision" ]; then
    echo "dash checkout is $actual_revision; expected $revision" >&2
    exit 2
fi
actual_tree=$(git -C "$source_tree" rev-parse 'HEAD^{tree}')
if [ "$actual_tree" != "$tree" ]; then
    echo "dash source tree is $actual_tree; expected $tree" >&2
    exit 2
fi
if ! grep -Fq 'AC_INIT([dash],[0.5.13.4])' "$source_tree/configure.ac"; then
    echo "dash checkout does not identify release 0.5.13.4" >&2
    exit 2
fi

# Keep the allowlist synchronized with the exact release tree. Git metadata
# and the GPL helper are the only deliberate omissions.
git -C "$source_tree" ls-files |
    sed '/^\.gitignore$/d;/^src\/\.gitignore$/d;/^src\/mksignames\.c$/d' \
    > "$actual_files"
if ! cmp -s "$manifest" "$actual_files"; then
    echo "tools/dash-files.txt does not match the pinned dash tree" >&2
    diff -u "$manifest" "$actual_files" >&2 || true
    exit 1
fi

gpl_paths=$(git -C "$source_tree" grep -l 'General Public License' |
    LC_ALL=C sort)
if [ "$gpl_paths" != "COPYING
src/mksignames.c" ]; then
    echo "dash GPL audit changed; review the new release before importing" >&2
    printf '%s\n' "$gpl_paths" >&2
    exit 1
fi

rm -rf "$destination"
while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    source_file=$source_tree/$relative
    target_file=$destination/$relative
    if [ ! -f "$source_file" ]; then
        echo "missing allowlisted dash file: $relative" >&2
        exit 1
    fi
    install -d "$(dirname -- "$target_file")"
    cp -p "$source_file" "$target_file"
done < "$manifest"

# Do not retain the upstream GNU Bash helper. The 0BSD implementation creates
# the platform-specific table from signal ABI facts during the dash build.
cp -p "$root/tools/dash-mksignames.c" "$destination/src/mksignames.c"
install -d "$root/LICENSES/dash"
cp -p "$source_tree/COPYING" "$root/LICENSES/dash/COPYING"

echo "Imported dash 0.5.13.4 without its GPL signal-name generator."
