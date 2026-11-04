# Third-party notices

The Taiji Desktop application code and the original project assets in this
repository are licensed under the GNU General Public License, version 3 or
later. See `LICENSE`.

Copyright (C) 2026 the Taiji Desktop project authors.

## LVGL 9.3
`third_party/lvgl` contains a trimmed copy of the LVGL source tree.
LVGL is licensed under the MIT License. See `third_party/lvgl/LICENCE.txt`.

## ZCOOL QingKe HuangYou
`assets/fonts/TaijiCJK.ttf` is the ZCOOL QingKe HuangYou typeface.
Copyright 2018 The ZCOOL QingKe HuangYou Project Authors.
Licensed under the SIL Open Font License 1.1. See `assets/fonts/OFL.txt`.

## Original Taiji / NovaRetro assets
`assets/platform_logos`, `assets/sounds` and `assets/images/splash.jpg` are
original works of the Taiji Desktop project authors and are distributed under
the same GPL-3.0-or-later terms as the application code.

The platform names and logos are trademarks of their respective owners. They
are used only to identify emulated platforms. This project is not affiliated
with, endorsed by, or sponsored by any console or platform vendor.

## SDL2
Linked as a system dependency. SDL2 is licensed under the zlib License.

## libpng
Linked as a system dependency. libpng is licensed under the libpng/zlib
License.

## GLib / GIO
Linked as system libraries. GLib and GIO are licensed under the GNU Lesser
General Public License, version 2.1 or later. They are dynamically linked and
not modified.

## Wayland
`wayland-client` and the generated layer-shell/input-method protocol code are
licensed under the MIT License.

## librime
Loaded dynamically at runtime (`dlopen`) as `librime.so.1`. librime is
licensed under the BSD 3-Clause License.

## uMTP-Responder (GPLv3)
`third_party/umtp-responder` is a vendored copy of uMTP-Responder
(https://github.com/viveris/uMTP-Responder), the MTP protocol implementation
used by `taiji-mtprd`. It is licensed under the GNU General Public License
version 3 - see `third_party/umtp-responder/LICENSE`.

It is built as a **separate executable**, not linked into the Taiji
application. The `taiji-mtpd` wrapper (`src/mtp/taiji_mtpd.c`) only generates
the responder config and locates the current user's home directory.

The complete corresponding source for uMTP-Responder is included in this
repository under `third_party/umtp-responder`.
