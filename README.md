# 太极 / Taiji

面向手柄的复古游戏前端与微型 Linux 桌面：扫描 ROM、启动模拟器，也提供应用库、文件管理、文本编辑和完整的桌面会话。

## 功能

- **游戏库**：多 ROM 根目录扫描，自动识别 60+ 平台，读取 Pegasus / gamelist 元数据与封面；收藏、最近游玩、游玩次数/时长、排序与筛选。
- **模拟器**：自动发现 RetroArch / DuckStation / PPSSPP / Dolphin / PCSX2 / melonDS / Ryujinx / MAME 等；可按平台或单个游戏覆盖启动命令，支持 libretro 核心选择。
- **初始设置（OOBE）**：首次启动依次设置欢迎、语言、时区、主题、网络、游戏目录、界面缩放/刷新率/渲染分辨率/最大暂停应用；完成后才写入配置。
- **时区与时间**：时区页显示所选时区及北京/东京/伦敦/纽约时间；可直接修改日期和时间；设置里提供「日期与时间」和「自动时间同步（NTP）」开关。
- **设置与重置**：重新运行初始设置；还原太极设置（清空配置、缓存、应用列表、文件收藏、共享书签和本地共享，游戏文件保留）。
- **应用与多任务**：导入 Linux `.desktop` 应用，前台独占、暂停/恢复；Guide/Home 返回太极；多任务概览。
- **文件与编辑**：内嵌文件管理器（局域网 SMB 扫描、mDNS、手动连接、共享管理、压缩包解压）；内置文本编辑器与屏幕键盘。
- **桌面会话**：SDDM + sway kiosk 会话，也可作为普通桌面程序运行；X11 / Wayland 兼容。
- **手柄与 UI**：Xbox / PlayStation / Switch 按键提示自适应；热插拔、摇杆/十字键连发、按键互换与自定义映射、手柄检测。
- **主题与缩放**：深空蓝、纸白、落日霓虹、森林复古，支持按时间自动切换；响应式布局，界面缩放、刷新率限制、渲染分辨率可调。
- **系统**：Wi-Fi / 蓝牙、音量、电池与 CPU/GPU 性能模式、电源菜单、SMB 共享、低内存模式。

## 依赖

构建依赖：

```text
CMake >= 3.16, pkg-config, C11 编译器
SDL2, libpng, glib2/gio
Linux 会话：wayland-client / wayland-scanner, libX11, libXext
```

Debian / Ubuntu：

```bash
sudo apt install build-essential cmake pkg-config libsdl2-dev libpng-dev \
  libglib2.0-dev libwayland-dev libx11-dev libxext-dev
```

Arch：

```bash
sudo pacman -S base-devel cmake pkgconf sdl2 libpng glib2 wayland libx11 libxext
```

运行依赖（完整列表见 `packaging/arch-bin/PKGBUILD`）：

- SDDM、sway、weston、xorg-server / xorg-xwayland
- armdeck-osk（独立屏幕键盘包）
- NetworkManager、BlueZ、power-profiles-daemon
- PipeWire + WirePlumber
- polkit、udisks2、gvfs（含 SMB）、samba、avahi
- grim、jq、7zip、librsvg、xdg-desktop-portal、desktop-file-utils、shared-mime-info
- Qt6（SDDM 主题）、hicolor / papirus 图标、flatpak、pacman

LVGL 已内置在 `third_party/lvgl`，无需联网下载。

## 构建

macOS：

```bash
brew install cmake sdl2
./scripts/build-macos.sh
open build/Taiji.app
```

Linux：

```bash
./scripts/build-linux.sh
./build/taiji
```

Linux Docker（不污染宿主机）：

```bash
./scripts/docker-linux-build.sh
# 产物：build/taiji-linux
```

## 安装与运行

```bash
sudo cmake --install build --prefix /usr
```

安装后会提供 SDDM 会话 `Taiji Wayland`（sway）和可选的 X11 兼容会话；在 SDDM 菜单选择即可。也可以直接运行 `taiji` 作为普通程序。

调试环境变量：

```bash
TAIJI_WINDOW=960x640 taiji      # 指定窗口尺寸
TAIJI_OOBE=1 taiji              # 强制进入初始设置
TAIJI_SKIP_OOBE=1 taiji         # 跳过初始设置
```

## Arch 预编译包（aarch64）

```bash
./scripts/make-arch-bin-payload.sh    # 构建 payload，并打印 sha256
# 把 sha256 同步到 packaging/arch-bin/PKGBUILD 和 .SRCINFO
./scripts/build-arch-bin-package.sh   # 生成 taiji-bin-*.pkg.tar.zst
```

`pkgrel` 与 `BUILD_NUMBER` 保持同步（`scripts/bump-build-number.sh`）。

## 配置与数据

- macOS：`~/Library/Application Support/Taiji/`
- Linux：`~/.config/taiji/`（可用 `TAIJI_CONFIG_DIR` 指定）
- 主要文件：`config.ini`、`library.tsv`、`apps.tsv`；日志在 `logs/`

## 许可

代码与资源以 **GPL-3.0-or-later** 发布；第三方组件与字体许可见 `THIRD_PARTY.md` 和 `assets/` 内的许可证文件。

- 主仓库：<https://github.com/linuxersx/taiji-desktop>
- 屏幕键盘：<https://github.com/linuxersx/armdeck-osk>
