# Goodix 27c6:550c libfprint driver (work in progress)

A libfprint driver for the **Goodix `27c6:550c`** fingerprint sensor (GF5288,
108×88) found in the Lenovo Yoga 9 14IRP8 and similar laptops. There is no
other Linux driver for this device.

This is a **fork of [AndyHazz/goodix53x5-libfprint]** (@ `d37cf8c`), which
drives the closely-related 53x5 family (5335/5385/5395). The 550c is the same
sensor family and image/matching pipeline, but differs in three low-level ways
that this fork adds as a device *variant* (selected from the USB id_table):

| Layer | 53x5 (GTLS) | 550c (this fork) |
|---|---|---|
| Transport framing | bare messages, continuation markers | extra **pack layer** `0xA0/0xB0/0xB2`, raw 64-byte chunks |
| Secure channel | custom GTLS (PRF + GEA/AES) | **standard TLS 1.2-PSK** (in-driver OpenSSL memory BIOs) |
| PSK | all-zero PSK written to sensor | **all-zero PSK** once reprovisioned (see below) |
| USB | interface 1, EP 0x03/0x81 | interface **0**, EP **0x01/0x83** |

New files: `drivers/goodix53x5/goodix53x5-tls.c` / `.h` (the TLS-PSK engine) and
`goodix53x5-firmware550c.c` / `.h` (the application-image loader used by
self-heal). Everything else is the AndyHazz driver with variant branches added.

## Licensing

The driver code is LGPL-2.1-or-later, matching its parent
[AndyHazz/goodix53x5-libfprint] and the goodix-fp-linux-dev work both derive
from.

