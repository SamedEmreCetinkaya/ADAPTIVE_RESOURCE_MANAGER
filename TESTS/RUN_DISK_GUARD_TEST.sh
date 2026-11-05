#!/bin/sh
# Real test of the disk guard with the real swapspace. The free disk limit is set above the real free
# space, so swap growth must stop while memory runs short. No disk is actually filled.
# Usage: sudo sh TESTS/RUN_DISK_GUARD_TEST.sh [PERCENT_OF_MEMORY]
set -u
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0"; exit 1; }
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
DAEMON="$ROOT/BUILD/adaptive-resource-managerd"
[ -x "$DAEMON" ] && [ -x "$ROOT/BUILD/memory-holder" ] || { echo "Build first: make"; exit 1; }
USER_ID=${SUDO_UID:-}
[ -n "$USER_ID" ] || { echo "Run through sudo so the test can use your user ID."; exit 1; }
POLICY=/etc/adaptive-resource-manager/RESOURCE_POLICY.conf
[ -f "$POLICY" ] || { echo "Run INSTALL.sh first."; exit 1; }
swap_file_count() { awk 'NR>1 && $2=="file"' /proc/swaps | wc -l; }
[ "$(swap_file_count)" = 0 ] || { echo "Swap files exist. Wait until they are gone. Nothing changed."; exit 1; }
PERCENT=${1:-110}
SERVICE=adaptive-resource-manager.service
SERVICE_WAS_ACTIVE=0
DAEMON_PID=""
SAMPLER_PID=""
WORK=$(mktemp -d)
chmod 755 "$WORK"

cleanup() {
    trap - EXIT INT TERM HUP
    [ -n "$SAMPLER_PID" ] && kill "$SAMPLER_PID" 2>/dev/null
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
    # swapspace would otherwise keep refusing new files until its cooldown ends.
    systemctl restart swapspace.service
    [ "$SERVICE_WAS_ACTIVE" = 1 ] && systemctl start "$SERVICE"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

install -d -m 755 "$WORK/run"
{
    grep -vE '^(RUN_MODE|SWAP_FILE_MIN_FREE_DISK_PERCENT)=' "$POLICY"
    echo RUN_MODE=ENFORCE
    echo SWAP_FILE_MIN_FREE_DISK_PERCENT=100
} > "$WORK/test.conf"
chmod 644 "$WORK/test.conf"
if systemctl is-active --quiet "$SERVICE"; then
    systemctl stop "$SERVICE"
    SERVICE_WAS_ACTIVE=1
fi
"$DAEMON" --config "$WORK/test.conf" --runtime-dir "$WORK/run" > "$WORK/daemon.log" 2>&1 &
DAEMON_PID=$!

# The share swapspace compares with its lower limit, using its documented meaning of free space.
sample() {
    while :; do
        echo "$(swap_file_count) $(awk '/^MemFree:/{f+=$2} /^SwapFree:/{f+=$2} /^Buffers:/{f+=$2*0.3} /^Cached:/{f+=$2*0.8} /^MemTotal:/{t+=$2} /^SwapTotal:/{t+=$2} END{printf "%d", f*100/t}' /proc/meminfo)" >> "$WORK/samples"
        sleep 1
    done
}
sample &
SAMPLER_PID=$!

setpriv --reuid="$USER_ID" --regid="${SUDO_GID:-$USER_ID}" --clear-groups sh "$ROOT/TESTS/RUN_PRESSURE_TEST.sh" "$PERCENT" 5
kill "$SAMPLER_PID" 2>/dev/null
SAMPLER_PID=""

PEAK_FILES=$(awk '$1>m{m=$1} END{print m+0}' "$WORK/samples")
LOWEST_FREE=$(awk 'NR==1||$2<m{m=$2} END{print m+0}' "$WORK/samples")
LOWER_LIMIT=$(sed -n 's/^lower_freelimit=//p' /run/adaptive-resource-manager-tunables/swapspace.conf 2>/dev/null)
grep -E 'event=SWAP_GROWTH' "$WORK/daemon.log"
echo "swap_files_peak=$PEAK_FILES lowest_free_percent=$LOWEST_FREE swapspace_lower_limit=${LOWER_LIMIT:-20}"
if [ "$PEAK_FILES" != 0 ]; then
    echo "DISK_GUARD_TEST_FAILED: a swap file was opened"
    exit 1
fi
if ! grep -q 'event=SWAP_GROWTH_HELD' "$WORK/daemon.log" || [ "$LOWEST_FREE" -ge "${LOWER_LIMIT:-20}" ]; then
    echo "DISK_GUARD_TEST_INCONCLUSIVE: swapspace never wanted a file. Try a larger percent."
    exit 2
fi
echo DISK_GUARD_TEST_PASSED
