#!/usr/bin/env bash
#
# Install the Goodix 550c application image so the driver's self-heal can find
# it. See the README section "Supplying the 550c application firmware" for what
# this file is, why the driver does not ship it, and how to obtain one.
#
# Usage: sudo tools/install-firmware.sh /path/to/550c-app.bin

set -euo pipefail

DEST=${DEST:-/etc/goodix550c-app.bin}
EXPECT_SHA=7a230f035b09e07a243bf7dd8c5055c8c8e8faf85dc102fd6c4f7b6249da6e8c
EXPECT_LEN=23436

if [ $# -ne 1 ]; then
  echo "usage: $0 /path/to/550c-app.bin" >&2
  exit 2
fi

SRC=$1

if [ ! -f "$SRC" ]; then
  echo "error: '$SRC' does not exist" >&2
  exit 1
fi

len=$(stat -c %s "$SRC")
if [ "$len" != "$EXPECT_LEN" ]; then
  echo "error: '$SRC' is $len bytes, expected $EXPECT_LEN" >&2
  exit 1
fi

sha=$(sha256sum "$SRC" | cut -d' ' -f1)
if [ "$sha" != "$EXPECT_SHA" ]; then
  echo "error: '$SRC' has sha256 $sha" >&2
  echo "       expected              $EXPECT_SHA" >&2
  echo "       This is not the 550c application image the driver expects." >&2
  exit 1
fi

install -Dm644 "$SRC" "$DEST"
echo "installed $DEST (sha256 $sha)"
echo "self-heal will now be able to restore the sensor after erase-to-IAP."