This repo contains **no vendor firmware**. The Goodix 550c application image
that self-heal reflashes is Goodix's proprietary, signed firmware; this project
holds no right to redistribute it, so it is loaded at runtime from a file the
machine's owner supplies. See
[Supplying the 550c application firmware](#supplying-the-550c-application-firmware).

## Status

- ✅ **Device open works on real hardware**: init, TLS-PSK handshake, config
  upload, sleep.
- ✅ **Enroll works**: 8-stage enroll completes, SIGFM template stored.
- ✅ **Verify works**: enrolled finger matches (SIGFM scores in the thousands vs
  a 150 threshold); a different finger is correctly rejected (score 0).
- ✅ **Automatic self-heal**: a unit still on a Windows-provisioned PSK is
  reprovisioned to the all-zero PSK on first open, with no manual steps — once
  the vendor application image is present locally (see
  [Supplying the 550c application firmware](#supplying-the-550c-application-firmware)
  and [Automatic self-heal](#automatic-self-heal)).

Verified end-to-end on real hardware, including through `fprintd`
(`fprintd-enroll` / `fprintd-verify`): the self-heal reprovisions a unit from a
Windows PSK to all-zero on first open, and enroll/verify then work normally.

## The PSK — reprovisioned to all-zero

The 550c protects its image channel with a TLS-PSK, and that PSK is **not a
fixed factory key**: the Windows Goodix stack generates a fresh random PSK each
time it provisions the sensor and stores it DPAPI-wrapped, which Windows
decrypts at runtime. So whatever PSK a unit currently holds is just the one its
last Windows provisioning wrote. The reverse-engineering work established that
the sensor can be **reprovisioned from Linux to the same all-zero PSK the rest
of the Goodix 51x0/52xd/55x4 family uses** (write the family white-box to the PSK
slot inside a container write, then reflash). Once reprovisioned, the sensor is
fully plug-and-play: this driver **defaults to the all-zero PSK** and needs no
configuration.

A unit still holding a Windows-provisioned PSK can override the default with its
recovered value (until it is reprovisioned) via either:

- the `GOODIX550C_PSK` environment variable (64 hex chars), or
- the file `/etc/goodix550c.psk` (64 hex chars).

See `drivers/goodix53x5/goodix53x5-session.c` (`goodix_550c_load_psk`) and the
RE repo for how to recover a per-machine PSK and, preferably, reprovision.

## Supplying the 550c application firmware

Reprovisioning the PSK requires erasing the sensor's application firmware to
reach IAP mode, which means a valid application image has to be written back
afterwards. That image is **Goodix's proprietary, signed firmware**. It is not
part of this driver, is not covered by this driver's licence, and **is not
distributed here** — nobody in this project holds the rights to redistribute it.

So the driver loads it at runtime from a file you supply. It looks, in order,
at:

1. `$GOODIX550C_FIRMWARE` (an explicit path), then
2. `/etc/goodix550c-app.bin`, then
3. `/usr/lib/firmware/goodix/550c-app.bin`, then
4. `/usr/local/lib/firmware/goodix/550c-app.bin`.

The image is checked against a known length (23436 bytes) and SHA-256
(`7a230f03…49da6e8c`) before the driver will touch the device, and the
existing PSK-derived HMAC self-check runs on top of that. A missing, truncated
or mismatched file disables self-heal and is reported in the journal; it never
results in a half-erased sensor, because the load happens before the erase.

To install an image you have obtained:

```
sudo tools/install-firmware.sh /path/to/550c-app.bin
```

**Obtaining the image.** It is the application image the Windows Goodix stack
writes to the sensor during provisioning, and it is recoverable from a USB
capture of that provisioning sequence on your own machine — that is how this
project got it. It is *not* stored verbatim in Lenovo's redistributable driver
package (`GoodixEngineAdapter.dll` and friends), so it cannot simply be
unpacked from the vendor download. See the RE repo for the capture procedure.

If your sensor is already on the all-zero PSK, you do not need this file at
all: self-heal is only for units still holding a Windows-provisioned PSK.

### Automatic self-heal

You normally don't reprovision by hand. When the all-zero TLS-PSK handshake
fails on open, **no PSK override is set**, and the vendor application image is
installed, the driver assumes the unit is still on a Windows-provisioned PSK and
self-heals: it reprovisions the sensor to the all-zero PSK, then the sensor is
usable on every subsequent open. A unit pinned to a recovered PSK (via the env
var or config file) is left untouched — self-heal only runs on the default path.

The application image is loaded and verified **first**, before any device state
changes. If it is missing or fails verification, self-heal declines to run and
logs why; the sensor is never left erased without a way back.

Before reflashing, the driver reads the sensor's PSK hash slot to decide what is
actually wrong:

- **Hash is a non-default PSK** → full reprovision: erase to the IAP bootloader,
  write the all-zero PSK container, reflash the app firmware, and commit. The
  IAP→app reset **re-enumerates the sensor on the USB bus**, so the open that
  triggered the heal completes with a "device removed / please retry" error.
  fprintd picks up the hotplugged sensor and reopens automatically; a CLI caller
  just runs the command again. This happens **once** per unit — thereafter the
  handshake succeeds directly.
- **Hash is already all-zero** → the PSK is correct and the handshake failure was
  transient (a freshly-reflashed sensor can fail its first handshake once). The
  driver **skips the reflash** and retries the open in place. No firmware writes.

All heal steps, including the live hash-slot value, are logged at warning level,
so `journalctl -u fprintd` shows exactly what happened, e.g.:

```
550c TLS-PSK handshake failed and no PSK override is set — self-healing: …
Self-heal: live PSK hash slot = 40258d6a… (32 bytes)
Self-heal: sensor is on a non-default PSK; reprovisioning it to the all-zero PSK
…
Self-heal: sensor reprovisioned to the all-zero PSK and re-enumerated …
```

Implementation: the heal state machine in `goodix53x5-session.c`
(`goodix_heal_ssm_handler`, armed from `goodix_maybe_start_selfheal`).

## Build (into a libfprint v1.94.10 tree)

```sh
git clone --depth 1 --branch v1.94.10 \
    https://gitlab.freedesktop.org/libfprint/libfprint.git
./install.sh ./libfprint          # copies driver + sigfm, applies meson patch
cd libfprint
meson setup builddir --prefix=/usr -Ddrivers=goodix53x5 \
    -Dudev_hwdb=disabled -Dudev_rules=disabled -Dintrospection=false \
    -Dinstalled-tests=false -Ddoc=false
ninja -C builddir
```

Build deps: meson, ninja, gcc/g++, glib2, libgusb (`gusb.pc`), OpenSSL 3.x,
OpenCV 4/5. Tested in a Fedora 42 container; host is Bazzite (libfprint 1.94.10,
so the build is ABI-compatible for a systemd-sysext overlay).

## Open test

`open_test.c` opens the first device and reports — exercises the full open SSM
without needing enroll. Compile against the build tree (see the RE repo notes)
and run with `LD_LIBRARY_PATH=<builddir>/libfprint G_MESSAGES_DEBUG=all`.

[AndyHazz/goodix53x5-libfprint]: https://github.com/AndyHazz/goodix53x5-libfprint
