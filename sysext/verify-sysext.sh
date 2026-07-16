#!/bin/bash
# Label + verify the built sysext tree on the HOST, without root and without
# touching the running system.
#
# Why this exists: a sysext is merged into /usr with overlayfs, and the merged
# directory inodes (/usr itself, /usr/lib64) take their SELinux label from the
# topmost layer that contains them — i.e. from OUR extension tree. If the tree
# is labeled user_home_t/var_lib_t, the merged /usr is too, and every confined
# domain loses access to /usr => sudo/PAM break system-wide (seen 2026-07;
# systemd issue #34387). Fix = label the whole tree, directories included, to
# match host policy (same recipe as the fedora-sysexts project).
#
# This script:
#   1. relabels the tree with setfiles against the host policy (works unprivileged)
#   2. simulates the systemd-sysext merge with an overlayfs mount in a throwaway
#      user namespace (same kernel codepath, zero effect on the real /usr)
#   3. asserts the merged view: /usr=usr_t, lib64=lib_t, sudo still sudo_exec_t,
#      and that our libfprint is the one being served
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
EXT="${1:-$HERE/goodix550c-fp}"
FC=/etc/selinux/targeted/contexts/files/file_contexts

[ -d "$EXT/usr" ] || { echo "No extension tree at $EXT — run build-sysext.sh first."; exit 1; }
[ -f "$FC" ] || { echo "No SELinux file_contexts at $FC — run this on the host, not in a container."; exit 1; }

# Base OS /usr of the booted ostree deployment: the ground truth for the
# no-shadowing check, and a clean lib source in case a bad merged extension
# is currently shadowing host libs (seen 2026-07-15: bundled old libselinux
# broke the host's setfiles while merged).
BASE=$(readlink -f "$(sed -n 's/.*ostree=\([^ ]*\).*/\1/p' /proc/cmdline)" 2>/dev/null)/usr
[ -d "$BASE/lib64" ] || BASE=/usr

echo "==> Relabeling $EXT against host policy"
env LD_LIBRARY_PATH="$BASE/lib64" /usr/sbin/setfiles -r "$EXT" "$FC" "$EXT/usr"
chcon --user=system_u -R "$EXT/usr"

fail=0
check() { # check <path> <expected_type>
  local t
  t=$(stat -c %C "$1" 2>/dev/null | cut -d: -f3) || t=MISSING
  if [ "$t" = "$2" ]; then echo "  OK   $1 = $2"; else echo "  FAIL $1 = $t (want $2)"; fail=1; fi
}

echo "==> No-shadowing check (bundled libs must NOT exist in the base OS /usr)"
for f in "$EXT"/usr/lib64/*; do
  b=$(basename "$f")
  case "$b" in libfprint-2.so*) continue;; esac
  if [ -e "$BASE/lib64/$b" ]; then
    echo "  FAIL $b shadows the base OS copy ($BASE/lib64/$b) — rebuild with fixed build-sysext.sh"
    fail=1
  else
    echo "  OK   $b (base OS lacks it)"
  fi
done

echo "==> Tree labels"
check "$EXT/usr" usr_t
check "$EXT/usr/lib64" lib_t
check "$EXT/usr/lib64/libfprint-2.so.2.0.0" lib_t
check "$EXT/usr/lib/extension-release.d/extension-release.goodix550c-fp" lib_t

echo "==> Simulated merge (overlayfs in a user namespace — does NOT touch /usr)"
ns_rc=0
# Merge against the pristine ostree base /usr: ground truth, and (unlike the
# live /usr) never itself an overlay — overlay-on-overlay fails in a userns.
unshare --user --map-root-user --mount env LD_LIBRARY_PATH="$BASE/lib64" bash -s -- "$EXT" "$BASE" <<'NSEOF' || ns_rc=$?
set -euo pipefail
EXT="$1"; BASE="$2"; fail=0
mnt=$(mktemp -d)
mount -t overlay overlay -o "lowerdir=$EXT/usr:$BASE,ro" "$mnt"
check() {
  local t
  t=$(stat -c %C "$1" 2>/dev/null | cut -d: -f3) || t=MISSING
  if [ "$t" = "$2" ]; then echo "  OK   merged ${1#$mnt} = $2"; else echo "  FAIL merged ${1#$mnt} = $t (want $2)"; fail=1; fi
}
check "$mnt" usr_t
check "$mnt/lib64" lib_t
check "$mnt/bin" bin_t
check "$mnt/bin/sudo" sudo_exec_t
check "$mnt/lib64/libfprint-2.so.2.0.0" lib_t
check "$mnt/lib64/libopencv_core.so.411" lib_t
# the merged view must serve OUR libfprint, not the host's
ours=$(stat -c %s "$EXT/usr/lib64/libfprint-2.so.2.0.0")
merged=$(stat -c %s "$mnt/lib64/libfprint-2.so.2.0.0")
if [ "$ours" = "$merged" ]; then echo "  OK   merged libfprint is ours ($merged bytes)"
else echo "  FAIL merged libfprint is $merged bytes, ours is $ours"; fail=1; fi
umount "$mnt"; rmdir "$mnt"
exit $fail
NSEOF
[ $ns_rc -eq 0 ] || fail=1

if [ $fail -eq 0 ]; then
  echo "PASS: tree labeled correctly; simulated merge presents a policy-clean /usr."
  echo "Next: sudo $HERE/deploy-sysext.sh   (transient — a reboot fully undoes it)"
else
  echo "FAIL: do NOT deploy this tree."; exit 1
fi
