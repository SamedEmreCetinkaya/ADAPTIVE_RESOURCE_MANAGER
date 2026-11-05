#!/bin/sh
# Removes the daemon and the extension. If ENABLE_ENFORCE.sh was run, run DISABLE_ENFORCE.sh first.
set -u
[ "$(id -u)" = 0 ] || { echo "Root required: sudo sh $0"; exit 1; }
UUID=focus-reporter@adaptive-resource-manager
systemctl disable --now adaptive-resource-manager.service adaptive-resource-manager-tunables.service 2>/dev/null
rm -f /etc/systemd/system/adaptive-resource-manager.service /etc/systemd/system/adaptive-resource-manager-tunables.service /usr/local/sbin/adaptive-resource-managerd
rm -rf /usr/local/libexec/adaptive-resource-manager /usr/local/share/doc/adaptive-resource-manager /etc/adaptive-resource-manager
rm -f /etc/systemd/user/app-.scope.d/ADAPTIVE_RESOURCE_MANAGER.conf
rmdir /etc/systemd/user/app-.scope.d 2>/dev/null
systemctl daemon-reload
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    # The user's home belongs to the user, so the user removes the extension.
    runuser -u "$SUDO_USER" -- rm -rf "$USER_HOME/.local/share/gnome-shell/extensions/$UUID"
fi
echo "Adaptive Resource Manager removed. Log out and back in to unload the extension."
