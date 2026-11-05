#!/bin/sh
# Restores the system files saved by ENABLE_ENFORCE.sh and returns the daemon to OBSERVE.
set -eu
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0 <backup directory>"; exit 1; }
BACKUP=${1:-}
USAGE="Usage: sudo sh $0 /var/backups/adaptive-resource-manager/<timestamp>"
case "$BACKUP" in
    /var/backups/adaptive-resource-manager/*/*|*..*) echo "$USAGE"; exit 1 ;;
    /var/backups/adaptive-resource-manager/?*) ;;
    *) echo "$USAGE"; exit 1 ;;
esac
# Files from this directory are copied over system files, so only root may be able to change it.
[ -d "$BACKUP" ] && [ ! -L "$BACKUP" ] && [ -f "$BACKUP/USER_NAME" ] || { echo "$USAGE"; exit 1; }
[ "$(stat -c '%u %a' "$BACKUP")" = "0 700" ] || { echo "Refusing $BACKUP: it must be owned by root with mode 700."; exit 1; }
USER_NAME=$(cat "$BACKUP/USER_NAME")
USER_ID=$(id -u "$USER_NAME") || { echo "Unknown user in the backup: $USER_NAME"; exit 1; }

sed -i 's/^RUN_MODE=.*/RUN_MODE=OBSERVE/' /etc/adaptive-resource-manager/RESOURCE_POLICY.conf
systemctl restart adaptive-resource-manager.service
systemctl disable adaptive-resource-manager-tunables.service 2>/dev/null || true
rm -f /etc/systemd/system/adaptive-resource-manager-tunables.service /etc/systemd/system/swapspace.service.d/ADAPTIVE_RESOURCE_MANAGER.conf /etc/systemd/user.conf.d/ADAPTIVE_RESOURCE_MANAGER.conf
# Nothing was saved if the file did not exist before enabling.
[ -e "$BACKUP/etc/systemd/zram-generator.conf" ] || rm -f /etc/systemd/zram-generator.conf

( cd "$BACKUP" && find . -type f ! -name USER_NAME ) | while read -r FILE; do
    case "$FILE" in
        ./etc/*|./usr/local/*) cp -p "$BACKUP/$FILE" "/$FILE" ;;
        *) echo "Not restored, outside system configuration: $FILE" ;;
    esac
done
systemctl daemon-reload
sysctl -q --system
USED_FILES=$(awk 'NR>1 && $2=="file" && $4>0' /proc/swaps | wc -l)
if [ "$USED_FILES" = 0 ]; then systemctl restart swapspace.service 2>/dev/null || true; else echo "Disk swap in use: swapspace returns to its old limits at the next boot."; fi
runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="/run/user/$USER_ID" systemctl --user daemon-reexec
echo "System files restored. The old zram size returns at the next boot."
