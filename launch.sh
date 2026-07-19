#!/bin/sh

set -eu

usage()
{
    cat <<'EOF'
usage: ./launch.sh [OPTIONS]

Launch an Xmin desktop session and attach the separate GLFW host viewer.

Options:
  --build-dir DIR   build tree containing bin/xmin-session and bin/xmin-viewer
  --screen WxHxD    override the Xmin screen geometry
  --fps RATE        set the viewer capture rate (1-240)
  --no-shm          force the viewer's portable tiled GetImage path
  -h, --help        show this help

The build directory defaults to $XMIN_BUILD_DIR, then ./.build when present,
then ./build/dev. Closing the viewer or interrupting this script also stops
the session and removes its temporary descriptor.
EOF
}

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
build_dir=${XMIN_BUILD_DIR:-}
screen=${XMIN_SCREEN:-}
frames_per_second=
no_shm=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            if [ "$#" -lt 2 ]; then
                echo "launch.sh: --build-dir requires a directory" >&2
                exit 2
            fi
            build_dir=$2
            shift 2
            ;;
        --screen)
            if [ "$#" -lt 2 ]; then
                echo "launch.sh: --screen requires WxHxD" >&2
                exit 2
            fi
            screen=$2
            shift 2
            ;;
        --fps)
            if [ "$#" -lt 2 ]; then
                echo "launch.sh: --fps requires a rate" >&2
                exit 2
            fi
            frames_per_second=$2
            shift 2
            ;;
        --no-shm)
            no_shm=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "launch.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$build_dir" ]; then
    if [ -x "$script_dir/.build/bin/xmin-session" ] &&
       [ -x "$script_dir/.build/bin/xmin-viewer" ]; then
        build_dir=$script_dir/.build
    elif [ -x "$script_dir/build/dev/bin/xmin-session" ] &&
         [ -x "$script_dir/build/dev/bin/xmin-viewer" ]; then
        build_dir=$script_dir/build/dev
    else
        build_dir=$script_dir/.build
    fi
elif [ "${build_dir#/}" = "$build_dir" ]; then
    build_dir=$script_dir/$build_dir
fi

session=$build_dir/bin/xmin-session
viewer=$build_dir/bin/xmin-viewer
if [ ! -x "$session" ] || [ ! -x "$viewer" ]; then
    echo "launch.sh: the full desktop is not built in $build_dir" >&2
    echo "launch.sh: configure with the viewer and desktop enabled, then build it" >&2
    exit 1
fi

temporary_root=${TMPDIR:-/tmp}
session_info=$(mktemp "$temporary_root/xmin-launch-XXXXXX")
session_pid=

cleanup()
{
    trap - EXIT HUP INT TERM
    if [ -n "$session_pid" ]; then
        kill -TERM "$session_pid" 2>/dev/null || :
        wait "$session_pid" 2>/dev/null || :
        session_pid=
    fi
    rm -f "$session_info"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

echo "launch.sh: starting the Xmin desktop from $build_dir"
if [ -n "$screen" ]; then
    "$session" --screen "$screen" --session-info "$session_info" &
else
    "$session" --session-info "$session_info" &
fi
session_pid=$!

attempt=0
while [ ! -s "$session_info" ]; do
    if ! kill -0 "$session_pid" 2>/dev/null; then
        set +e
        wait "$session_pid"
        session_status=$?
        set -e
        session_pid=
        if [ "$session_status" -eq 0 ]; then
            session_status=1
        fi
        echo "launch.sh: the desktop session exited before becoming ready" >&2
        exit "$session_status"
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 200 ]; then
        echo "launch.sh: timed out waiting for the desktop session" >&2
        exit 1
    fi
    sleep 0.05
done

set -- --session-info "$session_info"
if [ "$no_shm" -ne 0 ]; then
    set -- "$@" --no-shm
fi
if [ -n "$frames_per_second" ]; then
    set -- "$@" --fps "$frames_per_second"
fi

echo "launch.sh: attaching the GLFW viewer"
set +e
"$viewer" "$@"
viewer_status=$?
set -e
exit "$viewer_status"
