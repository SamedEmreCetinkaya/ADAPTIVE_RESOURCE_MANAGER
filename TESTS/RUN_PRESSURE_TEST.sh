#!/bin/sh
# Memory pressure beyond physical memory, to run after ENABLE_ENFORCE.sh. Needs no root.
# Fills 140% of memory and keeps touching it. The test process volunteers as the first OOM victim.
# Usage: sh TESTS/RUN_PRESSURE_TEST.sh [PERCENT_OF_MEMORY] [SECONDS]
set -u
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
HOLDER="$ROOT/BUILD/memory-holder"
[ -x "$HOLDER" ] || { echo "Build first: make"; exit 1; }
PERCENT=${1:-140}
SECONDS_TO_HOLD=${2:-60}
MEMORY_MIB=$(awk '/^MemTotal/{print int($2/1024)}' /proc/meminfo)
AVAILABLE_MIB=$(awk '/^MemAvailable/{print int($2/1024)}' /proc/meminfo)
if [ "$AVAILABLE_MIB" -lt $(( MEMORY_MIB * 40 / 100 )) ]; then
    echo "Needs 40% of memory available, has $AVAILABLE_MIB MiB of $MEMORY_MIB. Nothing started."
    exit 1
fi
SIZE_MIB=$(( MEMORY_MIB * PERCENT / 100 ))
mkdir -p "$ROOT/TESTS/RESULTS"
LOG="$ROOT/TESTS/RESULTS/PRESSURE_TEST_$(date +%Y%m%d_%H%M%S).log"
OOM_BEFORE=$(awk '/^oom_kill /{print $2}' /proc/vmstat)

# Written and synced every second so the last state survives a hard lock.
sample() {
    while :; do
        {
            printf '%s available_mib=%s zram_used_mib=%s disk_swap_used_mib=%s swap_file_sizes_mib=%s memory_pressure="%s" io_pressure="%s"\n' \
                "$(date +%T)" \
                "$(awk '/^MemAvailable/{print int($2/1024)}' /proc/meminfo)" \
                "$(awk '$1 ~ /zram/{used+=$4} END{print int(used/1024)}' /proc/swaps)" \
                "$(awk '$2=="file"{used+=$4} END{print int(used/1024)}' /proc/swaps)" \
                "$(awk '$2=="file"{printf "%s%d", separator, $3/1024; separator=","}' /proc/swaps)" \
                "$(sed -n 's/^full avg10=\([0-9.]*\).*/\1/p' /proc/pressure/memory)" \
                "$(sed -n 's/^full avg10=\([0-9.]*\).*/\1/p' /proc/pressure/io)"
        } >> "$LOG"
        sync "$LOG"
        sleep 1
    done
}
sample &
SAMPLER=$!
trap 'kill "$SAMPLER" 2>/dev/null' EXIT INT TERM HUP

echo "Filling $SIZE_MIB MiB ($PERCENT% of memory), then holding for $SECONDS_TO_HOLD s. Log: $LOG"
sh -c 'echo 1000 > /proc/self/oom_score_adj; exec "$@"' sh "$HOLDER" pressure "$SIZE_MIB" "$SECONDS_TO_HOLD" | tee -a "$LOG"
HOLDER_STATUS=$?
kill "$SAMPLER" 2>/dev/null
trap - EXIT INT TERM HUP

OOM_AFTER=$(awk '/^oom_kill /{print $2}' /proc/vmstat)
DISK_PEAK=$(sed -n 's/.*disk_swap_used_mib=\([0-9]*\).*/\1/p' "$LOG" | sort -n | tail -1)
ZRAM_PEAK=$(sed -n 's/.*zram_used_mib=\([0-9]*\).*/\1/p' "$LOG" | sort -n | tail -1)
FILE_SIZES=$(sed -n 's/.*swap_file_sizes_mib=\([0-9,]*\) .*/\1/p' "$LOG" | awk -F, 'NF > most {most = NF; sizes = $0} END{print sizes}')
{
    echo "zram_peak_mib=${ZRAM_PEAK:-0} disk_swap_peak_mib=${DISK_PEAK:-0} swap_file_sizes_at_peak_mib=${FILE_SIZES:-NONE} oom_kills=$(( OOM_AFTER - OOM_BEFORE ))"
    if [ "$OOM_AFTER" = "$OOM_BEFORE" ] && grep -q PRESSURE_FINISHED "$LOG"; then echo PRESSURE_TEST_PASSED; else echo PRESSURE_TEST_FAILED; fi
} | tee -a "$LOG"
[ "$OOM_AFTER" = "$OOM_BEFORE" ] && exit "$HOLDER_STATUS"
exit 1
