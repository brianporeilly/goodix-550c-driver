#!/bin/bash
# Deploy the goodix550c libfprint build as a systemd-sysext overlay so the
# host's fprintd/PAM use it. Run as root, AFTER verify-sysext.sh passes.
#
# Default = TRANSIENT deploy to /run/extensions: survives fprintd restarts but
# NOT a reboot, so even a worst-case /usr breakage is fully undone by
# rebooting. Once tested (fprintd-enroll/verify + sudo -v from another
# terminal), make it permanent with:  deploy-sysext.sh --persist
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/goodix550c-fp"
FC=/etc/selinux/targeted/contexts/files/file_contexts

PERSIST=0
[ "${1:-}" = "--persist" ] && PERSIST=1
if [ $PERSIST -eq 1 ]; then DST=/var/lib/extensions/goodix550c-fp; else DST=/run/extensions/goodix550c-fp; fi

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo."; exit 1; }
[ -d "$SRC/usr" ] || { echo "Missing $SRC — run build-sysext.sh first."; exit 1; }

echo "==> Unmerging any previous extensions"
systemd-sysext unmerge 2>/dev/null || true
rm -rf /run/extensions/goodix550c-fp /var/lib/extensions/goodix550c-fp

echo "==> Installing extension to $DST"
mkdir -p "$(dirname "$DST")"
cp -a "$SRC" "$DST"
chown -R root:root "$DST"

# CRITICAL under SELinux enforcing: the merged /usr and /usr/lib64 directory
# inodes take their labels from OUR tree (topmost overlayfs layer). Everything
# — directories included — must match host policy or all confined domains
# lose /usr and sudo/PAM break system-wide. verify-sysext.sh proves this
# out-of-band; we relabel again here at the final path as belt-and-braces.
echo "==> Relabeling $DST against host policy"
/usr/sbin/setfiles -r "$DST" "$FC" "$DST/usr"
chcon --user=system_u -R "$DST/usr"

type_of() { stat -c %C "$1" | cut -d: -f3; }
[ "$(type_of "$DST/usr")" = usr_t ] || { echo "ABORT: $DST/usr is $(type_of "$DST/usr"), not usr_t"; exit 1; }
[ "$(type_of "$DST/usr/lib64")" = lib_t ] || { echo "ABORT: $DST/usr/lib64 is not lib_t"; exit 1; }

echo "==> Merging extension into /usr"
systemd-sysext merge
systemd-sysext status

echo "==> Post-merge checks"
merged_usr=$(type_of /usr)
merged_lib=$(stat -c '%C %s' /usr/lib64/libfprint-2.so.2.0.0)
echo "    /usr = $merged_usr ; libfprint = $merged_lib"
if [ "$merged_usr" != usr_t ]; then
  echo "ABORT: merged /usr mislabeled ($merged_usr) — unmerging NOW."
  systemd-sysext unmerge
  exit 1
fi
sleep 1
if ausearch -m avc -ts recent 2>/dev/null | grep -q 'avc: *denied'; then
  echo "WARNING: fresh AVC denials — review them; 'systemd-sysext unmerge' to roll back:"
  ausearch -m avc -ts recent | tail -20
fi

echo "==> Restarting fprintd"
systemctl restart fprintd 2>/dev/null || pkill -f fprintd 2>/dev/null || true

if [ $PERSIST -eq 1 ]; then cat <<'MSG'

Deployed PERSISTENTLY (/var/lib/extensions). systemd-sysext.service re-merges
it on every boot. Remove with uninstall-sysext.sh.
MSG
else cat <<'MSG'

Deployed TRANSIENTLY (/run/extensions) — a reboot removes it completely.

KEEP THIS ROOT SHELL OPEN, then from ANOTHER terminal as your normal user:
  sudo -v                # proves PAM/sudo still work under the merged /usr
  fprintd-enroll         # enroll a finger via the overlaid driver
  fprintd-verify
If anything misbehaves: back in this shell, 'systemd-sysext unmerge'
(or just reboot — the transient extension vanishes).

When satisfied, make it permanent:
  sudo ./deploy-sysext.sh --persist
Then enable fingerprint auth (already enabled if authselect shows with-fingerprint):
  sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes
MSG
fi
