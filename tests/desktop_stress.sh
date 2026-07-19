#!/bin/sh

set -eu

usage()
{
    cat <<'EOF'
usage: tests/desktop_stress.sh [OPTIONS]

Run a seeded Xmin/JWM/st/GLFW lifecycle stress test on the current host DISPLAY.
Use xvfb-run -a when no graphical display is available.

Options:
  --build-dir DIR   build tree (default: .build, then build/dev)
  --iterations N    guest operation count (default: 500)
  --host-iterations N
                    GLFW host-window resize count (default: iterations)
  --lifecycle N     temporary terminal create/close cycles (default: 8)
  --reattach N      viewer detach/reattach cycles (default: 2)
  --seed N          reproducible random seed (default: 1)
  --delay MS        pause between guest operations (default: 0)
  --resize-only     use the original guest resize-only workload
  --no-host-resize  do not resize the GLFW host window
  --no-shm          test the viewer's portable tiled GetImage path
  -h, --help        show this help
EOF
}

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=
iterations=500
host_iterations=
lifecycle=8
reattach=2
seed=1
delay=0
no_shm=0
resize_only=0
host_resize=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            build_dir=$2
            shift 2
            ;;
        --iterations)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            iterations=$2
            shift 2
            ;;
        --host-iterations)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            host_iterations=$2
            shift 2
            ;;
        --lifecycle)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            lifecycle=$2
            shift 2
            ;;
        --reattach)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            reattach=$2
            shift 2
            ;;
        --seed)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            seed=$2
            shift 2
            ;;
        --delay)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            delay=$2
            shift 2
            ;;
        --resize-only)
            resize_only=1
            shift
            ;;
        --no-host-resize)
            host_resize=0
            shift
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
            echo "desktop_stress.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$host_iterations" ]; then
    host_iterations=$iterations
fi

if [ -z "${DISPLAY:-}" ]; then
    echo "desktop_stress.sh: DISPLAY is unset; use xvfb-run -a" >&2
    exit 2
fi
if [ -z "$build_dir" ]; then
    if [ -x "$source_dir/.build/bin/xmin-session" ]; then
        build_dir=$source_dir/.build
    else
        build_dir=$source_dir/build/dev
    fi
elif [ "${build_dir#/}" = "$build_dir" ]; then
    build_dir=$source_dir/$build_dir
fi

session=$build_dir/bin/xmin-session
viewer=$build_dir/bin/xmin-viewer
terminal=$build_dir/bin/xmin-st
control=$build_dir/bin/xminctl
feedback_target=$build_dir/bin/xminctl_target
for program in "$session" "$viewer" "$terminal" "$control" \
    "$feedback_target"; do
    if [ ! -x "$program" ]; then
        echo "desktop_stress.sh: missing executable: $program" >&2
        exit 1
    fi
done

temporary_root=${TMPDIR:-/tmp}
session_info=$(mktemp "$temporary_root/xmin-desktop-stress-XXXXXX")
capture=$session_info.ppm
viewer_before=$session_info-viewer-before.ppm
viewer_after=$session_info-viewer-after.ppm
guest_before=$session_info-guest-before.ppm
guest_after=$session_info-guest-after.ppm
session_pid=
viewer_pid=
terminal_pid=
terminal_window=
feedback_pid=
feedback_window=
host_stress_pid=
lifecycle_pid=

cleanup()
{
    trap - EXIT HUP INT TERM
    for pid in "$host_stress_pid" "$lifecycle_pid" "$feedback_pid" \
        "$viewer_pid" "$terminal_pid" "$session_pid"; do
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null || :
        fi
    done
    for pid in "$host_stress_pid" "$lifecycle_pid" "$feedback_pid" \
        "$viewer_pid" "$terminal_pid" "$session_pid"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || :
        fi
    done
    rm -f "$session_info" "$capture" "$viewer_before" "$viewer_after" \
        "$guest_before" "$guest_after"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

start_viewer()
{
    if [ "$no_shm" -ne 0 ]; then
        "$viewer" --session-info "$session_info" --no-shm &
    else
        "$viewer" --session-info "$session_info" &
    fi
    viewer_pid=$!
    "$control" wait-window --timeout 5000 "Xmin Viewer" >/dev/null
    if ! kill -0 "$viewer_pid" 2>/dev/null; then
        echo "desktop_stress.sh: viewer exited while attaching" >&2
        exit 1
    fi
}

stop_viewer()
{
    if [ -n "$viewer_pid" ]; then
        if ! "$control" close "Xmin Viewer" >/dev/null 2>&1; then
            kill -TERM "$viewer_pid" 2>/dev/null || :
        fi
        attempt=0
        while kill -0 "$viewer_pid" 2>/dev/null; do
            attempt=$((attempt + 1))
            if [ "$attempt" -ge 100 ]; then
                kill -TERM "$viewer_pid" 2>/dev/null || :
                break
            fi
            sleep 0.05
        done
        wait "$viewer_pid" 2>/dev/null || :
        viewer_pid=
    fi
}

PATH=$build_dir/bin:$PATH
export PATH
"$session" --session-info "$session_info" &
session_pid=$!

attempt=0
while [ ! -s "$session_info" ]; do
    if ! kill -0 "$session_pid" 2>/dev/null; then
        echo "desktop_stress.sh: session exited before becoming ready" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 200 ]; then
        echo "desktop_stress.sh: session startup timed out" >&2
        exit 1
    fi
    sleep 0.05
done

