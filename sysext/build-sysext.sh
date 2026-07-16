#!/bin/bash
# Build the goodix550c libfprint systemd-sysext image (sysext/goodix550c-fp/).
#
# Run inside the build container (distrobox 'fpdev') or any environment with:
#   meson ninja gcc gcc-c++ glib2-devel libgusb-devel openssl-devel opencv-devel
#
# Requires a libfprint v1.94.10 tree at ../libfprint with this driver already
# installed (goodix53x5-libfprint/install.sh). Set SYSEXT_ID / SYSEXT_VERSION_ID
# to match your host's /etc/os-release (defaults: bazzite / 44).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LFP="${LIBFPRINT_DIR:-$HERE/../libfprint}"
EXT="$HERE/goodix550c-fp"
STAGE="$HERE/.stage"

[ -f "$LFP/libfprint/meson.build" ] || { echo "No libfprint tree at $LFP"; exit 1; }

echo "==> Building full-driver libfprint (release)"
cd "$LFP"
[ -d sysextbuild ] || meson setup sysextbuild --prefix=/usr --libdir=lib64 \
  --buildtype=release -Dudev_hwdb=disabled -Dudev_rules=disabled \
  -Dintrospection=false -Dinstalled-tests=false -Ddoc=false
ninja -C sysextbuild
rm -rf "$STAGE"; DESTDIR="$STAGE" ninja -C sysextbuild install

echo "==> Assembling image tree"
rm -rf "$EXT"; mkdir -p "$EXT/usr/lib64" "$EXT/usr/lib/extension-release.d"
cp -a "$STAGE/usr/lib64/libfprint-2.so.2.0.0" "$EXT/usr/lib64/"
ln -sf libfprint-2.so.2.0.0 "$EXT/usr/lib64/libfprint-2.so.2"

echo "==> Bundling transitive deps the host base OS lacks"
# RULE: bundle a dependency ONLY if the host's /usr/lib64 does not provide it.
# Bundling a lib the host already has SHADOWS the host copy for every process
# (the container's build may be older: seen 2026-07-15 as libpcre2 "no version
# information available" warnings from grep/glib/flatpak host-wide, and a
# libselinux file_contexts.bin regex-version mismatch).
if [ -d /run/host/usr/lib64 ]; then HOSTROOT=/run/host; else HOSTROOT=; fi
if [ -z "${HOST_LIB64:-}" ]; then
  # Prefer the ostree base deployment: it is the true base OS content and is
  # unaffected by a currently-merged extension.
  for d in $(ls -td "$HOSTROOT"/sysroot/ostree/deploy/*/deploy/*/usr/lib64 2>/dev/null); do
    HOST_LIB64="$d"; break
  done
fi
if [ -z "${HOST_LIB64:-}" ]; then
  HOST_LIB64="$HOSTROOT/usr/lib64"
  # Live /usr lies about what the base provides while our extension is merged.
  if [ -e "$HOSTROOT/usr/lib/extension-release.d/extension-release.goodix550c-fp" ]; then
    echo "ERROR: goodix550c-fp sysext is currently MERGED and no ostree base"
    echo "found — 'sudo systemd-sysext unmerge' first, then rebuild."; exit 1
  fi
fi
echo "    host base lib64: $HOST_LIB64"
copy_deps() {
  local f="$1" dep b
  for dep in $(ldd "$f" 2>/dev/null | awk '/=>/{print $3}'); do
    b=$(basename "$dep")
    [ -e "$HOST_LIB64/$b" ] && continue     # host base provides it — never shadow
    [ -e "$EXT/usr/lib64/$b" ] && continue  # already bundled
    echo "    bundling $b (host lacks it)"
    cp -L "$dep" "$EXT/usr/lib64/$b"
    copy_deps "$dep"
  done
}
copy_deps "$EXT/usr/lib64/libfprint-2.so.2.0.0"

echo "==> extension-release (host match)"
cat > "$EXT/usr/lib/extension-release.d/extension-release.goodix550c-fp" <<EOF
ID=${SYSEXT_ID:-bazzite}
VERSION_ID=${SYSEXT_VERSION_ID:-44}
ARCHITECTURE=x86-64
EOF

if ldd "$EXT/usr/lib64/libfprint-2.so.2.0.0" | grep -q 'not found'; then
  echo "WARNING: some deps still unresolved (they must exist on the host):"
  ldd "$EXT/usr/lib64/libfprint-2.so.2.0.0" | grep 'not found'
fi
echo "Built $EXT ($(du -sh "$EXT" | cut -f1))."
echo "Next, ON THE HOST (not in the container): ./verify-sysext.sh   # labels + simulated-merge check"
echo "Then: sudo ./deploy-sysext.sh   # transient deploy; --persist once tested"
