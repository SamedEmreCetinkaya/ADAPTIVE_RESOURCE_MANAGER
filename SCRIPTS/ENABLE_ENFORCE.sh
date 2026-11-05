#!/bin/sh
# Moves zram, swapspace and kernel memory settings to values relative to this machine and switches the
# daemon to ENFORCE. Every replaced file is copied first. Undo with: DISABLE_ENFORCE.sh <backup directory>
# The zram size takes effect at the next boot. Everything else applies now.
set -eu
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0"; exit 1; }
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
USER_NAME=${SUDO_USER:-}
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || { echo "Run through sudo so your user manager can be updated."; exit 1; }
USER_ID=$(id -u "$USER_NAME") || { echo "Unknown user: $USER_NAME"; exit 1; }
[ -x /usr/local/sbin/adaptive-resource-managerd ] || { echo "Run INSTALL.sh first."; exit 1; }
SWAPSPACE=$(command -v swapspace) || { echo "Install swapspace first. Nothing changed."; exit 1; }
[ -x /usr/lib/systemd/system-generators/zram-generator ] || { echo "Install zram-generator first. Nothing changed."; exit 1; }

TUNABLES=/etc/adaptive-resource-manager/SYSTEM_TUNABLES.conf
[ -f "$TUNABLES" ] && [ ! -L "$TUNABLES" ] && [ "$(stat -c %u "$TUNABLES")" = 0 ] || { echo "Refusing $TUNABLES: only root may own it."; exit 1; }
case "$(stat -c %a "$TUNABLES")" in
    *[2367]?|*[2367]) echo "Refusing $TUNABLES: only root may be able to change it."; exit 1 ;;
esac
config_value() { sed -n "s/^$1=//p" "$TUNABLES" | tail -n 1 | tr -d '"'; }
ZRAM_SIZE_PERCENT=$(config_value ZRAM_SIZE_PERCENT)
ZRAM_COMPRESSION_ALGORITHM=$(config_value ZRAM_COMPRESSION_ALGORITHM)
case "$ZRAM_SIZE_PERCENT" in
    ''|*[!0-9.]*) echo "Invalid ZRAM_SIZE_PERCENT in $TUNABLES"; exit 1 ;;
esac
case "$ZRAM_COMPRESSION_ALGORITHM" in
    ''|*[!A-Za-z0-9\ \(\)=_-]*) echo "Invalid ZRAM_COMPRESSION_ALGORITHM in $TUNABLES"; exit 1 ;;
esac

BACKUP=/var/backups/adaptive-resource-manager/$(date +%Y%m%d_%H%M%S)
SWAPSPACE_OVERRIDE=/etc/systemd/system/swapspace.service.d/ADAPTIVE_RESOURCE_MANAGER.conf
install -d -m 700 /var/backups/adaptive-resource-manager "$BACKUP"
for FILE in /etc/systemd/zram-generator.conf /etc/swapspace.conf; do
    if [ -f "$FILE" ]; then
        install -d -m 700 "$BACKUP$(dirname "$FILE")"
        cp -p "$FILE" "$BACKUP$FILE"
    fi
done
echo "$USER_NAME" > "$BACKUP/USER_NAME"
echo "Backup: $BACKUP"

# The zram size becomes a formula of total memory.
ZRAM_FRACTION=$(awk -v percent="$ZRAM_SIZE_PERCENT" 'BEGIN{printf "%.2f", percent / 100}')
cat > /etc/systemd/zram-generator.conf <<EOF
[zram0]
zram-size = ram * $ZRAM_FRACTION
compression-algorithm = $ZRAM_COMPRESSION_ALGORITHM
EOF

# sysctl and swapspace values are computed at each boot.
install -m 644 -o root -g root "$ROOT/SYSTEMD/adaptive-resource-manager-tunables.service" /etc/systemd/system/adaptive-resource-manager-tunables.service
install -d -m 755 "$(dirname "$SWAPSPACE_OVERRIDE")"
cat > "$SWAPSPACE_OVERRIDE" <<EOF
[Unit]
After=adaptive-resource-manager-tunables.service
Requires=adaptive-resource-manager-tunables.service

[Service]
ExecStart=
ExecStart=$SWAPSPACE -c /run/adaptive-resource-manager-tunables/swapspace.conf
EOF
systemctl daemon-reload
systemctl enable adaptive-resource-manager-tunables.service
systemctl restart adaptive-resource-manager-tunables.service
USED_FILES=$(awk 'NR>1 && $2=="file" && $4>0' /proc/swaps | wc -l)
if [ "$USED_FILES" = 0 ]; then systemctl restart swapspace.service; else echo "Disk swap in use: swapspace picks up its new limits at the next boot."; fi

# A kernel OOM kill ends only the chosen process, not the whole application. Applies to applications
# started from now on.
install -d -m 755 /etc/systemd/user.conf.d
printf '[Manager]\nDefaultOOMPolicy=continue\n' > /etc/systemd/user.conf.d/ADAPTIVE_RESOURCE_MANAGER.conf
runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="/run/user/$USER_ID" systemctl --user daemon-reexec

sed -i 's/^RUN_MODE=.*/RUN_MODE=ENFORCE/' /etc/adaptive-resource-manager/RESOURCE_POLICY.conf
systemctl restart adaptive-resource-manager.service
sleep 1
systemctl is-active adaptive-resource-manager.service swapspace.service
cat /run/adaptive-resource-manager/STATUS
echo "Undo with: sudo sh $ROOT/SCRIPTS/DISABLE_ENFORCE.sh $BACKUP"
