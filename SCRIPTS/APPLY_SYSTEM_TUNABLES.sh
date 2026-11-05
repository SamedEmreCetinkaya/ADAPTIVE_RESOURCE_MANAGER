#!/bin/sh
# Runs at boot. Turns the relative values in SYSTEM_TUNABLES.conf into absolute kernel and swapspace settings.
# Usage: APPLY_SYSTEM_TUNABLES.sh [CONFIG] [OUTPUT_DIRECTORY] [MEMORY_KIB]
# An output directory other than the default only writes files, for tests.
set -eu
DEFAULT_OUTPUT_DIRECTORY=/run/adaptive-resource-manager-tunables
CONFIG=${1:-/etc/adaptive-resource-manager/SYSTEM_TUNABLES.conf}
OUTPUT_DIRECTORY=${2:-$DEFAULT_OUTPUT_DIRECTORY}
MEMORY_KIB=${3:-$(awk '/^MemTotal/{print $2}' /proc/meminfo)}

# Root acts on this file, so only root may be able to change it. It is read, never executed.
config_is_trusted() {
    [ "$(id -u)" = 0 ] || return 0
    [ -f "$CONFIG" ] && [ ! -L "$CONFIG" ] && [ "$(stat -c %u "$CONFIG")" = 0 ] || return 1
    case "$(stat -c %a "$CONFIG")" in
        *[2367]?|*[2367]) return 1 ;;
    esac
}
config_is_trusted || { echo "Refusing $CONFIG: it must be a regular file that only root can change." >&2; exit 1; }

number() {
    VALUE=$(sed -n "s/^$1=//p" "$CONFIG" | tail -n 1)
    case "$VALUE" in
        ''|*[!0-9.]*) echo "Invalid or missing $1 in $CONFIG" >&2; return 1 ;;
    esac
    printf '%s' "$VALUE"
}

# Percent of memory, rounded down. The shell has no decimals.
share() { awk -v memory="$1" -v percent="$2" 'BEGIN{printf "%d", memory * percent / 100}'; }
whole() { awk -v value="$1" 'BEGIN{printf "%d", value + 0.5}'; }

SWAP_FILE_MIN_PERCENT=$(number SWAP_FILE_MIN_PERCENT)
SWAP_FILE_MAX_PERCENT=$(number SWAP_FILE_MAX_PERCENT)
SWAP_FREE_TARGET_PERCENT=$(number SWAP_FREE_TARGET_PERCENT)
SWAP_LOWER_FREE_LIMIT_PERCENT=$(number SWAP_LOWER_FREE_LIMIT_PERCENT)
SWAP_UPPER_FREE_LIMIT_PERCENT=$(number SWAP_UPPER_FREE_LIMIT_PERCENT)
VM_DIRTY_PERCENT_OF_MEMORY=$(number VM_DIRTY_PERCENT_OF_MEMORY)
VM_DIRTY_BACKGROUND_PERCENT_OF_MEMORY=$(number VM_DIRTY_BACKGROUND_PERCENT_OF_MEMORY)
VM_SWAPPINESS=$(number VM_SWAPPINESS)
VM_PAGE_CLUSTER=$(number VM_PAGE_CLUSTER)

mkdir -p "$OUTPUT_DIRECTORY"
MEMORY_MIB=$(( MEMORY_KIB / 1024 ))
cat > "$OUTPUT_DIRECTORY/swapspace.conf" <<EOF
# Generated at boot from $CONFIG. Do not edit.
min_swapsize=$(share "$MEMORY_MIB" "$SWAP_FILE_MIN_PERCENT")m
max_swapsize=$(share "$MEMORY_MIB" "$SWAP_FILE_MAX_PERCENT")m
freetarget=$(whole "$SWAP_FREE_TARGET_PERCENT")
lower_freelimit=$(whole "$SWAP_LOWER_FREE_LIMIT_PERCENT")
upper_freelimit=$(whole "$SWAP_UPPER_FREE_LIMIT_PERCENT")
EOF

DIRTY_BYTES=$(share "$(( MEMORY_KIB * 1024 ))" "$VM_DIRTY_PERCENT_OF_MEMORY")
DIRTY_BACKGROUND_BYTES=$(share "$(( MEMORY_KIB * 1024 ))" "$VM_DIRTY_BACKGROUND_PERCENT_OF_MEMORY")
if [ "$OUTPUT_DIRECTORY" = "$DEFAULT_OUTPUT_DIRECTORY" ]; then
    sysctl -q -w vm.dirty_bytes="$DIRTY_BYTES"
    sysctl -q -w vm.dirty_background_bytes="$DIRTY_BACKGROUND_BYTES"
    sysctl -q -w vm.swappiness="$VM_SWAPPINESS"
    sysctl -q -w vm.page-cluster="$VM_PAGE_CLUSTER"
else
    printf 'vm.dirty_bytes=%s\nvm.dirty_background_bytes=%s\n' "$DIRTY_BYTES" "$DIRTY_BACKGROUND_BYTES" > "$OUTPUT_DIRECTORY/sysctl.conf"
fi