XMIN_DISPLAY=$(sed -n 's/^XMIN_DISPLAY=//p' "$session_info")
XMIN_XAUTHORITY=$(sed -n 's/^XMIN_XAUTHORITY=//p' "$session_info")
if [ -z "$XMIN_DISPLAY" ] || [ -z "$XMIN_XAUTHORITY" ]; then
    echo "desktop_stress.sh: invalid session descriptor" >&2
    exit 1
fi

start_viewer

DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$terminal" -t xmin-st-stress &
terminal_pid=$!
terminal_window=$(DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" wait-window --timeout 5000 xmin-st-stress)

if ! kill -0 "$viewer_pid" 2>/dev/null; then
    echo "desktop_stress.sh: viewer exited before stress began" >&2
    exit 1
fi

# Exercise the path a person uses: the viewer is already attached when st is
# created.  A deterministic XCB target repaints on each KeyPress so GLFW input,
# guest DAMAGE, and successive host frames can be asserted independently of a
# shell's PTY echo timing.  Capturing only the guest would miss a viewer stuck
# on its initial MapNotify frame.
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$feedback_target" --feedback &
feedback_pid=$!
feedback_window=$(DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" wait-window --timeout 5000 xmin-viewer-feedback-target)
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" activate "$feedback_window"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" wait-stable --quiet 50 --timeout 5000 "$feedback_window"
"$control" activate "Xmin Viewer"
"$control" wait-stable --quiet 50 --timeout 5000 "Xmin Viewer"
sleep 0.1
"$control" capture-window "Xmin Viewer" "$viewer_before"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" capture-window "$feedback_window" "$guest_before"
feedback_source=host
for feedback_key in x m i n f e e x; do
    if [ "$feedback_source" = host ]; then
        "$control" type "$feedback_key"
        feedback_source=guest
    else
        DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
            "$control" type "$feedback_key"
    fi
    attempt=0
    while :; do
        DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
            "$control" capture-window "$feedback_window" "$guest_after"
        if ! cmp -s "$guest_before" "$guest_after"; then
            break
        fi
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 50 ]; then
            echo "desktop_stress.sh: feedback key '$feedback_key' did not repaint the guest" >&2
            exit 1
        fi
        sleep 0.02
    done
    attempt=0
    while :; do
        "$control" capture-window "Xmin Viewer" "$viewer_after"
        if ! cmp -s "$viewer_before" "$viewer_after"; then
            break
        fi
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 50 ]; then
            echo "desktop_stress.sh: viewer missed a successive feedback repaint" >&2
            exit 1
        fi
        sleep 0.02
    done
    cp "$guest_after" "$guest_before"
    cp "$viewer_after" "$viewer_before"
done
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" close "$feedback_window"
if ! wait "$feedback_pid"; then
    echo "desktop_stress.sh: feedback target exited unsuccessfully" >&2
    feedback_pid=
    exit 1
fi
feedback_pid=

if [ "$host_resize" -ne 0 ] && [ "$host_iterations" -gt 0 ]; then
    "$control" stress-resize --iterations "$host_iterations" \
        --seed "$seed" "Xmin Viewer" &
    host_stress_pid=$!
fi

guest_stress=stress-window
if [ "$resize_only" -ne 0 ]; then
    guest_stress=stress-resize
fi
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" "$guest_stress" --iterations "$iterations" --seed "$seed" \
        --delay "$delay" "$terminal_window"

if [ -n "$host_stress_pid" ]; then
    if ! wait "$host_stress_pid"; then
        echo "desktop_stress.sh: GLFW host-window resize stress failed" >&2
        host_stress_pid=
        exit 1
    fi
    host_stress_pid=
fi

cycle=0
while [ "$cycle" -lt "$lifecycle" ]; do
    cycle=$((cycle + 1))
    title=xmin-st-lifecycle-$cycle
    DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
        "$terminal" -t "$title" &
    lifecycle_pid=$!
    DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
        "$control" wait-window --timeout 5000 "$title" >/dev/null
    DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
        "$control" close "$title"
    attempt=0
    while kill -0 "$lifecycle_pid" 2>/dev/null; do
        attempt=$((attempt + 1))
        if [ "$attempt" -ge 100 ]; then
            echo "desktop_stress.sh: terminal $title did not close" >&2
            exit 1
        fi
        sleep 0.05
    done
    if ! wait "$lifecycle_pid"; then
        echo "desktop_stress.sh: terminal $title exited unsuccessfully" >&2
        lifecycle_pid=
        exit 1
    fi
    lifecycle_pid=
done

cycle=0
while [ "$cycle" -lt "$reattach" ]; do
    cycle=$((cycle + 1))
    stop_viewer
    start_viewer
    "$control" geometry "Xmin Viewer" >/dev/null
    DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
        "$control" capture-root "$capture"
done

DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" map "$terminal_window"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" activate "$terminal_window"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" type "printf XMIN_STRESS_OK"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" key enter
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" wait-stable --quiet 50 --timeout 5000 "$terminal_window"
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" capture-root "$capture"

if ! kill -0 "$viewer_pid" 2>/dev/null; then
    echo "desktop_stress.sh: viewer exited during resize stress" >&2
    exit 1
fi
DISPLAY=$XMIN_DISPLAY XAUTHORITY=$XMIN_XAUTHORITY \
    "$control" geometry "$terminal_window" >/dev/null
echo "desktop_stress.sh: passed seed=$seed guest=$iterations "\
"host=$host_iterations lifecycle=$lifecycle reattach=$reattach"
