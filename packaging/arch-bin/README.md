# Arch Linux / pacman: prebuilt aarch64

This package installs a prebuilt aarch64 payload and does not compile on the
target. It is intended for RK3326-class ARM64 images.

## Build

From the repository root:

```bash
./scripts/make-arch-bin-payload.sh
# update the sha256 in PKGBUILD / .SRCINFO when the payload changes
./scripts/build-arch-bin-package.sh
```

The payload script requires Docker and the `taiji-build-env:bookworm` image.
It keeps `taiji-payload-env` and its CMake build directory, so later payloads are
incremental. The package script prepares `taiji-pkgbuild-env:archlinuxarm` once; later builds
reuse that image instead of downloading `base-devel` / `fakeroot` again.

## Architecture

The payload is aarch64 only. For x86_64, rebuild the payload on that architecture; the aarch64 payload
cannot be reused.

## Runtime ABI

The current payload is built against Debian bookworm glibc 2.36
(maximum symbol version GLIBC_2.34) and SDL2 `libSDL2-2.0.so.0`.
Arch Linux ARM rolling is expected to satisfy this, but verify on the target.

## Installed layout

The same layout as the source package:

```text
/usr/bin/taiji
/usr/libexec/taiji/
/usr/share/taiji/
/usr/share/sddm/themes/taiji/
/usr/share/wayland-sessions/taiji-wayland.desktop
/usr/share/xsessions/taiji-x11.desktop
/etc/sddm.conf.d/taiji.conf
/usr/lib/systemd/user/taiji-hotkeyd.service
/usr/lib/udev/rules.d/99-taiji-input.rules
```
