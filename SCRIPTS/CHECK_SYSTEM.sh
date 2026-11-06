#!/bin/sh
# Checks this system before anything is installed. Reads only and changes nothing. Needs no root.
# Usage: CHECK_SYSTEM.sh [install|enforce] [root directory, for tests]
# BLOCK stops the install scripts. WARN is printed and the install goes on.
set -u
MODE=${1:-enforce}
SYSTEM_ROOT=${2:-}
case "$MODE" in
    install|enforce) ;;
    *) echo "Usage: $0 [install|enforce]"; exit 2 ;;
esac
REPOSITORY=$(dirname "$(dirname "$(readlink -f "$0")")")
METADATA="$REPOSITORY/GNOME_EXTENSION/focus-reporter@adaptive-resource-manager/metadata.json"
BLOCKS=0
WARNINGS=0
ok() { echo "OK    $1"; }
warn() { echo "WARN  $1"; WARNINGS=$((WARNINGS + 1)); }
block() { echo "BLOCK $1"; BLOCKS=$((BLOCKS + 1)); }

MACHINE=$(uname -m)
if [ "$MACHINE" = x86_64 ]; then ok "Architecture $MACHINE"; else block "Architecture $MACHINE: only x86_64 is supported"; fi

RELEASE=$(uname -r)
MAJOR=${RELEASE%%.*}
MINOR_AND_REST=${RELEASE#*.}
MINOR=${MINOR_AND_REST%%[!0-9]*}
case "$MAJOR.$MINOR" in
    *[!0-9.]*|.*|*.) block "Linux $RELEASE: could not read the version, 6.7 or newer is needed" ;;
    *)
        if [ "$MAJOR" -gt 6 ] || { [ "$MAJOR" -eq 6 ] && [ "$MINOR" -ge 7 ]; }; then ok "Linux $MAJOR.$MINOR"
        else block "Linux $MAJOR.$MINOR: 6.7 or newer is needed"; fi ;;
esac

if [ -d "$SYSTEM_ROOT/run/systemd/system" ]; then ok "systemd is running"; else block "systemd is not running: it is required"; fi

if grep -qw memory "$SYSTEM_ROOT/sys/fs/cgroup/cgroup.controllers" 2>/dev/null; then ok "cgroup v2 with the memory controller"
else block "cgroup v2 with the memory controller is required"; fi

if grep -q ' /sys/fs/cgroup cgroup2 .*memory_recursiveprot' "$SYSTEM_ROOT/proc/mounts" 2>/dev/null; then ok "memory_recursiveprot"
else warn "cgroup v2 is not mounted with memory_recursiveprot: protection will not reach groups inside an application"; fi

if cat "$SYSTEM_ROOT/proc/pressure/memory" > /dev/null 2>&1; then ok "Pressure information"
else warn "No pressure information: swap files are not retired by themselves. Add psi=1 to the kernel command line"; fi

if [ "$(cat "$SYSTEM_ROOT/sys/module/zswap/parameters/enabled" 2>/dev/null)" = Y ]; then
    warn "zswap is on: pages are compressed twice on top of zram. Add zswap.enabled=0 to the kernel command line"
fi

if command -v systemctl > /dev/null 2>&1; then
    for SERVICE in systemd-oomd earlyoom; do
        if systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
            warn "$SERVICE is running: it can kill applications under the memory pressure and swap use this software allows. They have not been tested together"
        fi
    done
fi

# Focus reporting. Without it the daemon leaves application limits alone, so this does not stop the install.
if [ -d "$SYSTEM_ROOT/run/systemd/seats" ]; then ok "logind seat information"
else warn "No logind seat information: nobody can report focus"; fi
SHELL_MAJOR=$(gnome-shell --version 2>/dev/null | sed -n 's/^GNOME Shell \([0-9]*\).*/\1/p')
SUPPORTED_SHELLS=$(sed -n 's/.*"shell-version": *\[\(.*\)\].*/\1/p' "$METADATA")
case "$SUPPORTED_SHELLS" in
    *"\"$SHELL_MAJOR\""*) FOCUS_REPORTER=1 ;;
    *) FOCUS_REPORTER=0 ;;
esac
if [ -n "$SHELL_MAJOR" ] && [ "$FOCUS_REPORTER" = 1 ]; then
    ok "GNOME Shell $SHELL_MAJOR: the focus reporter works in a GNOME session"
else
    warn "No focus reporter here (GNOME Shell ${SHELL_MAJOR:-not found}, supported: $SUPPORTED_SHELLS): no application gets priority or a background limit. Swap management still works"
fi
if grep -q '/app\.slice/' "$SYSTEM_ROOT/proc/self/cgroup" 2>/dev/null; then ok "This shell runs in app.slice"
else warn "This shell runs outside app.slice, as over SSH or on desktops that do not use it. Only applications in app.slice are managed"; fi

if [ "$MODE" = enforce ]; then
    # Searched by directory: an ordinary user's PATH usually has no sbin, and this check needs no root.
    SWAPSPACE_FOUND=0
    for DIRECTORY in /usr/local/sbin /usr/sbin /sbin /usr/local/bin /usr/bin /bin; do
        [ -x "$SYSTEM_ROOT$DIRECTORY/swapspace" ] && SWAPSPACE_FOUND=1
    done
    SWAPSPACE_UNIT_FOUND=0
    for DIRECTORY in /etc/systemd/system /usr/local/lib/systemd/system /usr/lib/systemd/system /lib/systemd/system; do
        [ -e "$SYSTEM_ROOT$DIRECTORY/swapspace.service" ] && SWAPSPACE_UNIT_FOUND=1
    done
    if [ "$SWAPSPACE_FOUND" = 0 ]; then block "swapspace is missing: see the README for your distribution"
    elif [ "$SWAPSPACE_UNIT_FOUND" = 0 ]; then block "swapspace.service is missing: ENABLE_ENFORCE.sh configures swapspace through it, so a swapspace built from source needs its own unit"
    else ok "swapspace"; fi
    if [ -x "$SYSTEM_ROOT/usr/lib/systemd/system-generators/zram-generator" ]; then ok "zram-generator"; else block "zram-generator is missing"; fi

    SWAP_DIRECTORY="$SYSTEM_ROOT/var/lib/swapspace"
    [ -d "$SWAP_DIRECTORY" ] || SWAP_DIRECTORY="$SYSTEM_ROOT/var/lib"
    FILESYSTEM=$(stat -f -c %T "$SWAP_DIRECTORY" 2>/dev/null)
    case "$FILESYSTEM" in
        ext2/ext3) ok "Swap files on ext4" ;;
        zfs) block "Swap files do not work on ZFS" ;;
        btrfs) warn "Swap files on btrfs are not tested here. An active swap file also blocks snapshots of its subvolume" ;;
        *) warn "Swap files on ${FILESYSTEM:-an unknown filesystem} are not tested here. Only ext4 is" ;;
    esac

    if [ "$(cat "$SYSTEM_ROOT/sys/fs/selinux/enforce" 2>/dev/null)" = 1 ]; then
        warn "SELinux is enforcing: not tested here, and it may refuse swap files"
    fi
fi

if [ "$BLOCKS" -gt 0 ]; then
    echo "CHECK_SYSTEM=FAILED blocks=$BLOCKS warnings=$WARNINGS. Nothing was changed."
    exit 1
fi
echo "CHECK_SYSTEM=PASSED warnings=$WARNINGS"
