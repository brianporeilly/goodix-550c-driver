#!/bin/bash
# Deploy the goodix550c libfprint build as a systemd-sysext overlay so the
# host's fprintd/PAM use it. Reversible with uninstall-sysext.sh. Run as root.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/goodix550c-fp"
DST=/var/lib/extensions/goodix550c-fp

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo."; exit 1; }
[ -d "$SRC" ] || { echo "Missing $SRC — run build-sysext.sh first."; exit 1; }

echo "==> Unmerging any previous extensions"
systemd-sysext unmerge 2>/dev/null || true

echo "==> Installing extension to $DST"
mkdir -p /var/lib/extensions
rm -rf "$DST"
cp -a "$SRC" "$DST"

# Under SELinux enforcing, the overlaid libraries must be lib_t or fprintd
# cannot map them.
echo "==> Labeling bundled libraries lib_t"
chcon -t lib_t "$DST"/usr/lib64/*.so* 2>/dev/null || true

echo "==> Merging extension into /usr"
systemd-sysext merge
systemd-sysext status

echo "==> Overlaid libfprint (expect our ~1.0M build, context lib_t):"
ls -lZ /usr/lib64/libfprint-2.so.2.0.0

echo "==> Restarting fprintd"
systemctl restart fprintd 2>/dev/null || true
pkill -f fprintd 2>/dev/null || true

cat <<'MSG'

Deployed. The systemd-sysext.service is enabled, so this re-merges on every boot.

Next (as your normal user, reader on the host):
  fprintd-enroll          # enroll a finger via fprintd
  fprintd-verify          # verify it
Then enable fingerprint for login/sudo:
  sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes

If enroll fails, check SELinux denials:
  sudo ausearch -m avc -ts recent
MSG
