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
| PSK | all-zero PSK written to sensor | factory **per-machine PSK** (baked in; see below) |
| USB | interface 1, EP 0x03/0x81 | interface **0**, EP **0x01/0x83** |

New files: `drivers/goodix53x5/goodix53x5-tls.c` / `.h` (the TLS-PSK engine).
Everything else is the AndyHazz driver with variant branches added.

## Status

- ✅ **Device open works on real hardware**: init, TLS-PSK handshake, config
  upload, sleep — `fp_device_open_sync` returns OK.
- ⏳ Enroll / verify (scan + image capture + SIGFM) — in progress.

## The PSK is machine-specific

The 550c stores its PSK DPAPI-wrapped in the sensor; Windows decrypts it at
runtime. `goodix_550c_psk` in `drivers/goodix53x5/goodix53x5-session.c` holds
the value recovered from **one specific laptop**. Other 550c units must recover
their own PSK (see the reverse-engineering repo) and replace that constant.

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
