#!/bin/sh
# Tests that need no root and touch no real system setting. ENFORCE runs against a fake cgroup tree.
set -u
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
BINARY="$ROOT/BUILD/adaptive-resource-managerd-sanitized"
WORK=$(mktemp -d)
FAILURES=0
DAEMON_PID=""

cleanup() {
    [ -n "$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

check() {
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: expected '$3', got '$2'"
        FAILURES=$((FAILURES + 1))
    fi
}

check_contains() {
    if grep -q "$2" "$3"; then
        echo "PASS $1"
    else
        echo "FAIL $1: '$2' not found in $3"
        FAILURES=$((FAILURES + 1))
    fi
}

send_focus() {
    python3 -I -c 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); s.sendto(sys.argv[2].encode(),sys.argv[1])' "$WORK/run/focus.sock" "$1"
}

wait_for() {
    i=0
    while ! grep -q "$1" "$2" 2>/dev/null && [ $i -lt 80 ]; do sleep 0.1; i=$((i + 1)); done
}

GIB=1073741824
MEMORY_BYTES=$(awk '/^MemTotal/{print $2 * 1024}' /proc/meminfo)
percent() { echo $(( MEMORY_BYTES / 10000 * $1 * 100 + MEMORY_BYTES % 10000 * $1 * 100 / 10000 )); }

echo "== Configuration"
"$BINARY" --probe --config "$ROOT/CONFIG/RESOURCE_POLICY.conf" > "$WORK/probe" 2>&1
check "probe exit status" "$?" 0
check_contains "probe reports CPU classes" 'CPU_PERFORMANCE=' "$WORK/probe"
check_contains "probe reports GPU state" 'GPU_MEMORY_CGROUP_CONTROL=' "$WORK/probe"
"$BINARY" --print-policy --config "$ROOT/CONFIG/RESOURCE_POLICY.conf" > "$WORK/policy" 2>&1
check "shipped policy loads" "$?" 0
check_contains "shipped policy observes" 'RUN_MODE=OBSERVE' "$WORK/policy"
echo 'FOREGROUND_CPU_SET=LEAST_BUSY:12.50' > "$WORK/decimal.conf"
"$BINARY" --print-policy --config "$WORK/decimal.conf" > "$WORK/decimal" 2>&1
check_contains "accepts two decimals in selectors" 'LEAST_BUSY:12.50' "$WORK/decimal"
check "growth margin matches the largest swap file" "$(sed -n 's/^SWAP_FILE_GROWTH_MARGIN_PERCENT_OF_MEMORY=//p' "$ROOT/CONFIG/RESOURCE_POLICY.conf")" "$(sed -n 's/^SWAP_FILE_MAX_PERCENT=//p' "$ROOT/CONFIG/SYSTEM_TUNABLES.conf")"

echo "== Policy works together on 8, 16, 24 and 32 GB"
# Usable memory is about 1 GiB below the nominal size. Amounts in MiB, measured on the development machine:
# hot desktop and kernel 1434, protected clean cache 15% (vm.clean_min_ratio on kernels that have it), foreground hot set 2150,
# background build 3687, zram compression 2:1, and call time 5 ms at 16 MiB chunks.
for NOMINAL in 8 16 24 32; do
    USABLE=$((NOMINAL - 1))
    "$BINARY" --print-policy --config "$ROOT/CONFIG/RESOURCE_POLICY.conf" --assume-memory-gib "$USABLE" > "$WORK/size" 2>&1
    value() { sed -n "s/^$1_MIB=//p; s/.* $1_MIB=//p" "$WORK/size" | head -1; }
    TOTAL=$(value MEMORY_TOTAL); [ -n "$TOTAL" ] || TOTAL=$(sed -n 's/^MEMORY_TOTAL_MIB=//p' "$WORK/size")
    HIGH=$(value FOREGROUND_MEMORY_HIGH); LOW=$(value FOREGROUND_MEMORY_LOW); BACKGROUND=$(value BACKGROUND_MEMORY_HIGH)
    RESERVE=$(value FOREGROUND_READBACK_RESERVE); IDLE=$(value IDLE_READBACK_RESERVE); CHUNK=$(value READBACK_MAX_CHUNK)
    UNREACHABLE=$(value SWAP_FILE_RETIRE_UNREACHABLE)
    CLEAN=$((TOTAL * 15 / 100))
    ZRAM=$(awk -v t="$TOTAL" -v p="$(sed -n 's/^ZRAM_SIZE_PERCENT=//p' "$ROOT/CONFIG/SYSTEM_TUNABLES.conf")" 'BEGIN{printf "%d", t * p / 100}')
    check "$NOMINAL GB foreground leaves room for desktop and clean cache" "$([ $((TOTAL - HIGH)) -ge $((1434 + CLEAN)) ] && echo yes)" yes
    check "$NOMINAL GB foreground hot set is protected" "$([ "$LOW" -ge 2048 ] && echo yes)" yes
    check "$NOMINAL GB background build nearly fits" "$([ "$BACKGROUND" -ge 3687 ] && echo yes)" yes
    check "$NOMINAL GB foreground outranks background" "$([ "$HIGH" -gt "$BACKGROUND" ] && echo yes)" yes
    check "$NOMINAL GB readback stops above protected cache" "$([ "$RESERVE" -ge $((CLEAN + 256)) ] && echo yes)" yes
    check "$NOMINAL GB idle reserve exceeds foreground reserve" "$([ "$IDLE" -gt "$RESERVE" ] && echo yes)" yes
    check "$NOMINAL GB full zram leaves working memory" "$([ $((ZRAM / 2)) -le $((TOTAL - 1434 - CLEAN - LOW)) ] && echo yes)" yes
    check "$NOMINAL GB read chunk stays short" "$([ "$CHUNK" -le 16 ] && echo yes)" yes
    check "$NOMINAL GB final swapoff stays short" "$([ "$UNREACHABLE" -le 350 ] && echo yes)" yes
    check "$NOMINAL GB worst case swapoff stays bounded" "$([ "$(value SWAP_FILE_RETIRE_WORST_CASE_READ)" -le 2048 ] && echo yes)" yes
    MIN_FREE=$(value SWAP_FILE_RETIRE_MIN_FREE_SWAP)
    LOWER_LIMIT=$(sed -n 's/^SWAP_LOWER_FREE_LIMIT_PERCENT=//p' "$ROOT/CONFIG/SYSTEM_TUNABLES.conf")
    check "$NOMINAL GB swapspace has time to add a file" "$([ "$MIN_FREE" -ge 2048 ] && echo yes)" yes
    check "$NOMINAL GB retiring a file does not ask for a new one" "$(awk -v free="$((IDLE + MIN_FREE))" -v total="$((TOTAL + ZRAM))" -v limit="$LOWER_LIMIT" 'BEGIN{print (free > total * limit / 100) ? "yes" : "no"}')" yes
    # Each new file adds the distance between the limits. The kernel allows about 23 swap files besides
    # zram, and 22 are counted here. The measured 140% pressure test used disk swap of 86% of memory.
    FREE_TARGET=$(sed -n 's/^SWAP_FREE_TARGET_PERCENT=//p' "$ROOT/CONFIG/SYSTEM_TUNABLES.conf")
    check "$NOMINAL GB swap files can reach the disk swap a heavy load needs" "$(awk -v start="$((TOTAL + ZRAM))" -v memory="$TOTAL" -v target="$FREE_TARGET" -v limit="$LOWER_LIMIT" 'BEGIN{step = (target - limit) / 100; print (start * ((1 + step) ^ 22 - 1) >= memory * 0.9) ? "yes" : "no"}')" yes
done
for BAD in 'FOREGROUND_MEMORY_HIGH_PERCENT=101' 'UNKNOWN_KEY=1' 'FOREGROUND_CPU_SET=FAST' 'FOREGROUND_CPU_SET=ALL:0' 'RUN_MODE=MAYBE' 'SWAP_FILE_DIRECTORY=relative' 'FOREGROUND_MEMORY_HIGH_PERCENT=abc' 'FOREGROUND_CPU_SET=LEAST_BUSY:0.00' 'SWAP_FILE_DIRECTORY=/var/lib/../tmp' 'SWAP_FILE_DIRECTORY=/var/lib/swapspace/'; do
    echo "$BAD" > "$WORK/bad.conf"
    "$BINARY" --print-policy --config "$WORK/bad.conf" > /dev/null 2>&1
    check "rejects $BAD" "$?" 2
done
printf 'FOREGROUND_MEMORY_HIGH_PERCENT=10\nFOREGROUND_MEMORY_LOW_PERCENT=20\n' > "$WORK/bad.conf"
"$BINARY" --print-policy --config "$WORK/bad.conf" > /dev/null 2>&1
check "rejects low above high" "$?" 2

# Fake cgroup tree: the real group name of this shell, plus one other application.
REAL_GROUP=$(sed -n 's/^0:://p' /proc/$$/cgroup)
case "$REAL_GROUP" in
    */app.slice/*) ;;
    *) REAL_GROUP="/user.slice/user-$(id -u).slice/user@$(id -u).service/app.slice/app-test-focus.scope" ;;
esac
GROUP_RELATIVE=$(echo "$REAL_GROUP" | sed -E 's|(/app\.slice/[^/]+).*|\1|')
APP_SLICE=$(echo "$GROUP_RELATIVE" | sed 's|/app\.slice/.*|/app.slice|')
FAKE="$WORK/cgroup"
FOCUS="$FAKE$GROUP_RELATIVE"
OTHER="$FAKE$APP_SLICE/app-other-background.scope"
mkdir -p "$FOCUS" "$OTHER" "$WORK/run" "$WORK/seats"
chmod 700 "$WORK/run"
echo "ACTIVE_UID=$(id -u)" > "$WORK/seats/seat0"
make_group() {
    echo 1000 > "$1/memory.current"; echo "$2" > "$1/memory.high"; echo "$3" > "$1/memory.max"
    echo "$4" > "$1/memory.swap.max"; echo 0 > "$1/memory.swap.current"; echo 0 > "$1/memory.low"
    echo "$5" > "$1/cpu.weight"; echo "$6" > "$1/cpu.max"
    : > "$1/cgroup.procs"; : > "$1/cgroup.threads"
    printf 'some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' > "$1/memory.pressure"
    printf 'some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n' > "$1/io.pressure"
}
make_group "$FOCUS" max max max 100 "max 100000"
make_group "$OTHER" 4294967296 6442450944 3221225472 40 "400000 100000"
ANCESTOR=$(dirname "$FOCUS")
while [ "$ANCESTOR" != "$FAKE" ]; do echo 0 > "$ANCESTOR/memory.low"; ANCESTOR=$(dirname "$ANCESTOR"); done
echo 'Filename Type Size Used Priority' > "$WORK/swaps"

read_value() { cat "$1"; }

echo "== OBSERVE changes nothing"
cat > "$WORK/observe.conf" <<EOF
RUN_MODE=OBSERVE
FOCUS_DEBOUNCE_MILLISECONDS=50
BACKGROUND_GRACE_SECONDS=1
EOF
"$BINARY" --config "$WORK/observe.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$WORK/observe.log" 2>&1 &
DAEMON_PID=$!
wait_for 'event=STARTED' "$WORK/observe.log"
sleep 0.3
send_focus $$
wait_for 'event=FOCUS_CHANGED' "$WORK/observe.log"
check_contains "focus change is logged" 'event=FOCUS_CHANGED.*mode=OBSERVE' "$WORK/observe.log"
wait_for 'event=FOCUS_STALL' "$WORK/observe.log"
check_contains "stall report attributes CPU and GPU" 'event=FOCUS_STALL.*cpu_stall_milliseconds=.*gpu_busy_percent=.*performance_cpu_mhz=' "$WORK/observe.log"
check "observe leaves memory.high" "$(read_value "$FOCUS/memory.high")" max
check "observe leaves cpu.weight" "$(read_value "$FOCUS/cpu.weight")" 100
check "observe leaves other group" "$(read_value "$OTHER/memory.high")" 4294967296
send_focus 999999999
send_focus 1
wait_for 'FOCUS_REPORT_REJECTED' "$WORK/observe.log"
check_contains "rejects a process of another user" 'FOCUS_REPORT_REJECTED pid=1 ' "$WORK/observe.log"
kill "$DAEMON_PID"; wait "$DAEMON_PID"
check "observe exit status" "$?" 0
DAEMON_PID=""
if grep -qE 'AddressSanitizer|runtime error' "$WORK/observe.log"; then echo "FAIL sanitizer in observe"; FAILURES=$((FAILURES + 1)); else echo "PASS no sanitizer report in observe"; fi

echo "== ENFORCE on the fake tree"
cat > "$WORK/enforce.conf" <<EOF
RUN_MODE=ENFORCE
FOCUS_DEBOUNCE_MILLISECONDS=50
BACKGROUND_GRACE_SECONDS=1
FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_ENABLED=0
FOREGROUND_MEMORY_HIGH_PERCENT=75.00
FOREGROUND_MEMORY_LOW_PERCENT=25.00
BACKGROUND_MEMORY_HIGH_PERCENT=50.00
FOREGROUND_CPU_WEIGHT_PERCENT=500.00
BACKGROUND_CPU_QUOTA_PERCENT=50.00
EOF
"$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$WORK/enforce.log" 2>&1 &
DAEMON_PID=$!
wait_for 'event=STARTED' "$WORK/enforce.log"
sleep 0.5
HIGH_50=$(percent 50); HIGH_75=$(percent 75); LOW_25=$(percent 25)
CPUS=$(nproc)
check "background gets relative memory.high" "$(read_value "$FOCUS/memory.high")" "$HIGH_50"
check "background keeps a stricter original" "$(read_value "$OTHER/memory.high")" 4294967296
check "background CPU quota scales with CPUs" "$(read_value "$FOCUS/cpu.max")" "$(( 100000 * CPUS / 2 )) 100000"
send_focus $$
wait_for 'event=FOCUS_CHANGED' "$WORK/enforce.log"
check "foreground gets relative memory.high" "$(read_value "$FOCUS/memory.high")" "$HIGH_75"
check "foreground gets relative memory.low" "$(read_value "$FOCUS/memory.low")" "$LOW_25"
check "foreground CPU weight is relative" "$(read_value "$FOCUS/cpu.weight")" 500
check "foreground lifts CPU quota" "$(read_value "$FOCUS/cpu.max")" max
check "protection reaches ancestors" "$(read_value "$(dirname "$FOCUS")/memory.low")" "$LOW_25"
sleep 0.2
echo "== Crash keeps originals"
kill -9 "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null
"$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$WORK/enforce2.log" 2>&1 &
DAEMON_PID=$!
wait_for 'event=STARTED' "$WORK/enforce2.log"
sleep 0.5
check_contains "originals are reloaded" 'event=ORIGINALS_RESTORED' "$WORK/enforce2.log"
kill "$DAEMON_PID"; wait "$DAEMON_PID"
check "enforce exit status" "$?" 0
DAEMON_PID=""
check "restore memory.high" "$(read_value "$FOCUS/memory.high")" max
check "restore other memory.high" "$(read_value "$OTHER/memory.high")" 4294967296
check "restore other memory.max" "$(read_value "$OTHER/memory.max")" 6442450944
check "restore other swap.max" "$(read_value "$OTHER/memory.swap.max")" 3221225472
check "restore cpu.weight" "$(read_value "$FOCUS/cpu.weight")" 100
check "restore other cpu.max" "$(read_value "$OTHER/cpu.max")" "400000 100000"
check "originals file removed" "$(ls "$WORK/run/ORIGINAL_VALUES" 2>/dev/null | wc -l)" 0
if grep -qE 'AddressSanitizer|runtime error' "$WORK/enforce.log" "$WORK/enforce2.log"; then echo "FAIL sanitizer in enforce"; FAILURES=$((FAILURES + 1)); else echo "PASS no sanitizer report in enforce"; fi

echo "== Demotion after focus leaves"
"$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$WORK/enforce3.log" 2>&1 &
DAEMON_PID=$!
wait_for 'event=STARTED' "$WORK/enforce3.log"
sleep 0.5
send_focus $$
wait_for 'event=FOCUS_CHANGED' "$WORK/enforce3.log"
check "foreground before moving away" "$(read_value "$FOCUS/memory.high")" "$HIGH_75"
wait_for 'FOREGROUND_CPU_WEIGHT=500' "$WORK/run/STATUS"
check_contains "status shows the foreground CPU weight" 'FOREGROUND_CPU_WEIGHT=500' "$WORK/run/STATUS"
mv "$FOCUS/cpu.weight" "$WORK/cpu.weight.saved"
wait_for 'FOREGROUND_CPU_WEIGHT=UNAVAILABLE' "$WORK/run/STATUS"
check_contains "status shows a missing CPU controller" 'FOREGROUND_CPU_WEIGHT=UNAVAILABLE' "$WORK/run/STATUS"
mv "$WORK/cpu.weight.saved" "$FOCUS/cpu.weight"
# A process outside app.slice clears the foreground: systemd --user runs in user@.service itself.
USER_MANAGER=$(pgrep -u "$(id -u)" -xo systemd)
send_focus "$USER_MANAGER"
wait_for 'event=FOCUS_CHANGED group=NONE' "$WORK/enforce3.log"
check_contains "focus outside app.slice clears foreground" 'event=FOCUS_CHANGED group=NONE' "$WORK/enforce3.log"
check "grace keeps the old limit" "$(read_value "$FOCUS/memory.high")" "$HIGH_75"
wait_for 'event=GROUP_DEMOTED' "$WORK/enforce3.log"
check_contains "old group is demoted" 'event=GROUP_DEMOTED' "$WORK/enforce3.log"
check "demotion applies background limit" "$(read_value "$FOCUS/memory.high")" "$HIGH_50"
check "demotion clears protection" "$(read_value "$FOCUS/memory.low")" 0
kill "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null
DAEMON_PID=""

echo "== Permission checks"
start_enforce_daemon() { # log
    "$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$1" 2>&1 &
    DAEMON_PID=$!
    wait_for 'event=STARTED' "$1"
    sleep 0.5
}
stop_enforce_daemon() { # log
    kill "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
    if grep -qE 'AddressSanitizer|runtime error' "$1"; then echo "FAIL sanitizer in $1"; FAILURES=$((FAILURES + 1)); fi
}
echo "ACTIVE_UID=$(( $(id -u) + 1 ))" > "$WORK/seats/seat0"
start_enforce_daemon "$WORK/seat.log"
check "saved limits are private" "$(stat -c %a "$WORK/run/ORIGINAL_VALUES")" 600
check "the focus socket is closed to other users" "$(stat -c %a "$WORK/run/focus.sock")" 600
send_focus $$
wait_for 'FOCUS_REPORT_REJECTED' "$WORK/seat.log"
check_contains "rejects a sender without the active seat" 'reason=NOT_ACTIVE_SEAT_USER' "$WORK/seat.log"
check "no priority without the active seat" "$(read_value "$FOCUS/memory.high")" "$HIGH_50"
echo "ACTIVE_UID=$(id -u)" > "$WORK/seats/seat0"
send_focus $$
wait_for 'event=FOCUS_CHANGED' "$WORK/seat.log"
check "the user at the active seat gets priority" "$(read_value "$FOCUS/memory.high")" "$HIGH_75"
wait_for 'FOREGROUND=ACTIVE' "$WORK/run/STATUS"
check_contains "status reports a foreground" 'FOREGROUND=ACTIVE' "$WORK/run/STATUS"
check "status names no application" "$(grep -c 'app-' "$WORK/run/STATUS")" 0
check "status stays readable" "$(stat -c %a "$WORK/run/STATUS")" 644
stop_enforce_daemon "$WORK/seat.log"

BAD_SPACE="$FAKE$APP_SLICE/bad name.scope"
BAD_LINE="$FAKE$APP_SLICE/$(printf 'bad\nname.scope')"
mkdir -p "$BAD_SPACE" "$BAD_LINE"
make_group "$BAD_SPACE" max max max 100 "max 100000"
make_group "$BAD_LINE" max max max 100 "max 100000"
start_enforce_daemon "$WORK/names.log"
check "ignores a group name with a space" "$(read_value "$BAD_SPACE/memory.high")" max
check "ignores a group name with a line break" "$(read_value "$BAD_LINE/memory.high")" max
stop_enforce_daemon "$WORK/names.log"
rm -rf "$BAD_SPACE" "$BAD_LINE"

mkdir -p "$WORK/outside.scope"
echo max > "$WORK/outside.scope/memory.high"
printf '1 2 3 4 max 100000|%s\n' "$WORK/outside.scope" > "$WORK/run/ORIGINAL_VALUES"
start_enforce_daemon "$WORK/forged.log"
stop_enforce_daemon "$WORK/forged.log"
check "saved limits cannot name a path outside application groups" "$(read_value "$WORK/outside.scope/memory.high")" max

LINK_GROUP="$FAKE$APP_SLICE/app-link-test.scope"
mkdir -p "$LINK_GROUP"
make_group "$LINK_GROUP" max max max 100 "max 100000"
echo keep > "$WORK/link_target"
rm "$LINK_GROUP/memory.high"
ln -s "$WORK/link_target" "$LINK_GROUP/memory.high"
start_enforce_daemon "$WORK/link.log"
stop_enforce_daemon "$WORK/link.log"
check "a limit is never written through a link" "$(read_value "$WORK/link_target")" keep
rm -rf "$LINK_GROUP"

echo "== Other users cannot crowd out the active user"
create_foreign_groups() { # uid
    FOREIGN="$FAKE/user.slice/user-$1.slice/user@$1.service/app.slice"
    mkdir -p "$FOREIGN"
    ( cd "$FOREIGN" && mkdir $(seq -f 'flood%g.scope' 1 600) )
}
create_foreign_groups 200000
start_enforce_daemon "$WORK/flood.log"
wait_for 'GROUPS=514 ' "$WORK/run/STATUS"
check_contains "one user is limited to 512 groups" 'GROUPS=514 ' "$WORK/run/STATUS"
stop_enforce_daemon "$WORK/flood.log"
for FOREIGN_UID in 200001 200002 200003 200004 200005 200006 200007 200008; do create_foreign_groups "$FOREIGN_UID"; done
start_enforce_daemon "$WORK/flood.log"
wait_for 'GROUPS=3584 ' "$WORK/run/STATUS"
check_contains "other users stop below the total limit" 'GROUPS=3584 ' "$WORK/run/STATUS"
check_contains "the limit is logged" 'event=GROUP_LIMIT_REACHED' "$WORK/flood.log"
NEW_GROUP="$FAKE$APP_SLICE/app-after-flood.scope"
mkdir -p "$NEW_GROUP"
make_group "$NEW_GROUP" max max max 100 "max 100000"
wait_for "$HIGH_50" "$NEW_GROUP/memory.high"
check "the active user's new group is still accepted" "$(read_value "$NEW_GROUP/memory.high")" "$HIGH_50"
stop_enforce_daemon "$WORK/flood.log"
rm -rf "$NEW_GROUP" "$FAKE"/user.slice/user-20000?.slice

chmod 775 "$WORK/run"
"$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > /dev/null 2>&1
check "refuses a runtime directory others can change" "$?" 1
chmod 700 "$WORK/run"
if [ "$(id -u)" != 0 ]; then
    "$BINARY" --config "$WORK/enforce.conf" --runtime-dir "$WORK/run" > /dev/null 2>&1
    check "refuses ENFORCE on real groups without root" "$?" 1
fi

echo "== Swap files return without the calm period"
mkdir -p "$WORK/swap_files"
chmod 700 "$WORK/swap_files"
: > "$WORK/swap_files/1"
chmod 600 "$WORK/swap_files/1"
MEMORY_KIB=$(awk '/^MemTotal/{print $2}' /proc/meminfo)
cat > "$WORK/retire.conf" <<EOF
RUN_MODE=ENFORCE
FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_CALM_SECONDS=3600
IDLE_READBACK_RESERVE_PERCENT=0
IDLE_PRESSURE_STOP_PERCENT=100
SWAP_FILE_DIRECTORY=$WORK/swap_files
SWAP_FILE_MIN_FREE_DISK_PERCENT=0
EOF
RETIRE_CONFIG="$WORK/retire.conf"
# The kernel refuses swapoff here, so a SWAPOFF_FAILED line means the daemon chose to retire the file.
run_retire() { # zram_used_kib file_used_kib
    {
        echo 'Filename Type Size Used Priority'
        echo "/dev/zram0 partition $MEMORY_KIB $1 100"
        echo "$WORK/swap_files/1 file $MEMORY_KIB $2 -1"
    } > "$WORK/retire_swaps"
    "$BINARY" --config "$RETIRE_CONFIG" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/retire_swaps" --seat-directory "$WORK/seats" > "$WORK/retire.log" 2>&1 &
    DAEMON_PID=$!
    wait_for 'event=STARTED' "$WORK/retire.log"
    sleep 2.5
    kill "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
    ATTEMPTS=$(grep -c 'event=SWAPOFF_FAILED' "$WORK/retire.log")
    if grep -qE 'AddressSanitizer|runtime error' "$WORK/retire.log"; then echo "FAIL sanitizer in retire"; FAILURES=$((FAILURES + 1)); fi
}
run_retire 0 0
check "empty file is retired, and a refusal is not retried at once" "$ATTEMPTS" 1
run_retire 0 $(( MEMORY_KIB / 200 ))
check "file under the read limit is retired" "$ATTEMPTS" 1
run_retire 0 $(( MEMORY_KIB / 10 ))
check "file over the worst case limit waits for the idle read" "$ATTEMPTS" 0
run_retire "$MEMORY_KIB" 0
check "file stays when too little swap would remain" "$ATTEMPTS" 0
chmod 660 "$WORK/swap_files/1"
run_retire 0 0
check "a file others can change is not touched" "$ATTEMPTS" 0
check_contains "a file others can change is reported" 'event=SWAP_FILE_UNTRUSTED' "$WORK/retire.log"
chmod 600 "$WORK/swap_files/1"
mv "$WORK/swap_files/1" "$WORK/swap_files/real"
ln -s real "$WORK/swap_files/1"
run_retire 0 0
check "a link is not touched" "$ATTEMPTS" 0
rm "$WORK/swap_files/1"
mv "$WORK/swap_files/real" "$WORK/swap_files/1"
chmod 777 "$WORK/swap_files"
run_retire 0 0
check "a directory others can change is not trusted" "$ATTEMPTS" 0
chmod 700 "$WORK/swap_files"

echo "== Disk guard"
SIGNAL_FILE="$WORK/signal_seen"
MANAGER_GROUP="$FAKE/system.slice/swapspace.service"
mkdir -p "$MANAGER_GROUP"
sh -c 'trap "echo seen >> \"$1\"" USR2; while :; do sleep 0.1; done' sh "$SIGNAL_FILE" &
SIGNAL_TARGET=$!
sleep 0.3
echo "$SIGNAL_TARGET" > "$MANAGER_GROUP/cgroup.procs"
chmod 644 "$MANAGER_GROUP/cgroup.procs"
: > "$SIGNAL_FILE"
# A limit of 100% is always crossed. No memory rule would allow retiring anything here.
cat > "$WORK/guard.conf" <<EOF
RUN_MODE=ENFORCE
FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_CALM_SECONDS=3600
IDLE_READBACK_RESERVE_PERCENT=100
IDLE_PRESSURE_STOP_PERCENT=1
SWAP_FILE_DIRECTORY=$WORK/swap_files
SWAP_FILE_MIN_FREE_DISK_PERCENT=100
EOF
RETIRE_CONFIG="$WORK/guard.conf"
run_retire "$MEMORY_KIB" 0
check "an empty file goes back under the disk limit, whatever memory does" "$ATTEMPTS" 1
check_contains "the hold is logged" 'event=SWAP_GROWTH_HELD' "$WORK/retire.log"
check_contains "status reports the hold" 'SWAP_GROWTH=HELD' "$WORK/run/STATUS"
check "swapspace is told to add no file" "$([ -s "$SIGNAL_FILE" ] && echo yes)" yes
check "the signal repeats while the limit holds" "$([ "$(wc -l < "$SIGNAL_FILE")" -ge 2 ] && echo yes)" yes
run_retire "$MEMORY_KIB" $(( MEMORY_KIB / 200 ))
check "a file with pages keeps the normal rules under the disk limit" "$ATTEMPTS" 0
: > "$SIGNAL_FILE"
chmod 664 "$MANAGER_GROUP/cgroup.procs"
run_retire "$MEMORY_KIB" 0
check "a process list others can change gets no signal" "$(wc -c < "$SIGNAL_FILE")" 0
chmod 644 "$MANAGER_GROUP/cgroup.procs"
RETIRE_CONFIG="$WORK/retire.conf"
: > "$SIGNAL_FILE"
run_retire "$MEMORY_KIB" 0
check "a limit of 0 turns the guard off" "$(wc -c < "$SIGNAL_FILE")" 0
check_contains "status reports growth allowed" 'SWAP_GROWTH=ALLOWED' "$WORK/run/STATUS"
kill "$SIGNAL_TARGET" 2>/dev/null; wait "$SIGNAL_TARGET" 2>/dev/null

echo "== CPU selectors follow the detected topology"
# Each selector is checked against what the probe found on this machine.
cpu_list() { expand_cpus "$(sed -n "s/^$1=\([^ ]*\) .*/\1/p" "$WORK/probe")"; }
# taskset and the probe may write the same set as 14-15 or 14,15.
expand_cpus() { python3 -I -c 'import sys; print(",".join(str(c) for p in sys.argv[1].split(",") for c in range(int(p.split("-")[0]), int(p.split("-")[-1])+1)))' "$1"; }
run_selector() {
    cat > "$WORK/selector.conf" <<EOF
RUN_MODE=ENFORCE
BACKGROUND_CPU_SET=$1
FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_ENABLED=0
EOF
    sleep 300 &
    SLEEPER=$!
    echo "$SLEEPER" > "$OTHER/cgroup.threads"
    "$BINARY" --config "$WORK/selector.conf" --runtime-dir "$WORK/run" --cgroup-root "$FAKE" --proc-swaps "$WORK/swaps" --seat-directory "$WORK/seats" > "$WORK/selector.log" 2>&1 &
    DAEMON_PID=$!
    wait_for 'event=STARTED' "$WORK/selector.log"
    sleep 0.5
    AFFINITY=$(expand_cpus "$(taskset -cp "$SLEEPER" | sed 's/.*: //')")
    kill "$DAEMON_PID"; wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
    kill "$SLEEPER"; wait "$SLEEPER" 2>/dev/null
    : > "$OTHER/cgroup.threads"
}
run_selector PERFORMANCE
check "PERFORMANCE selects the fastest class" "$AFFINITY" "$(cpu_list CPU_PERFORMANCE)"
run_selector EFFICIENCY
check "EFFICIENCY selects the middle class" "$AFFINITY" "$(cpu_list CPU_EFFICIENCY)"
run_selector LOW_POWER
check "LOW_POWER selects the slowest class" "$AFFINITY" "$(cpu_list CPU_LOW_POWER)"
run_selector LEAST_BUSY:10
EXPECTED_COUNT=$(( ( $(nproc) * 10 + 99 ) / 100 ))
ACTUAL_COUNT=$(echo "$AFFINITY" | tr ',' '\n' | wc -l)
check "LEAST_BUSY:10 selects 10% of CPUs" "$ACTUAL_COUNT" "$EXPECTED_COUNT"
if grep -qE 'AddressSanitizer|runtime error' "$WORK/selector.log"; then echo "FAIL sanitizer in selectors"; FAILURES=$((FAILURES + 1)); else echo "PASS no sanitizer report in selectors"; fi

echo ""
if [ "$FAILURES" = 0 ]; then echo "ALL_TESTS_PASSED"; else echo "FAILURES=$FAILURES"; exit 1; fi
