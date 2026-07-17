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
    tools/extract-firmware.py <driver.exe | Wbdi.dll | directory | .zip> [-o out.bin]

Point it at Lenovo's fingerprint driver installer for your machine (a
Lenovo-modified Inno Setup .exe). Unpacking that needs `innoextract`, which
most distros package. An already-unpacked directory works too.

Then install it with:
    sudo tools/install-firmware.sh 550c-app.bin
"""
import argparse
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

# The 550c application build this driver has verified end-to-end.
FW_TAG = b"GF5288_GM168SEC_APP_13022"
FW_PAYLOAD_LEN = 23424
FW_IMAGE_LEN = 23436
FW_SHA256 = "7a230f035b09e07a243bf7dd8c5055c8c8e8faf85dc102fd6c4f7b6249da6e8c"

# Lenovo's fingerprint driver package for the Yoga 9 14IRP8 (support page
# DS560180). A plain static file: no authentication, no cookie, no click-through
# agreement. --download fetches it to your machine, from Lenovo, at your explicit
# request; nothing here mirrors or redistributes it.
#
# The digest below is the one Lenovo publishes on the support page (and it
# matches the file byte-for-byte), so a changed or tampered upstream file is
# reported rather than silently used.
#
# Other machines carrying a 550c ship their own package — find yours from your
# vendor's support site and pass the .exe as an argument instead.
LENOVO_PAGE = ("https://pcsupport.lenovo.com/us/en/products/laptops-and-netbooks/"
               "yoga-series/yoga-9-14irp8/downloads/"
               "ds560180-fingerprinter-driver-goodix-fpc-for-windows-11-64-bit-"
               "yoga-9-14irp8")
LENOVO_EXE_URL = "https://download.lenovo.com/consumer/mobiles/mfyo027fx76fy1f0.exe"
LENOVO_EXE_SHA256 = \
    "5acd915744cef27f94872c42b48123f33f323e36794fe6f95c4d62b4ad575a31"
LENOVO_EXE_LEN = 3671720


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


def walk_dir(path):
    for root, _, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            if os.path.getsize(full) > 64 * 1024 * 1024:
                continue
            with open(full, "rb") as fh:
                yield full, fh.read()


def is_inno_setup(blob: bytes) -> bool:
    return blob[:2] == b"MZ" and b"Inno Setup" in blob[:0x400000]


def candidates(path):
    """Yield (label, bytes) for each thing worth searching."""
    if os.path.isdir(path):
        yield from walk_dir(path)
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

    # Lenovo ships the driver as a Lenovo-modified Inno Setup installer, which
    # LZMA-compresses its payload — the firmware is not visible in the .exe
    # itself. Unpack it first if we can.
    if is_inno_setup(blob):
        if shutil.which("innoextract") is None:
            print(f"'{path}' is an Inno Setup installer; innoextract is needed to "
                  "unpack it.", file=sys.stderr)
            print("  install it (e.g. 'dnf install innoextract' / 'apt install "
                  "innoextract'), or unpack the installer elsewhere and point this "
                  "script at the resulting directory.", file=sys.stderr)
            return
        with tempfile.TemporaryDirectory() as tmp:
            print(f"unpacking Inno Setup installer '{os.path.basename(path)}'…")
            proc = subprocess.run(
                ["innoextract", "-e", "-s", "-d", tmp, path],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if proc.returncode != 0:
                print(f"error: innoextract failed: "
                      f"{proc.stderr.decode(errors='replace').strip()[:200]}",
                      file=sys.stderr)
                return
            yield from walk_dir(tmp)
        return

    yield path, blob


def download_installer(dest_dir):
    """Fetch Lenovo's driver installer, at the user's explicit request."""
    import urllib.request

    dest = os.path.join(dest_dir, os.path.basename(LENOVO_EXE_URL))
    print(f"downloading {LENOVO_EXE_URL}")
    print(f"  (Lenovo's driver package for the Yoga 9 14IRP8 — support page:")
    print(f"   {LENOVO_PAGE})")
    try:
        with urllib.request.urlopen(LENOVO_EXE_URL, timeout=120) as resp, \
                open(dest, "wb") as out:
            shutil.copyfileobj(resp, out)
    except Exception as e:
        print(f"error: download failed: {e}", file=sys.stderr)
        print(f"       Download it yourself from {LENOVO_PAGE}", file=sys.stderr)
        print("       and pass the .exe as an argument.", file=sys.stderr)
        return None

    blob = open(dest, "rb").read()
    digest = hashlib.sha256(blob).hexdigest()
    print(f"  {len(blob)} bytes, sha256 {digest}")
    if digest != LENOVO_EXE_SHA256:
        print("error: downloaded installer does not match the digest Lenovo "
              "publishes.", file=sys.stderr)
        print(f"       expected {LENOVO_EXE_SHA256}", file=sys.stderr)
        print("       Upstream may have published a new package. Verify it "
              "yourself and pass the .exe as an argument.", file=sys.stderr)
        return None
    print("  digest matches the one Lenovo publishes.")
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?",
                    help="driver .exe, Wbdi.dll, a directory, or a .zip")
    ap.add_argument("-o", "--output", default="550c-app.bin", help="output path")
    ap.add_argument("--download", action="store_true",
                    help="fetch Lenovo's Yoga 9 14IRP8 driver package first "
                         "(downloads from Lenovo to this machine)")
    args = ap.parse_args()

    if not args.source and not args.download:
        ap.error("give a source (driver .exe, Wbdi.dll, directory, .zip) "
                 "or --download")

    with tempfile.TemporaryDirectory() as dl_tmp:
        if args.download:
            got = download_installer(dl_tmp)
            if got is None:
                return 1
            args.source = got
        return extract_from(args.source, args.output)


def extract_from(source, output):
    for label, data in candidates(source):
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

        with open(output, "wb") as fh:
            fh.write(image)
        print(f"\nwrote {output}")
        print(f"install it with:  sudo tools/install-firmware.sh {output}")
        return 0

    print(f"error: no {FW_TAG.decode()} firmware found in '{source}'",
          file=sys.stderr)
    print("       Point this at Lenovo's fingerprint driver .exe, an unpacked "
          "copy of it, or Wbdi.dll —", file=sys.stderr)
    print(f"       or run with --download. Support page: {LENOVO_PAGE}",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
