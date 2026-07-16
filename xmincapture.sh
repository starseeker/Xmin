#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${XMIN_BUILD_DIR:-${script_dir}/.build}"
display=${DISPLAY:-}
authority=${XAUTHORITY:-}
output_dir=""
capture_desktop=1
capture_windows=1
control=${XMIN_CONTROL:-}

usage()
{
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [OUTPUT_DIR]

Capture the current Xmin desktop and each named, mapped window as binary PPM.
Only xminctl is required; no host X11 client library or capture utility is used.

Options:
  --build-dir DIR   Xmin build directory (default: ${build_dir})
  --control FILE    xminctl executable (default: \$XMIN_CONTROL or build tree)
  --display DISPLAY X display to capture (default: \$DISPLAY)
  --authority FILE  Xauthority file (default: \$XAUTHORITY)
  --output-dir DIR  Output directory (default: timestamped directory)
  --desktop-only    Capture only the root desktop
  --windows-only    Capture only named, mapped windows
  -h, --help        Show this help

Run this inside the environment created by xminlaunch.sh or xmin-run so DISPLAY
and XAUTHORITY identify the private Xmin server.
EOF
}

die()
{
    printf '%s\n' "xmincapture.sh: $*" >&2
    exit 2
}

while (($# > 0)); do
    case "$1" in
        --build-dir)
            (($# >= 2)) || die "--build-dir requires a directory"
            build_dir=$2
            shift 2
            ;;
        --build-dir=*)
            build_dir=${1#*=}
            shift
            ;;
        --control)
            (($# >= 2)) || die "--control requires a file"
            control=$2
            shift 2
            ;;
        --control=*)
            control=${1#*=}
            shift
            ;;
        --display)
            (($# >= 2)) || die "--display requires a value"
            display=$2
            shift 2
            ;;
        --display=*)
            display=${1#*=}
            shift
            ;;
        --authority)
            (($# >= 2)) || die "--authority requires a file"
            authority=$2
            shift 2
            ;;
        --authority=*)
            authority=${1#*=}
            shift
            ;;
        --output-dir)
            (($# >= 2)) || die "--output-dir requires a directory"
            output_dir=$2
            shift 2
            ;;
        --output-dir=*)
            output_dir=${1#*=}
            shift
            ;;
        --desktop-only)
            capture_desktop=1
            capture_windows=0
            shift
            ;;
        --windows-only)
            capture_desktop=0
            capture_windows=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            [[ -z "$output_dir" ]] || die "more than one output directory specified"
            output_dir=$1
            shift
            ;;
    esac
done

(($# == 0)) || die "unexpected argument: $1"
[[ -n "$display" ]] || die "DISPLAY is not set; use --display or xminlaunch.sh"

if [[ -z "$control" ]]; then
    if [[ -x "${script_dir}/xminctl" ]]; then
        control="${script_dir}/xminctl"
    elif command -v xminctl >/dev/null 2>&1; then
        control=$(command -v xminctl)
    else
        control="${build_dir}/src/control/xminctl"
    fi
fi
[[ -x "$control" ]] || die "xminctl is not executable: $control"

export DISPLAY=$display
if [[ -n "$authority" ]]; then
    export XAUTHORITY=$authority
fi

if [[ -z "$output_dir" ]]; then
    output_dir="xmin-capture-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p -- "$output_dir"

capture_count=0
if ((capture_desktop)); then
    desktop_path="${output_dir}/desktop.ppm"
    "$control" --display "$display" capture-root "$desktop_path"
    printf '%s\n' "$desktop_path"
    ((capture_count += 1))
fi

if ((capture_windows)); then
    listing=$("$control" --display "$display" list-windows)
    printf '%s\n' "$listing" > "${output_dir}/windows.tsv"
    first_line=1
    while IFS=$'\t' read -r window_id x y width height state title; do
        if ((first_line)); then
            first_line=0
            continue
        fi
        [[ "$state" == mapped ]] || continue
        [[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]] || continue
        ((width > 1 && height > 1)) || continue
        stem="window-${window_id#0x}"
        ppm_path="${output_dir}/${stem}.ppm"
        if "$control" --display "$display" capture-window "$window_id" "$ppm_path"; then
            printf '%s\n' "$ppm_path"
            ((capture_count += 1))
        else
            printf '%s\n' "xmincapture.sh: capture failed: $window_id $title" >&2
        fi
    done <<< "$listing"
fi

printf 'Captured %d target(s) in %s\n' "$capture_count" "$output_dir"
