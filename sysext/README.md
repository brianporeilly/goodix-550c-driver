# Host deployment via systemd-sysext

Overlays the host's `/usr/lib64/libfprint-2.so` with a build that includes the
goodix53x5 driver (with 550c support) plus the bundled OpenCV runtime the SIGFM
matcher needs — without `rpm-ostree` layering. Designed for immutable
Fedora/ublue hosts (Bazzite). Fully reversible.

## How it works

`systemd-sysext` merges an extension image into `/usr` via overlayfs, so files
in the extension shadow the base ones. This ships only:

```
goodix550c-fp/usr/lib64/libfprint-2.so.2.0.0   # full-driver build + 550c
goodix550c-fp/usr/lib64/lib{opencv_*,openblas,tbb,gfortran}.so*  # SIGFM deps
goodix550c-fp/usr/lib/extension-release.d/extension-release.goodix550c-fp
```

`fprintd` (base OS) links `libfprint-2.so.2`, so the overlay is picked up with
no changes to fprintd itself. `systemd-sysext.service` is enabled, so the
overlay re-merges on every boot.

## Build

Match `extension-release` to your host (`ID` / `VERSION_ID` from
`/etc/os-release`). Inside the build container (`distrobox enter fpdev`):

```sh
SYSEXT_ID=bazzite SYSEXT_VERSION_ID=44 ./build-sysext.sh
```

Requires a `../libfprint` v1.94.10 tree with the driver installed
(`goodix53x5-libfprint/install.sh`). **Build against your host's exact
libfprint version** so the soname/ABI matches.

## Deploy / revert (root)

```sh
sudo ./deploy-sysext.sh      # install + merge + relabel + restart fprintd
sudo ./uninstall-sysext.sh   # unmerge + remove; restores stock libfprint
```

Then, as your user with the reader on the host:

```sh
fprintd-enroll
fprintd-verify
sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes
```

## Notes

- **SELinux (enforcing):** the deploy script `chcon -t lib_t` the bundled
  libraries so `fprintd` can map them under the overlay. If enroll fails, check
  `sudo ausearch -m avc -ts recent`.
- **Version pinning:** the build is ABI-tied to the host libfprint version. If a
  host update bumps libfprint, rebuild against the new version and redeploy.
- The bundled OpenCV libs land in `/usr/lib64` (globally visible). A cleaner
  packaging would confine them under a private dir with an RPATH on libfprint.
