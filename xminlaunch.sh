#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${XMIN_BUILD_DIR:-${script_dir}/.build}"
screen=""
dpi=""

usage()
{
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] -- COMMAND [ARG ...]

Launch a command on a private Xmin display using Xmin's software OpenGL.

Options:
  --build-dir DIR   Xmin build directory (default: ${build_dir})
  --screen WxHxD    Override the Xmin screen geometry
  --dpi DPI         Override the Xmin screen resolution
  -h, --help        Show this help

The build directory may also be set with XMIN_BUILD_DIR. XMIN_SERVER,
XMIN_RUNNER, XMIN_CONTROL, and XMIN_GL_LIBRARY can override individual build
artifacts. The launched command receives XMIN_CONTROL pointing at xminctl.
EOF
}

die()
{
    printf '%s\n' "xminlaunch.sh: $*" >&2
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
        --screen)
            (($# >= 2)) || die "--screen requires WxHxD"
            screen=$2
            shift 2
            ;;
        --screen=*)
            screen=${1#*=}
            shift
            ;;
        --dpi)
            (($# >= 2)) || die "--dpi requires a value"
            dpi=$2
            shift 2
            ;;
        --dpi=*)
            dpi=${1#*=}
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
            break
            ;;
    esac
done

(($# > 0)) || die "no command specified"

server=${XMIN_SERVER:-${build_dir}/src/server/Xmin}
runner=${XMIN_RUNNER:-${build_dir}/src/launcher/xmin-run}
gl_library=${XMIN_GL_LIBRARY:-${build_dir}/src/client/glx/libGL.so.1}
control=${XMIN_CONTROL:-${build_dir}/src/control/xminctl}

[[ -x "$server" ]] || die "Xmin server is not executable: $server"
[[ -x "$runner" ]] || die "Xmin launcher is not executable: $runner"
[[ -f "$gl_library" ]] || die "Xmin OpenGL library was not found: $gl_library"
[[ -x "$control" ]] || die "Xmin control client is not executable: $control"

gl_directory="$(cd -- "$(dirname -- "$gl_library")" && pwd -P)"
gl_library="${gl_directory}/$(basename -- "$gl_library")"

preload=$gl_library
if [[ -n ${LD_PRELOAD:-} ]]; then
    preload="${preload}:${LD_PRELOAD}"
fi

library_path=$gl_directory
if [[ -n ${LD_LIBRARY_PATH:-} ]]; then
    library_path="${library_path}:${LD_LIBRARY_PATH}"
fi

runner_args=(--server "$server")
if [[ -n "$screen" ]]; then
    runner_args+=(--screen "$screen")
fi
if [[ -n "$dpi" ]]; then
    runner_args+=(--dpi "$dpi")
fi

# Apply the loader overrides to the application child only. Preloading libGL
# into the Xmin server would collide with its separately embedded renderer.
exec "$runner" "${runner_args[@]}" -- \
    env XMIN_CONTROL="$control" LD_PRELOAD="$preload" \
        LD_LIBRARY_PATH="$library_path" "$@"
