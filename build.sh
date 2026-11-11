#!/usr/bin/env bash
# One entry point for every build in this repo. Run with no arguments for help.
#
#   ./build.sh test          host tests (no ESP-IDF needed)
#   ./build.sh fw -b demo    firmware for the demo board (-b bike for the PCB)
#   ./build.sh flash -b demo build, flash it as stable, open the monitor
#
# The port is found by itself (the Espressif board in /dev/serial/by-id);
# override with -p PORT or PORT=...
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
PORT="${PORT:-}"     # empty: find_port picks the board
BOARD=""            # bike | demo, from -b; firmware commands require it
JOBS="$(nproc 2>/dev/null || echo 4)"

say()  { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: ./build.sh <command> [-b demo|bike] [-p PORT]

Host (no ESP-IDF):
  test        build and run the host tests
  sim         build the simulator and open its window
  check -b B  host tests, then the firmware build

Firmware (ESP-IDF v6.0.2, sourced for you). Each needs -b:
  -b demo     the Guition 4.3" demo board (demo_p4/)
  -b bike     the CRF250F PCB (the repo root)
  A bike image on the demo board boots to a dark screen.

  fw          build the firmware, copied to releases/
  flash       build, flash as the new STABLE image, open the monitor
  dev         build, write it to the DEV slot only (stable left alone)
  monitor     open the serial monitor
  menuconfig  edit sdkconfig
  size        firmware size summary

  Every build bumps the version in firmware.txt and asks what's new;
  press Enter to rebuild the current version without a bump.

  flash and dev reset the board with its watchdog, not over USB, and the
  monitor opens without a reset: a USB reset can leave a P4 boot-looping
  with a dark screen. If it does, unplug it and plug it back in.

Housekeeping:
  clean       delete the host, sim, bike and demo build trees

  A running tools/pod_keys.py is paused for flash and dev and resumed after.

Port: ${PORT:-found by itself: the Espressif board in /dev/serial/by-id}
      (change with -p PORT or PORT=...)
EOF
}

# --- ESP-IDF ---------------------------------------------------------------

idf_env() {
    if ! command -v idf.py >/dev/null 2>&1; then
        [[ -f "$IDF_EXPORT" ]] || die "ESP-IDF not found at $IDF_EXPORT"
        say "Sourcing ESP-IDF v6.0.2"
        # shellcheck disable=SC1090
        . "$IDF_EXPORT" >/dev/null
    fi
    # The monitor decodes a crash with riscv32-esp-elf-gdb, which EIM's
    # export.sh leaves off PATH; without it the monitor dies on the first panic.
    if ! command -v riscv32-esp-elf-gdb >/dev/null 2>&1; then
        local gdb
        for gdb in "$HOME"/.espressif/tools/riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin; do
            [[ -x "$gdb/riscv32-esp-elf-gdb" ]] && PATH="$gdb:$PATH"
        done
    fi
}

# Every reset over USB-Serial/JTAG (esptool's hard-reset, the monitor's reset
# on open) can leave a P4 v1.x boot-looping in its ROM until it is unplugged
# (components/p4_restart_fix covers only the firmware's own resets). So the
# writes reset with the RTC watchdog instead, and the monitor opens without
# resetting.
flash_stable() {
    local b; b="$(project_dir)/build"
    say "Flashing $BOARD firmware as stable (watchdog reset after)"
    (cd "$b" && python3 -m esptool --chip esp32p4 -p "$PORT" -b 460800 \
        --before default-reset --after watchdog-reset write-flash "@flash_args")
}

# Where each board's project lives.
project_dir() { [[ "$BOARD" == demo ]] && echo "$ROOT/demo_p4" || echo "$ROOT"; }
idf()         { idf.py -C "$(project_dir)" "$@"; }

# A build tree configured by the other machine (left over from the Syncthing
# days) names a Python env that does not exist here, and idf.py refuses it.
# Build trees are per machine now, so it is safe to start that one afresh.
fix_foreign_build() {
    local dir="$1" cache="$1/CMakeCache.txt" py
    [[ -f "$cache" ]] || return 0
    py="$(grep -m1 '^PYTHON:' "$cache" | cut -d= -f2- || true)"
    if [[ -n "$py" && ! -x "$py" ]]; then
        say "$dir was configured on another machine ($py); cleaning it"
        rm -rf "$dir"
    fi
}

