#!/bin/sh
# Real kernel test with the capability set of the installed service. Uses its own swap file and its
# own application group. Other applications keep their limits: background percentages are 100 here.
# Usage: sudo sh TESTS/RUN_ROOT_TESTS.sh
set -u
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0"; exit 1; }
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
DAEMON="$ROOT/BUILD/adaptive-resource-managerd"
HOLDER="$ROOT/BUILD/memory-holder"
[ -x "$DAEMON" ] && [ -x "$HOLDER" ] || { echo "Build first: make"; exit 1; }
USER_ID=${SUDO_UID:-}
[ -n "$USER_ID" ] || { echo "Run through sudo so the test can use your user ID."; exit 1; }

MEMORY_MIB=$(awk '/^MemTotal/{print int($2/1024)}' /proc/meminfo)
available_mib() { awk '/^MemAvailable/{print int($2/1024)}' /proc/meminfo; }
HOLD_MIB=$(( MEMORY_MIB / 15 ))
REQUIRED_MIB=$(( MEMORY_MIB * 40 / 100 ))
[ "$(available_mib)" -ge "$REQUIRED_MIB" ] || { echo "Needs $REQUIRED_MIB MiB available, has $(available_mib). Nothing changed."; exit 1; }

SWAP_DIRECTORY=/var/lib/adaptive-resource-manager-test
SWAP_FILE="$SWAP_DIRECTORY/1"
APP_SLICE="/sys/fs/cgroup/user.slice/user-$USER_ID.slice/user@$USER_ID.service/app.slice"
GROUP="$APP_SLICE/app-adaptive-resource-manager-test.scope"
[ ! -e "$SWAP_DIRECTORY" ] && [ ! -e "$GROUP" ] || { echo "Leftovers from an earlier test exist. Nothing changed."; exit 1; }
RESULT_DIRECTORY="$ROOT/TESTS/RESULTS"
LOG_NAME="ROOT_TEST_$(date +%Y%m%d_%H%M%S).log"
WORK=$(mktemp -d)
chmod 755 "$WORK"
LOG="$WORK/$LOG_NAME"
: > "$LOG"
mkfifo "$WORK/commands"
FAILURES=0
DAEMON_PID=""
HOLDER_PID=""
BOUNDING="--bounding-set=-all,+sys_admin,+sys_nice,+sys_ptrace,+dac_override,+chown"
SERVICE=adaptive-resource-manager.service
SERVICE_WAS_ACTIVE=0
# Some kernels refuse to reclaim anonymous memory below this share of memory. The test turns it off only
# when it would get in the way, and restores it on exit.
ANON_FLOOR_FILE=/proc/sys/vm/anon_min_ratio
ANON_FLOOR_BEFORE=""

