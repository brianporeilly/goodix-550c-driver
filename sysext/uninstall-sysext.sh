#!/bin/bash
# Revert the goodix550c sysext overlay; restores the stock host libfprint.
# Run as root.
set -e
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo."; exit 1; }

systemd-sysext unmerge 2>/dev/null || true
rm -rf /var/lib/extensions/goodix550c-fp
systemctl restart fprintd 2>/dev/null || true
pkill -f fprintd 2>/dev/null || true

echo "Reverted. Stock host libfprint restored:"
ls -lZ /usr/lib64/libfprint-2.so.2.0.0
