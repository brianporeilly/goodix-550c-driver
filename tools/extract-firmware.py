#!/usr/bin/env python3
"""Extract the Goodix 550c application image from Lenovo's driver package.

The image self-heal reflashes is Goodix's proprietary firmware, so this driver
does not ship it. It does not have to: the image is stored verbatim inside
Wbdi.dll in Lenovo's publicly downloadable fingerprint driver package, and the
12-byte header the Windows stack prepends before writing it is fully derived
from the payload. So this script rebuilds the exact image the sensor expects
from a copy of the vendor package you obtain yourself.

Layout, for the record:

    <version string> <payload>            in Wbdi.dll (.rdata)

    image = header || payload
      header[0:4]  = crc32-mpeg2(header[4:12])   # checksum over the next 8 bytes
      header[4:8]  = len(payload)                # uint32 LE
      header[8:12] = crc32-mpeg2(payload)        # uint32 LE

Usage:
    tools/extract-firmware.py <Wbdi.dll | driver.zip | directory> [-o out.bin]

Then install it with:
    sudo tools/install-firmware.sh 550c-app.bin
"""
import argparse
import hashlib
import io
import os
import struct
import sys
import zipfile

# The 550c application build this driver has verified end-to-end.
FW_TAG = b"GF5288_GM168SEC_APP_13022"
FW_PAYLOAD_LEN = 23424
FW_IMAGE_LEN = 23436
FW_SHA256 = "7a230f035b09e07a243bf7dd8c5055c8c8e8faf85dc102fd6c4f7b6249da6e8c"


def crc32_mpeg2(data: bytes) -> int:
    """CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no reflect, no xorout."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def build_image(payload: bytes) -> bytes:
    body = struct.pack("<II", len(payload), crc32_mpeg2(payload))
    return struct.pack("<I", crc32_mpeg2(body)) + body + payload


def find_payload(data: bytes):
    """Return the payload following the firmware version tag, or None."""
    i = data.find(FW_TAG)
    if i < 0:
        return None
    start = i + len(FW_TAG)
    payload = data[start:start + FW_PAYLOAD_LEN]
    if len(payload) != FW_PAYLOAD_LEN:
        return None
    return payload


def candidates(path):
    """Yield (label, bytes) for each thing worth searching."""
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            for name in files:
                full = os.path.join(root, name)
                if os.path.getsize(full) > 64 * 1024 * 1024:
                    continue
                with open(full, "rb") as fh:
                    yield full, fh.read()
        return

    with open(path, "rb") as fh:
        blob = fh.read()

    if zipfile.is_zipfile(io.BytesIO(blob)):
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            for name in z.namelist():
                if name.endswith("/"):
                    continue
                yield f"{path}!{name}", z.read(name)
        return

    yield path, blob


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="Wbdi.dll, the Lenovo driver .zip, or a directory")
    ap.add_argument("-o", "--output", default="550c-app.bin", help="output path")
    args = ap.parse_args()

    for label, data in candidates(args.source):
        payload = find_payload(data)
        if payload is None:
            continue

        image = build_image(payload)
        digest = hashlib.sha256(image).hexdigest()
        print(f"found {FW_TAG.decode()} in {label}")
        print(f"  payload : {len(payload)} bytes")
        print(f"  image   : {len(image)} bytes")
        print(f"  sha256  : {digest}")

        if digest != FW_SHA256:
            print(f"  expected: {FW_SHA256}", file=sys.stderr)
            print("error: rebuilt image does not match the known-good digest.",
                  file=sys.stderr)
            print("       This may be a different firmware build; the driver will "
                  "refuse it.", file=sys.stderr)
            return 1

        with open(args.output, "wb") as fh:
            fh.write(image)
        print(f"\nwrote {args.output}")
        print(f"install it with:  sudo tools/install-firmware.sh {args.output}")
        return 0

    print(f"error: no {FW_TAG.decode()} firmware found in '{args.source}'",
          file=sys.stderr)
    print("       Point this at Wbdi.dll from Lenovo's fingerprint driver package "
          "(or the .zip itself).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