cleanup() {
    trap - EXIT INT TERM HUP
    [ -n "$ANON_FLOOR_BEFORE" ] && echo "$ANON_FLOOR_BEFORE" > "$ANON_FLOOR_FILE"
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    [ -n "$HOLDER_PID" ] && kill "$HOLDER_PID" 2>/dev/null
    exec 3>&- 2>/dev/null
    wait 2>/dev/null
    swapoff "$SWAP_FILE" 2>/dev/null
    rm -f "$SWAP_FILE"
    rmdir "$SWAP_DIRECTORY" "$GROUP" 2>/dev/null
    # The results directory belongs to the user, so root writes nothing there.
    setpriv --reuid="$USER_ID" --regid="${SUDO_GID:-$USER_ID}" --clear-groups sh -c 'mkdir -p "$1" && cat > "$1/$2"' sh "$RESULT_DIRECTORY" "$LOG_NAME" < "$LOG"
    rm -rf "$WORK"
    [ "$SERVICE_WAS_ACTIVE" = 1 ] && systemctl start "$SERVICE"
    if grep -q adaptive-resource-manager-test /proc/swaps; then echo "WARNING: test swap still active: $SWAP_FILE"; else echo "Test swap and group removed. Log: $RESULT_DIRECTORY/$LOG_NAME"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

say() { echo "$*" | tee -a "$LOG"; }
fail() { say "FAIL $*"; FAILURES=$((FAILURES + 1)); }
pass() { say "PASS $*"; }
disk_used_mib() { awk -v file="$SWAP_FILE" '$1==file{print int($4/1024)}' /proc/swaps; }
group_cached_mib() { awk '$1=="swapcached"{print int($2/1048576)}' "$GROUP/memory.stat"; }
written_pages() { awk '/^pswpout /{print $2}' /proc/vmstat; }
as_user() { setpriv --reuid="$USER_ID" --clear-groups "$@"; }
send_focus() { as_user python3 -I -c 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); s.sendto(sys.argv[2].encode(),sys.argv[1])' "$WORK/run/focus.sock" "$1"; }
wait_log() { # pattern file seconds
    i=0
    while ! grep -q "$1" "$2" && [ $i -lt $(( $3 * 5 )) ]; do sleep 0.2; i=$((i + 1)); done
    grep -q "$1" "$2"
}
create_swap() {
    fallocate -l $(( HOLD_MIB * 3 ))M "$SWAP_FILE" && chmod 600 "$SWAP_FILE" && mkswap -q "$SWAP_FILE" && swapon -p 200 "$SWAP_FILE"
}
remove_swap() { swapoff "$SWAP_FILE" 2>/dev/null; rm -f "$SWAP_FILE"; }
holder_command() { printf '%s' "$1" >&3; }
anon_lru_mib() { awk '/^Active\(anon\):/{a=$2} /^Inactive\(anon\):/{i=$2} END{print int((a+i)/1024)}' /proc/meminfo; }
turn_off_anon_floor() {
    [ -z "$ANON_FLOOR_BEFORE" ] && [ -w "$ANON_FLOOR_FILE" ] || return 1
    ANON_FLOOR_BEFORE=$(cat "$ANON_FLOOR_FILE")
    say "Kernel anonymous memory floor off for this test: was $ANON_FLOOR_BEFORE% = $(( MEMORY_MIB * ANON_FLOOR_BEFORE / 100 )) MiB, anonymous memory $(anon_lru_mib) MiB, group cache $(group_cached_mib) MiB"
    echo 0 > "$ANON_FLOOR_FILE"
}
# Pages must leave the swap cache or the next read never touches the disk.
page_out_fully() {
    holder_command p
    i=0
    while [ "${disk:-0}" -lt $(( HOLD_MIB * 85 / 100 )) ] && [ $i -lt 40 ]; do sleep 0.25; disk=$(disk_used_mib); i=$((i + 1)); done
    disk=0
    i=0
    # Pages cannot leave the cache while the disk is still writing them, which can take many seconds.
    while [ $i -lt 100 ]; do
        printf '%s' $(( HOLD_MIB * 1048576 )) > "$GROUP/memory.reclaim" 2>/dev/null
        [ "$(group_cached_mib)" -le 8 ] && break
        [ $i = 20 ] && turn_off_anon_floor
        sleep 0.3
        i=$((i + 1))
    done
    say "Prepared: disk $(disk_used_mib) MiB, group cache $(group_cached_mib) MiB"
    [ "$(group_cached_mib)" -le 8 ] || fail "test preparation: pages stayed in the cache, so the next result is not a real disk read"
}
verify_data() {
    before=$(grep -c VERIFY "$WORK/holder.out")
    holder_command v
    i=0
    while [ "$(grep -c VERIFY "$WORK/holder.out")" -le "$before" ] && [ $i -lt 600 ]; do sleep 0.2; i=$((i + 1)); done
    result=$(grep VERIFY "$WORK/holder.out" | tail -1)
    if [ "$(grep -c VERIFY "$WORK/holder.out")" -gt "$before" ] && echo "$result" | grep -q 'VERIFY=OK'; then pass "data intact after $1 ($result)"; else fail "data after $1: ${result:-no answer}"; fi
}
start_daemon() { # log config
    setpriv "$BOUNDING" "$DAEMON" --config "$2" --runtime-dir "$WORK/run" --seat-directory "$WORK/seats" > "$1" 2>&1 &
    DAEMON_PID=$!
    wait_log 'event=STARTED' "$1" 10 || fail "daemon did not start"
    sleep 0.3
}
stop_daemon() { kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null; DAEMON_PID=""; }
write_config() {
    cat > "$WORK/test.conf" <<EOF
RUN_MODE=ENFORCE
FOCUS_DEBOUNCE_MILLISECONDS=50
BACKGROUND_GRACE_SECONDS=1
BACKGROUND_MEMORY_HIGH_PERCENT=100
BACKGROUND_SWAP_MAX_PERCENT=100
BACKGROUND_CPU_WEIGHT_PERCENT=100
BACKGROUND_CPU_QUOTA_PERCENT=100
SWAP_FILE_DIRECTORY=$SWAP_DIRECTORY
SWAP_FILE_RETIRE_MIN_FREE_SWAP_PERCENT=0
SWAP_FILE_MIN_FREE_DISK_PERCENT=0
$1
EOF
}
start_holder() {
    sh -c 'echo $$ > "$1/cgroup.procs" && shift && exec "$@"' sh "$GROUP" setpriv --reuid="$USER_ID" --clear-groups "$HOLDER" hold "$HOLD_MIB" < "$WORK/commands" > "$WORK/holder.out" &
    HOLDER_PID=$!
    exec 3> "$WORK/commands"
    wait_log READY "$WORK/holder.out" 120 || { fail "holder did not start"; exit 1; }
}

# The tests expect limits only from their own daemon. The service restarts on exit.
if systemctl is-active --quiet "$SERVICE"; then
    systemctl stop "$SERVICE"
    SERVICE_WAS_ACTIVE=1
fi
install -d -m 700 "$SWAP_DIRECTORY"
install -d -m 755 "$WORK/run" "$WORK/seats"
echo "ACTIVE_UID=$USER_ID" > "$WORK/seats/seat0"
mkdir -p "$GROUP"
create_swap || exit 1
{ date; uptime; cat /proc/swaps; echo "HOLD_MIB=$HOLD_MIB"; } >> "$LOG"
start_holder

say ""; say "== 1) Clicking an application brings its pages back and protects it"
write_config 'IDLE_READBACK_ENABLED=0'
page_out_fully
start_daemon "$WORK/d1.log" "$WORK/test.conf"
send_focus "$HOLDER_PID"
wait_log 'event=READBACK_FINISHED' "$WORK/d1.log" 90 || fail "foreground readback did not finish"
grep -E 'event=(FOCUS_CHANGED|READBACK_STARTED|READBACK_FINISHED)' "$WORK/d1.log" | tee -a "$LOG"
SWAP_IN=$(sed -n 's/.*event=READBACK_FINISHED.*swap_in_mib=\([0-9]*\).*/\1/p' "$WORK/d1.log" | tail -1)
if [ "${SWAP_IN:-0}" -ge $(( HOLD_MIB * 80 / 100 )) ]; then pass "real disk reads: $SWAP_IN MiB"; else fail "real disk reads: ${SWAP_IN:-0} MiB of $HOLD_MIB"; fi
LOW=$(cat "$GROUP/memory.low")
if [ "$LOW" -gt 0 ]; then pass "foreground protected: memory.low=$LOW"; else fail "memory.low was not set"; fi
if [ "$(cat "$APP_SLICE/memory.low")" -gt 0 ]; then pass "protection reaches ancestors"; else fail "ancestor memory.low was not set"; fi
if [ "$(cat "$GROUP/cpu.weight")" -gt 100 ]; then pass "foreground CPU weight raised: $(cat "$GROUP/cpu.weight")"; else fail "CPU weight was not raised"; fi
wait_log 'event=FOCUS_STALL' "$WORK/d1.log" 10 && grep 'event=FOCUS_STALL' "$WORK/d1.log" | tee -a "$LOG"
verify_data "foreground readback"
stop_daemon
if [ "$(cat "$GROUP/memory.low")" = 0 ] && [ "$(cat "$APP_SLICE/memory.low")" = 0 ]; then pass "protection removed on exit"; else fail "memory.low left behind"; fi

say ""; say "== 2) Pressure while the foreground application is protected"
write_config 'IDLE_READBACK_ENABLED=0'
page_out_fully
start_daemon "$WORK/d2.log" "$WORK/test.conf"
send_focus "$HOLDER_PID"
wait_log 'event=READBACK_FINISHED' "$WORK/d2.log" 90
CACHE_BEFORE=$(group_cached_mib); WRITTEN_BEFORE=$(written_pages)
AVAILABLE=$(available_mib); ALLOCATE=$(( AVAILABLE - MEMORY_MIB / 15 ))
say "Before: group cache $CACHE_BEFORE MiB, available $AVAILABLE MiB, allocating $ALLOCATE MiB"
as_user "$HOLDER" allocate "$ALLOCATE" | tee -a "$LOG"
say "After: group cache $CACHE_BEFORE -> $(group_cached_mib) MiB, written $(( ( $(written_pages) - WRITTEN_BEFORE ) / 256 )) MiB (report only)"
verify_data "pressure on the foreground application"
stop_daemon

say ""; say "== 3) Calm system: disk swap returns and the file is retired"
# The pressure step pushed other applications' tmpfs and GPU pages here. Process scans cannot reach
# those, so the file would rightly be kept. Start clean to exercise the retire path.
remove_swap
create_swap || exit 1
write_config 'FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_CALM_SECONDS=2
IDLE_READBACK_RESERVE_PERCENT=10
IDLE_PRESSURE_STOP_PERCENT=100'
page_out_fully
START=$(date +%s)
start_daemon "$WORK/d3.log" "$WORK/test.conf"
wait_log 'event=SWAP_FILE_RETIRED\|event=SWAP_FILE_KEPT' "$WORK/d3.log" 120 || fail "idle readback did not finish"
grep -E 'event=(IDLE_WAIT|READBACK_STARTED|READBACK_FINISHED|SWAP_FILE)' "$WORK/d3.log" | tee -a "$LOG"
say "Elapsed $(( $(date +%s) - START )) s"
if grep -q 'event=SWAP_FILE_RETIRED' "$WORK/d3.log" && [ ! -e "$SWAP_FILE" ]; then pass "swap file retired"; else fail "swap file still exists"; fi
verify_data "idle readback"
stop_daemon

say ""; say "== 4) Idle readback stops and releases its pages when memory is needed"
# This step checks the daemon's release, which that floor could block.
turn_off_anon_floor
[ -e "$SWAP_FILE" ] || create_swap
AVAILABLE=$(available_mib)
RESERVE_MIB=$(( AVAILABLE - MEMORY_MIB / 10 ))
RESERVE_PERCENT=$(( RESERVE_MIB * 100 / MEMORY_MIB ))
write_config "FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_CALM_SECONDS=2
IDLE_READBACK_RESERVE_PERCENT=$RESERVE_PERCENT
IDLE_DISK_BUSY_TARGET_PERCENT=1
READBACK_MAX_RATE_PERCENT_OF_MEMORY_PER_SECOND=1
IDLE_PRESSURE_STOP_PERCENT=100"
page_out_fully
start_daemon "$WORK/d4.log" "$WORK/test.conf"
wait_log 'event=READBACK_STARTED' "$WORK/d4.log" 60 || fail "idle readback did not start"
sleep 2
CACHE_BEFORE=$(group_cached_mib)
as_user "$HOLDER" allocate $(( AVAILABLE - MEMORY_MIB / 15 )) | tee -a "$LOG"
grep -E 'event=(READBACK_STOPPED|PAGES_RELEASED)' "$WORK/d4.log" | tee -a "$LOG"
say "Group cache $CACHE_BEFORE -> $(group_cached_mib) MiB"
if [ "$CACHE_BEFORE" -lt 16 ]; then fail "readback had not started reading (cache $CACHE_BEFORE MiB)"; fi
if grep -q 'event=READBACK_STOPPED' "$WORK/d4.log"; then pass "readback stopped under pressure"; else fail "readback did not stop"; fi
if grep -q 'event=PAGES_RELEASED' "$WORK/d4.log"; then pass "pages were released"; else fail "pages were not released"; fi
if [ "$(group_cached_mib)" -le 64 ]; then pass "group cache emptied"; else fail "group cache still $(group_cached_mib) MiB"; fi
verify_data "release"
stop_daemon

say ""; say "== 5) Unused space returns without the calm period"
write_config 'FOREGROUND_READBACK_ENABLED=0
IDLE_READBACK_CALM_SECONDS=3600
IDLE_READBACK_RESERVE_PERCENT=10
IDLE_PRESSURE_STOP_PERCENT=100'
remove_swap
create_swap || exit 1
start_daemon "$WORK/d5.log" "$WORK/test.conf"
if wait_log 'event=SWAP_FILE_RETIRED' "$WORK/d5.log" 5 && [ ! -e "$SWAP_FILE" ]; then pass "empty file retired at once"; else fail "empty file kept"; fi
grep 'event=SWAP_FILE' "$WORK/d5.log" | tee -a "$LOG"
stop_daemon

holder_command q
exec 3>&-
wait "$HOLDER_PID" 2>/dev/null
HOLDER_PID=""
HOLD_MIB=$(( MEMORY_MIB / 250 ))
create_swap || exit 1
start_holder
page_out_fully
start_daemon "$WORK/d6.log" "$WORK/test.conf"
wait_log 'event=SWAP_FILE_RETIRED' "$WORK/d6.log" 5
grep 'event=SWAP_FILE' "$WORK/d6.log" | tee -a "$LOG"
USED=$(sed -n 's/.*event=SWAP_FILE_RETIRED.*used_mib=\([0-9]*\).*/\1/p' "$WORK/d6.log" | tail -1)
if [ "${USED:-0}" -ge $(( HOLD_MIB / 2 )) ] && [ ! -e "$SWAP_FILE" ]; then pass "file holding $USED MiB retired at once"; else fail "small file: retired ${USED:-no} MiB of $HOLD_MIB"; fi
verify_data "retiring a small file"
stop_daemon

# Reading pages back keeps their swap slots, so the file looks full although swapoff reads nothing.
holder_command q
exec 3>&-
wait "$HOLDER_PID" 2>/dev/null
HOLDER_PID=""
HOLD_MIB=$(( MEMORY_MIB / 30 ))
create_swap || exit 1
start_holder
page_out_fully
verify_data "reading pages back into memory"
say "Before retiring: file $(disk_used_mib) MiB, group cache $(group_cached_mib) MiB"
start_daemon "$WORK/d9.log" "$WORK/test.conf"
wait_log 'event=SWAP_FILE_RETIRED' "$WORK/d9.log" 5
grep 'event=SWAP_FILE' "$WORK/d9.log" | tee -a "$LOG"
USED=$(sed -n 's/.*event=SWAP_FILE_RETIRED.*used_mib=\([0-9]*\).*/\1/p' "$WORK/d9.log" | tail -1)
if [ "${USED:-0}" -ge $(( HOLD_MIB * 80 / 100 )) ] && [ ! -e "$SWAP_FILE" ]; then pass "file with $USED MiB already in memory retired at once"; else fail "file with pages in memory: retired ${USED:-no} MiB of $HOLD_MIB"; fi
verify_data "retiring a file whose pages are in memory"
stop_daemon

say ""; say "== 6) Permission checks with real privileges"
start_daemon "$WORK/d10.log" "$WORK/test.conf"
if [ "$(stat -c '%u %a' "$WORK/run/focus.sock")" = "$USER_ID 600" ]; then pass "the focus socket belongs to the active seat user"; else fail "focus socket: $(stat -c '%u %a' "$WORK/run/focus.sock")"; fi
if setpriv --reuid=65534 --regid=65534 --clear-groups python3 -I -c 'import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); s.sendto(b"1",sys.argv[1])' "$WORK/run/focus.sock" 2>/dev/null; then fail "another user reached the focus socket"; else pass "another user cannot reach the focus socket"; fi
as_user python3 -I -c 'import os,sys; os.chmod(sys.argv[1], 0o666)' "$WORK/run/focus.sock"
sleep 2.5
if [ "$(stat -c '%u %a' "$WORK/run/focus.sock")" = "$USER_ID 600" ]; then pass "a mode changed by the owner is replaced"; else fail "socket after chmod: $(stat -c '%u %a' "$WORK/run/focus.sock")"; fi
echo "ACTIVE_UID=65534" > "$WORK/seats/seat0"
sleep 2.5
if [ "$(stat -c '%u %a' "$WORK/run/focus.sock")" = "65534 600" ]; then pass "the socket follows the active seat"; else fail "socket after seat change: $(stat -c '%u %a' "$WORK/run/focus.sock")"; fi
rm "$WORK/seats/seat0"
sleep 2.5
if [ "$(stat -c '%u %a' "$WORK/run/focus.sock")" = "0 600" ]; then pass "without an active seat the socket returns to root"; else fail "socket without a seat: $(stat -c '%u %a' "$WORK/run/focus.sock")"; fi
echo "ACTIVE_UID=$USER_ID" > "$WORK/seats/seat0"
stop_daemon
chmod 664 "$WORK/test.conf"
setpriv "$BOUNDING" "$DAEMON" --config "$WORK/test.conf" --runtime-dir "$WORK/run" --seat-directory "$WORK/seats" > "$WORK/d7.log" 2>&1
if [ "$?" = 2 ]; then pass "refuses a configuration others can change"; else fail "started with a configuration others can change"; fi
chmod 644 "$WORK/test.conf"
[ -e "$SWAP_FILE" ] || create_swap || exit 1
chmod 660 "$SWAP_FILE"
start_daemon "$WORK/d8.log" "$WORK/test.conf"
wait_log 'event=SWAP_FILE_UNTRUSTED' "$WORK/d8.log" 5
if grep -q 'event=SWAP_FILE_UNTRUSTED' "$WORK/d8.log" && [ -e "$SWAP_FILE" ] && grep -q "^$SWAP_FILE " /proc/swaps; then pass "keeps a swap file others can change"; else fail "acted on a swap file others can change"; fi
stop_daemon
remove_swap

say ""
if [ "$FAILURES" = 0 ]; then say "ALL_ROOT_TESTS_PASSED"; else say "FAILURES=$FAILURES"; fi
exit "$FAILURES"