# The board's port. /dev/ttyACM0 is only the usual name: a board that resets
# while something (pod_keys.py, a monitor) holds its port comes back as
# ttyACM1. The by-id link names the board, whatever number it got.
find_port() {
    local ports=() p
    for p in /dev/serial/by-id/*Espressif*; do [[ -e "$p" ]] && ports+=("$p"); done
    if [[ ${#ports[@]} -eq 0 ]]; then
        for p in /dev/ttyACM*; do [[ -e "$p" ]] && ports+=("$p"); done
    fi
    case ${#ports[@]} in
        0) die "no board found (no Espressif port in /dev/serial/by-id, no /dev/ttyACM*). Is it plugged in, with a data cable?" ;;
        1) PORT="${ports[0]}" ;;
        *) die "more than one board: ${ports[*]}. Pick one with -p PORT" ;;
    esac
    say "Port: $PORT -> $(readlink -f "$PORT")"
}

need_port() {
    [[ -n "$PORT" ]] || find_port
    [[ -e "$PORT" ]] || die "$PORT not found. Is the board plugged in?"
}

# tools/pod_keys.py holds the port; SIGUSR1 lets go of it, SIGUSR2 takes it
# back (docs/console.md). POD_PAUSED=1 when one was running and is now paused.
POD_PID_FILE="${XDG_RUNTIME_DIR:-/tmp}/pod_keys.pid"
POD_PAUSED=0
pod_keys_pause() {
    local pid
    pid="$(cat "$POD_PID_FILE" 2>/dev/null)" || return 0
    # A crashed pod_keys.py leaves its pid file; SIGUSR1 would kill whatever reused the pid.
    grep -qa pod_keys.py "/proc/$pid/cmdline" 2>/dev/null || return 0
    kill -USR1 "$pid" 2>/dev/null || return 0
    POD_PAUSED=1
    say "Paused pod_keys.py ($pid) for the flash"
    sleep 0.5   # let it close the port
}
pod_keys_resume() {
    [[ $POD_PAUSED -eq 1 ]] || return 0
    kill -USR2 "$(cat "$POD_PID_FILE")" 2>/dev/null && say "Resumed pod_keys.py; its window shows the log"
}

need_board() {
    [[ -n "$BOARD" ]] || die "say which board: -b demo (the Guition 4.3\" board) or -b bike (the CRF250F PCB)"
}

# Bump firmware.txt's version (1.15 -> 1.16) and ask what is new. A blank
# answer rebuilds the current version unchanged, for a quick re-flash.
# Not asked twice in one run, so `check` and `flash` prompt once.
BUMPED=0
bump_version() {
    [[ $BUMPED -eq 1 ]] && return 0
    BUMPED=1
    local cur feat major minor next new full
    cur="$(awk '$1 == "version" {print $2}' firmware.txt)"
    feat="$(sed -n 's/^feature[[:space:]]\+//p' firmware.txt)"
    [[ "$cur" =~ ^([0-9]+)\.([0-9]+)$ ]] || die "firmware.txt: can't parse version '$cur'"
    major="${BASH_REMATCH[1]}"; minor="${BASH_REMATCH[2]}"
    next="$major.$((minor + 1))"

    if [[ ! -t 0 ]]; then
        say "Not a terminal: building $cur ($feat) without a bump"
        return 0
    fi
    echo "Current: $cur ($feat)"
    read -r -p "What's new in $next? 1-3 words (blank = rebuild $cur as is): " new
    new="$(echo "$new" | tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//')"
    if [[ -z "$new" ]]; then
        say "Keeping $cur ($feat)"
        return 0
    fi
    [[ $(wc -w <<<"$new") -le 3 ]] || die "keep the feature to three words or fewer"
    [[ "$new" =~ ^[A-Za-z0-9\ ._-]+$ ]] || die "feature may use letters, digits, space . _ - only"
    # The image holds "<version> <board> <feature>" in 31 characters
    # (cmake/dash_version.cmake); "bike" and "demo" are the same length.
    full="$next bike $new"
    [[ ${#full} -le 31 ]] || die "'$full' is ${#full} characters; the image holds 31. Shorter, please."
    sed -i "s/^version[[:space:]].*/version $next/; s/^feature[[:space:]].*/feature $new/" firmware.txt
    say "firmware.txt: $cur -> $next ($new)"
}

fw_build() {
    need_board
    idf_env
    fix_foreign_build "$(project_dir)/build"
    bump_version
    say "Building $BOARD firmware ($(grep -E '^(version|feature)' firmware.txt | tr '\n' ' '))"
    idf build
}

# --- host ------------------------------------------------------------------

host_test() {
    say "Host tests"
    cmake -S test -B build-host >/dev/null
    cmake --build build-host -j "$JOBS"
    ctest --test-dir build-host --output-on-failure -j "$JOBS"
}

sim_run() {
    say "Simulator"
    cmake -S sim -B build-sim >/dev/null
    cmake --build build-sim -j "$JOBS"
    python3 sim/dash_sim.py
}

# --- arguments -------------------------------------------------------------

cmd="${1:-}"
[[ $# -gt 0 ]] && shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)  PORT="${2:?-p needs a port}"; shift 2 ;;
        -b|--board) BOARD="${2:?-b needs bike or demo}"; shift 2
                    [[ "$BOARD" == bike || "$BOARD" == demo ]] || die "-b is bike or demo" ;;
        *) die "unknown option: $1 (try ./build.sh help)" ;;
    esac
done

case "$cmd" in
    test)       host_test ;;
    sim)        sim_run ;;
    check)      need_board; host_test; fw_build ;;
    fw|build)   fw_build ;;
    flash)      need_board; need_port; fw_build; pod_keys_pause
                trap pod_keys_resume EXIT
                flash_stable
                # pod_keys.py already scrolls the log; a monitor would fight it for the port.
                [[ $POD_PAUSED -eq 1 ]] || idf -p "$PORT" monitor --no-reset ;;
    dev)        need_board; need_port; fw_build; pod_keys_pause
                trap pod_keys_resume EXIT
                python3 tools/flash_dev.py -p "$PORT" "$(project_dir)/build/dash.bin" ;;
    monitor)    need_board; idf_env; need_port; idf -p "$PORT" monitor --no-reset ;;
    menuconfig) need_board; idf_env; fix_foreign_build "$(project_dir)/build"
                idf menuconfig ;;
    size)       need_board; idf_env; idf size ;;
    clean)      say "Removing build-host, build-sim, build, demo_p4/build"
                rm -rf build-host build-sim build demo_p4/build ;;
    ""|help|-h|--help) usage ;;
    *)          usage; die "unknown command: $cmd" ;;
esac
