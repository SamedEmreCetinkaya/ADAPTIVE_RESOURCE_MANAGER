#!/bin/sh
# Installs the daemon in OBSERVE mode and the focus extension. Changes no memory setting until
# ENABLE_ENFORCE.sh is run.
set -eu
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0"; exit 1; }
ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
[ -x "$ROOT/BUILD/adaptive-resource-managerd" ] || { echo "Build first: make"; exit 1; }
USER_NAME=${SUDO_USER:-}
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || { echo "Run through sudo so the extension goes to your account."; exit 1; }
USER_ID=$(id -u "$USER_NAME") || { echo "Unknown user: $USER_NAME"; exit 1; }
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
UUID=focus-reporter@adaptive-resource-manager
as_user() { runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="/run/user/$USER_ID" "$@"; }

install -m 755 -o root -g root "$ROOT/BUILD/adaptive-resource-managerd" /usr/local/sbin/adaptive-resource-managerd
install -d -m 755 -o root -g root /etc/adaptive-resource-manager /usr/local/libexec/adaptive-resource-manager /usr/local/share/doc/adaptive-resource-manager
[ -e /etc/adaptive-resource-manager/RESOURCE_POLICY.conf ] || install -m 644 -o root -g root "$ROOT/CONFIG/RESOURCE_POLICY.conf" /etc/adaptive-resource-manager/RESOURCE_POLICY.conf
[ -e /etc/adaptive-resource-manager/SYSTEM_TUNABLES.conf ] || install -m 644 -o root -g root "$ROOT/CONFIG/SYSTEM_TUNABLES.conf" /etc/adaptive-resource-manager/SYSTEM_TUNABLES.conf
install -m 755 -o root -g root "$ROOT/SCRIPTS/APPLY_SYSTEM_TUNABLES.sh" /usr/local/libexec/adaptive-resource-manager/APPLY_SYSTEM_TUNABLES.sh
install -m 644 -o root -g root "$ROOT/README.md" /usr/local/share/doc/adaptive-resource-manager/README.md
install -m 644 -o root -g root "$ROOT/SYSTEMD/adaptive-resource-manager.service" /etc/systemd/system/adaptive-resource-manager.service
if [ -e /etc/systemd/system/adaptive-resource-manager-tunables.service ]; then
    install -m 644 -o root -g root "$ROOT/SYSTEMD/adaptive-resource-manager-tunables.service" /etc/systemd/system/adaptive-resource-manager-tunables.service
fi
install -d -m 755 -o root -g root /etc/systemd/user/app-.scope.d
install -m 644 -o root -g root "$ROOT/SYSTEMD/CPU_CONTROLLER.conf" /etc/systemd/user/app-.scope.d/ADAPTIVE_RESOURCE_MANAGER.conf
as_user systemctl --user daemon-reload || true
systemctl daemon-reload
systemctl enable adaptive-resource-manager.service
# restart, not start: a reinstall must load the new binary.
systemctl restart adaptive-resource-manager.service

# The user's home belongs to the user, so root writes nothing there.
EXTENSION_DIRECTORY="$USER_HOME/.local/share/gnome-shell/extensions/$UUID"
as_user sh -c 'install -d -m 755 "$1" && install -m 644 "$2/metadata.json" "$2/extension.js" "$1/"' sh "$EXTENSION_DIRECTORY" "$ROOT/GNOME_EXTENSION/$UUID"

systemctl is-active adaptive-resource-manager.service
for _ in 1 2 3 4 5; do
    [ -e /run/adaptive-resource-manager/STATUS ] && break
    sleep 1
done
cat /run/adaptive-resource-manager/STATUS || true
echo
EXTENSION_STATE=$(as_user gnome-extensions info "$UUID" 2>/dev/null | sed -n 's/^ *State: //p')
if [ "$EXTENSION_STATE" != ACTIVE ]; then
    echo "Next: log out and back in, then run: gnome-extensions enable $UUID"
fi
