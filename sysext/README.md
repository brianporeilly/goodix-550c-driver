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
no changes to fprintd itself.

## ⚠️ SELinux — read this before deploying

The merged `/usr` and `/usr/lib64` **directory inodes take their SELinux
labels from the extension tree** (topmost overlayfs layer). If the tree is
labeled `user_home_t`/`var_lib_t`, the merged `/usr` is too, and **every
confined domain loses access to `/usr` — sudo and PAM break system-wide**
(observed 2026-07 on this machine; upstream: systemd issue
[#34387](https://github.com/systemd/systemd/issues/34387), systemd-side fix
merged in v257 via [PR #35132](https://github.com/systemd/systemd/pull/35132),
but the *image* must still be labeled correctly at build time).

Fix (same recipe as the [fedora-sysexts](https://fedora-sysexts.github.io/)
project): relabel the **entire tree, directories included**, against host
policy — `setfiles -r <tree> file_contexts <tree>/usr` + `chcon
--user=system_u -R`. `verify-sysext.sh` does this and then proves the result
with a simulated merge (overlayfs in a throwaway user namespace, no root, no
effect on the real `/usr`) before you ever touch the system.

## Workflow

```sh
# 1. Build — inside the build container (distrobox enter fpdev):
SYSEXT_ID=bazzite SYSEXT_VERSION_ID=44 ./build-sysext.sh

# 2. Verify — on the HOST, unprivileged; labels the tree + simulates the merge:
./verify-sysext.sh

# 3. Deploy TRANSIENTLY (to /run/extensions — a reboot fully undoes it):
sudo ./deploy-sysext.sh
#    keep that root shell open; from another terminal: sudo -v,
#    fprintd-enroll, fprintd-verify

# 4. Once happy, make it permanent (survives reboots via systemd-sysext.service):
sudo ./deploy-sysext.sh --persist

# Revert at any time:
sudo ./uninstall-sysext.sh
```

Build requires a `../libfprint` v1.94.10 tree with the driver installed
(`goodix53x5-libfprint/install.sh`). **Build against your host's exact
libfprint version** so the soname/ABI matches. Match `extension-release` to
your host (`ID`/`VERSION_ID` from `/etc/os-release`).

Then enable fingerprint auth:

```sh
sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes
```

## Notes

- **Version pinning:** the build is ABI-tied to the host libfprint version. If a
  host update bumps libfprint, rebuild against the new version and redeploy.
  A major-version rebase (VERSION_ID change) makes systemd skip the extension
  (fail-safe mismatch) until it's rebuilt.
- The deploy script relabels again at the install path and refuses to merge if
  labels are wrong; post-merge it asserts `/usr` is `usr_t` and auto-unmerges
  if not, then scans for fresh AVC denials.
- The bundled OpenCV libs land in `/usr/lib64` (globally visible). A cleaner
  packaging would confine them under a private dir with an RPATH on libfprint.
