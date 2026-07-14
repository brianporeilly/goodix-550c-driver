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

echo "==> Bundling OpenCV + non-core transitive deps"
# Never bundle core system libraries (they are always present on the host).
DENY='^(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux|libglib-2\.0|libgobject-2\.0|libgio-2\.0|libgusb|libssl|libcrypto|libz|libpng16|libjpeg)\.'
copy_deps() {
  local f="$1" dep b
  for dep in $(ldd "$f" 2>/dev/null | awk '/=>/{print $3}'); do
    b=$(basename "$dep")
    echo "$b" | grep -qE "$DENY" && continue
    [ -e "$EXT/usr/lib64/$b" ] && continue
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
echo "Built $EXT ($(du -sh "$EXT" | cut -f1)). Deploy with: sudo ./deploy-sysext.sh"
