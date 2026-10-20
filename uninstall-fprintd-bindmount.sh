#!/bin/bash
set -euo pipefail

DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
DROPIN_FILE="$DROPIN_DIR/goodix550c.conf"
DEST_DIR="/opt/goodix550c"

[ "$(id -u)" -eq 0 ] || { echo "Must run with sudo/root"; exit 1; }

if [ -f "$DROPIN_FILE" ]; then
    echo "==> Removing systemd drop-in: $DROPIN_FILE"
    rm -f "$DROPIN_FILE"
    rmdir "$DROPIN_DIR" 2>/dev/null || true
else
    echo "==> Systemd drop-in not found ($DROPIN_FILE), skipping"
fi

if [ -d "$DEST_DIR" ]; then
    echo "==> Removing $DEST_DIR"
    rm -rf "$DEST_DIR"
else
    echo "==> Directory $DEST_DIR not found, skipping"
fi

echo "==> Reloading systemd daemon"
systemctl daemon-reload

echo "==> Restarting fprintd"
systemctl restart fprintd

echo "==> Uninstalled. fprintd is back to stock configuration."
