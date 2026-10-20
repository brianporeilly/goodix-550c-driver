#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/sysext/goodix550c-fp}"
[ -d "$BUILD_DIR" ] || BUILD_DIR="$SCRIPT_DIR/sysext/goodix550c-fp"
SRC="${SRC:-$BUILD_DIR/usr/lib64/libfprint-2.so.2.0.0}"

DEST_DIR="/opt/goodix550c/lib64"
DEST_FILE="$DEST_DIR/libfprint-2.so.2.0.0"
DROPIN_DIR="/etc/systemd/system/fprintd.service.d"
DROPIN_FILE="$DROPIN_DIR/goodix550c.conf"

if [ ! -f "$SRC" ]; then
    echo "ERROR: Build output files not found."
    echo "Missing compiled driver: $SRC"
    echo ""
    echo "The driver build must be completed before running this script."
    echo "Please run build-sysext.sh and verify-sysext.sh (in the sysext/ directory) first."
    exit 1
fi

[ "$(id -u)" -eq 0 ] || { echo "Must run with sudo/root"; exit 1; }

echo "==> Ensuring any previous systemd-sysext overlay is unmerged"
systemd-sysext unmerge 2>/dev/null || true

EXPECTED_DROPIN="[Service]
BindReadOnlyPaths=$DEST_FILE:/usr/lib64/libfprint-2.so.2.0.0"

if [ -f "$DEST_FILE" ] && [ -f "$DROPIN_FILE" ] && \
   cmp -s "$SRC" "$DEST_FILE" && [ "$(cat "$DROPIN_FILE" 2>/dev/null)" = "$EXPECTED_DROPIN" ]; then
    echo "==> Driver is already installed"
    echo "==> Verifying that the rest of the process is in place"
else
    echo "==> Creating $DEST_DIR"
    mkdir -p "$DEST_DIR"

    echo "==> Installing compiled driver to $DEST_FILE"
    cp -f "$SRC" "$DEST_FILE"

    echo "==> Writing systemd service drop-in to $DROPIN_FILE"
    mkdir -p "$DROPIN_DIR"
    cat > "$DROPIN_FILE" <<EOF
$EXPECTED_DROPIN
EOF
fi

echo "==> Setting file permissions and ownership"
chmod 755 "$DEST_FILE"
chown -R root:root /opt/goodix550c

echo "==> Setting SELinux contexts"
chcon -u system_u -t lib_t "$DEST_FILE" || true
chcon -u system_u -t lib_t "$DEST_DIR" || true

echo "==> Reloading systemd daemon"
systemctl daemon-reload

echo "==> Restarting fprintd"
systemctl restart fprintd

echo "==> Setup complete. Checking service status:"
systemctl status fprintd --no-pager
