# Linux 集成 / rootfs 文档

本文给另一个项目提供安装、引用和 rootfs 集成所需的稳定信息。

## 1. 集成边界

太极目前是一个**独立可执行程序**，不提供 `libtaiji.so` 或 C API。

另一个项目可以引用：

```text
/usr/bin/taiji
Wayland / X11 session desktop 文件
SDDM 主题
systemd user service
udev rule
用户配置目录
```

不要把 `/usr/libexec/taiji/*` 当作稳定 API。它们是会话内部 helper，版本升级时可能调整；需要调用时优先使用 `/usr/bin/taiji` 和 session desktop。

## 2. pacman 包

项目只发布预编译 aarch64 包：

```text
taiji-bin   packaging/arch-bin
```

`taiji-bin`：

```text
provides = taiji
conflicts = taiji
arch = aarch64
```

当前预编译 payload 基于：

```text
glibc 2.36
最高符号版本 GLIBC_2.34
libSDL2-2.0.so.0
```

x86_64 需要在 x86_64 环境重新生成二进制 payload，不能直接使用 aarch64 包。

## 3. 安装

### 3.1 构建发布包

```bash
./scripts/make-arch-bin-payload.sh
# 更新 PKGBUILD / .SRCINFO 中的 sha256
./scripts/build-arch-bin-package.sh
```

`make-arch-bin-payload.sh` 使用持久化容器 `taiji-payload-env`，CMake 构建目录
保留在容器内；第一次全量编译，之后只编译改动部分。
第一次运行 `build-arch-bin-package.sh` 会创建
`taiji-pkgbuild-env:archlinuxarm` 镜像并安装 `base-devel` / `fakeroot`；
后续直接复用，不会重复下载依赖。

### 3.2 在目标系统上安装

```bash
sudo pacman -U packaging/arch-bin/taiji-bin-1.0.0-165-aarch64.pkg.tar.zst
```

### 3.3 安装到离线 rootfs

如果 rootfs 里已经有 pacman 数据库：

```bash
pacman --root "$ROOTFS" \
  --dbpath "$ROOTFS/var/lib/pacman" \
  --cachedir /var/cache/pacman/pkg \
  -U /path/to/taiji-bin-1.0.0-165-aarch64.pkg.tar.zst
```

如果 rootfs 还没有包数据库，直接解包 payload：

```bash
tar -xzf taiji-1.0.0-aarch64.tar.gz -C "$ROOTFS"
```

payload 解包后就是完整的：

```text
usr/
etc/
```

然后由 rootfs 构建系统自行满足 `depends` 中的库和会话依赖。

### 3.3 CMake 安装（开发机）

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DTAIJI_INSTALL_SDDM_THEME=ON \
  -DTAIJI_INSTALL_WAYLAND_SESSION=ON \
  -DTAIJI_INSTALL_X11_SESSION=ON \
  -DTAIJI_BUILD_HOTKEYD=ON

cmake --build build
sudo DESTDIR=/tmp/taiji-rootfs cmake --install build
```

这种方式不会安装 `/etc/sddm.conf.d/taiji.conf`，需要从下面复制：

```text
packaging/sddm/sddm.conf.d/taiji.conf
```

## 4. 安装后的文件

```text
/usr/bin/taiji
/usr/libexec/taiji/taiji-session
/usr/libexec/taiji/taiji-wayland-session
/usr/libexec/taiji/taiji-session-loop
/usr/libexec/taiji/taiji-run-app
/usr/libexec/taiji/taiji-run-background
/usr/libexec/taiji/taiji-return
/usr/libexec/taiji/taiji-resume
/usr/libexec/taiji/taiji-hotkeyd

/usr/share/taiji/assets/
/usr/share/taiji/sway.conf

/usr/share/applications/taiji.desktop
/usr/share/icons/hicolor/256x256/apps/taiji.png

/usr/share/wayland-sessions/taiji-wayland.desktop
/usr/share/xsessions/taiji-x11.desktop

/usr/share/sddm/themes/taiji/
/etc/sddm.conf.d/taiji.conf

/usr/lib/systemd/user/taiji-hotkeyd.service
/usr/libexec/taiji/taiji-power
/etc/systemd/logind.conf.d/taiji-power.conf
/usr/lib/udev/rules.d/99-taiji-input.rules

/usr/share/licenses/taiji/
/usr/share/doc/taiji/README.md
```

## 5. 系统配置

### SDDM

Wayland 是默认会话。`RememberLastSession=false` 防止上次选择 X11 后覆盖默认值。
虚拟键盘初始隐藏，点击密码框或让焦点进入密码框时显示；登录失败会自动重新显示。

包内已安装：

```ini
# /etc/sddm.conf.d/taiji.conf
[Theme]
Current=taiji

[Users]
DefaultSession=taiji-wayland.desktop
RememberLastSession=false
```

启用：

```bash
systemctl enable sddm
```

离线 rootfs：

```bash
systemctl --root "$ROOTFS" enable sddm
```

### NetworkManager

```bash
systemctl enable --now NetworkManager
```

离线：

```bash
systemctl --root "$ROOTFS" enable NetworkManager
```

### BlueZ

```bash
systemctl enable --now bluetooth
```

离线：

```bash
systemctl --root "$ROOTFS" enable bluetooth
```

### CPU 性能模式

```bash
systemctl enable --now power-profiles-daemon
```

没有该服务时，太极会回退到 ACPI `platform_profile` 或 `cpufreq` sysfs。

### 手柄 Guide / Home

```bash
systemctl --user enable --now taiji-hotkeyd
```

udev rule 已安装：

```text
/usr/lib/udev/rules.d/99-taiji-input.rules
```

规则内容：

```text
KERNEL=="event*", SUBSYSTEM=="input", TAG+="uaccess"
```

首次安装或升级后可以手动刷新：

```bash
udevadm control --reload-rules
udevadm trigger --subsystem-match=input
```

会话脚本也会尝试启动 `taiji-hotkeyd`；进程内有 flock，不会重复运行。

实现细节：

- 短按 Guide/Home（< 0.6 秒，松手时触发）：`taiji-return`
- 长按 Guide/Home（>= 0.6 秒）：`taiji-overview`
- `taiji-capture` 按会话类型截图：Wayland 用 `grim`，X11 用 `xwd` + `convert`，最后回退 `/dev/fb0`
- 每个受管应用写入 `$XDG_RUNTIME_DIR/taiji/tasks/<id>.task`，并分配 `taiji-app-N` 工作区
- 启动新应用时若暂停数达到 `max_paused_apps`，弹窗询问是否关闭最久未使用的任务
- `taiji-overview` 截取当前窗口、暂停受管应用、切回 `taiji` 工作区并写入
  `$XDG_RUNTIME_DIR/taiji/overview_request`
- 卡片顶部显示 `应用名称 (状态)`；按 X 两次关闭选中任务
- 启动应用前用 `taiji-capture --self` 缓存太极界面作为“太极”卡片预览
- 任务列表由 `taiji-tasks` 生成，合并 `taiji-run-app` 状态与
  `swaymsg -t get_tree` 中的外部窗口
- 切换到外部窗口使用 `swaymsg '[con_id=<id>]' focus`
- Wayland 截图需要 `grim`，X11 需要 `xwd` 和 ImageMagick
- `/dev/fb0` 作为最后后备，需要 udev `uaccess` 或 `video` 组权限

## 6. 启动方式

### 直接启动

```bash
taiji --kiosk
```

常用参数：

```text
--fullscreen      桌面全屏
--kiosk           无边框全屏，显示管理器会话使用
--windowed        强制窗口模式
--display NAME    opengl | opengles | vulkan | sdl | software
--renderer NAME   opengl | opengles2 | vulkan | software | auto
--help
--version
```

默认渲染后端是 OpenGL。

### 通过 SDDM

选择：

```text
Taiji Wayland
Taiji X11
```

对应：

```text
/usr/share/wayland-sessions/taiji-wayland.desktop
/usr/share/xsessions/taiji-x11.desktop
```

Wayland 会话实际执行：

```text
/usr/libexec/taiji/taiji-wayland-session
```

它启动 sway、hotkeyd 和 session loop。Xwayland 随包安装并由 sway 自动管理。

安装包不再提供 X11/openbox 用户会话。

### 前台应用和返回

`taiji-run-app`、`taiji-return`、`taiji-resume` 是内部 helper。它们通过：

```text
$XDG_RUNTIME_DIR/taiji/state
```

共享前台应用状态，并使用：

```text
swaymsg
```

切换工作区。`wmctrl` 只在自行编译 X11 兼容会话时使用，安装包不需要。

## 7. 用户配置和数据

### 配置目录

优先级：

```text
$TAIJI_CONFIG_DIR
$XDG_CONFIG_HOME/taiji
~/.config/taiji
```

### 文件

```text
config.ini          用户设置
apps.tsv            应用库
icons/              SVG 转换后的图标缓存
logs/               前台应用日志
```

### `config.ini`

主要键：

```text
root=...
last_platform=...
last_game=...
theme=0..3
sound=0/1
volume=0..100
ui_scale_auto=1
ui_scale=100..300
renderer=opengl|opengles2|vulkan
grid_columns=0..8
scan_on_start=0/1
show_cover_panel=0/1
low_power=0/1
app_mode=...
max_paused_apps=1..6
command.<platform_id>=...
```

`renderer` 可选 `opengl` / `opengles2` / `vulkan`，也可在设置 → 显示 → SDL 渲染器中修改，重启后生效。

`ui_scale_auto=1` 时按显示器高度自动选择缩放档位（720p=100%，900p=125%，
1080p=150%，1200p=175%，1440p=200%）。用户手动选择后写入
`ui_scale_auto=0`，之后始终使用 `ui_scale`。

`ui_scale` 是手动界面缩放百分比：

```text
100 = 1.00x
125 = 1.25x
150 = 1.50x
175 = 1.75x
200 = 2.00x
225 = 2.25x
250 = 2.50x
275 = 2.75x
300 = 3.00x
```

设置路径：

```text
设置 → 显示 → 界面缩放
```

缩放档位可手动选择至 3.00x；低分辨率下界面会非常紧凑，建议按实际屏幕选择。

Wayland 下采用两套缩放：

- 太极自身保持内部 LVGL 缩放，界面清晰
- 启动外部应用时，`taiji-scale` 临时设置当前 sway output scale
- 回到太极前恢复原 output scale
- 外部应用前台期间，Qt/GTK 环境变量保持 1.0，避免和合成器缩放叠加
- 启动/恢复/返回分别由 `taiji-run-app`、`taiji-resume`、`taiji-return` 处理

缩放目标写入 `$XDG_RUNTIME_DIR/taiji/app_scale`，工具包回退变量写入
`$XDG_RUNTIME_DIR/taiji/toolkit_scale`，前台切换状态写入
`$XDG_RUNTIME_DIR/taiji/output_scaled`。

X11 没有合成器全局缩放，继续使用：

- `QT_SCALE_FACTOR` / `QT_AUTO_SCREEN_SCALE_FACTOR`
- `GDK_SCALE` / `GDK_DPI_SCALE`
- `XCURSOR_SIZE`
- `xrandr --dpi` / `Xft.dpi`

第一版切换没有过渡动画。如果实机确认有闪屏，再增加淡入淡出层。

### 系统音量

音量键、音量控制卡和状态栏喇叭图标都作用于系统默认输出。按下音量键会自动弹出音量卡并同步滑块：

```bash
wpctl get-volume @DEFAULT_AUDIO_SINK@
wpctl set-volume @DEFAULT_AUDIO_SINK@ 50%
wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
```

sway 配置已经把全局音量键绑定到太极的信号接口：

```bash
pkill -USR1 -x taiji   # 音量+
pkill -USR2 -x taiji   # 音量-
pkill -HUP  -x taiji   # 静音
```

太极收到信号后走应用内音量逻辑，因此会更新系统音量并弹出音量卡。

外部应用或全屏游戏前台时，太极会通过 `$XDG_RUNTIME_DIR/taiji/osd.sock` 通知 `taiji-osd`：

- Wayland：`wlr-layer-shell` OVERLAY 层，全屏也能覆盖
- X11：override-redirect SDL 窗口，并用 XShape 做圆角和鼠标穿透
- 位置固定在右上角
- 最后一次调节 2 秒后自动隐藏

需要 PipeWire + WirePlumber。`config.ini` 的 `volume` 只控制太极自己的界面音效音量。

应用页在 Linux 下每 10 秒重新扫描 `.desktop`，安装新应用后无需重启太极。
列表为连续滚动网格，上/下/左/右移动焦点，上方向键可从第二行回到上一行。

### `apps.tsv`

制表符分隔，12 列：

```text
id
name
command
icon
mode: auto|resident|handoff
on_return: pause|run|close
backend: auto|wayland|x11
heavy: 0|1
background: 0|1
startup: 0|1
category
desktop_file
```

字段中不要使用制表符和换行。桌面 `.desktop` 导入项使用：

```text
desktop-<hash>
```

作为 ID。

### 运行时目录

```text
$XDG_RUNTIME_DIR/taiji/state
$XDG_RUNTIME_DIR/taiji/handoff
$XDG_RUNTIME_DIR/taiji/resume_screen
$XDG_RUNTIME_DIR/taiji/taiji-hotkeyd.lock
$XDG_RUNTIME_DIR/taiji/osd.sock
$XDG_RUNTIME_DIR/taiji/osd.lock
$XDG_RUNTIME_DIR/taiji/scale
$XDG_RUNTIME_DIR/taiji/thumbs/
$XDG_RUNTIME_DIR/taiji/logs/overview.log
```

没有 `XDG_RUNTIME_DIR` 时回退到：

```text
/tmp/taiji
```

### 文件管理器 / XDG Portal

```text
/usr/libexec/taiji/taiji-files
/usr/libexec/taiji/taiji-files-portal
/usr/share/xdg-desktop-portal/portals/taiji.portals
/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.taiji.service
```

D-Bus 名称：

```text
org.freedesktop.impl.portal.desktop.taiji
```

接口：

```text
org.freedesktop.impl.portal.FileChooser
```

安装后太极顶部导航新增“文件”页，内嵌在太极窗口内；导航栏空间不足时可左右滚动。
文件页采用 Finder 风格：左侧为个人收藏、挂载设备与共享文件夹，右侧为带日期/大小/种类
的列表。U 盘和 SD 卡通过 `udisksctl` 挂载；电脑共享通过 `gvfs-smb` 连接。
文件页 L2 显示/关闭隐藏文件，R2 刷新当前目录、设备和共享列表。
添加游戏目录、应用图标路径等输入框支持按 Y/Start 或点“浏览目录 / 浏览文件”进入内嵌
文件页选择，选择后回填路径。选择期间导航栏只保留“文件”可操作，其他栏目变灰锁定。
Portal 选择器仍使用独立的 `taiji-files` 窗口。

支持 OpenFile / SaveFile / SaveFiles，使用应用传入的 `filename`、`current_folder`
和 `directory` 选项。GTK 应用需要：

```bash
export GTK_USE_PORTAL=1
```

会话脚本已经自动导出。选择器 UI 使用当前屏幕最上层窗口，不切换工作区。

## 8. 环境变量

生产/集成相关：

| 变量 | 用途 |
|---|---|
| `TAIJI_CONFIG_DIR` | 覆盖配置目录 |
| `TAIJI_ASSETS` | 覆盖资源目录 |
| `TAIJI_BIN` | session 脚本使用的二进制路径 |
| `TAIJI_SESSION_MODE` | `wayland` / `x11` |
| `TAIJI_SWAY_CONFIG` | 覆盖 sway.conf |
| `TAIJI_OPENBOX_CONFIG` | 仅自行编译 X11 兼容会话时使用 |
| `TAIJI_HOTKEYD` | 覆盖 hotkeyd 路径 |
| `TAIJI_RETURN_CMD` | hotkeyd 触发的返回命令 |
| `TAIJI_BACKGROUND_CMD` | 可选后台音乐命令 |
| `TAIJI_STATE_FILE` | 覆盖前台应用状态文件 |
| `TAIJI_KIOSK` | 环境变量方式启用 kiosk |
| `TAIJI_FULLSCREEN` | 环境变量方式启用全屏 |
| `TAIJI_DISPLAY` / `TAIJI_RENDERER` | 渲染后端 |
| `TAIJI_LOW_POWER` | 强制低功耗 UI 模式 |
| `TAIJI_NO_HIDPI` | 禁用 2x 原生渲染 |

`TAIJI_TEST_*`、`TAIJI_SCREENSHOT*`、`TAIJI_WINDOW` 是测试/截图开关，不要写入生产镜像。

## 9. 依赖

pacman 会自动安装 `depends`。

核心运行库：

```text
sdl2
glibc
```

会话和桌面：

```text
sddm
sway
xorg-server
xorg-xwayland
```

功能：

```text
networkmanager
power-profiles-daemon
pipewire
wireplumber
librsvg
polkit
qt6-declarative
qt6-imageformats
qt6-wayland
hicolor-icon-theme
desktop-file-utils
```

可选：

```text
pipewire-pulse
mesa
noto-fonts-cjk
imagemagick
evtest
joystick
```

系统音量控制依赖 PipeWire / WirePlumber 提供的 `wpctl`。界面音效音量仍保存在
`config.ini` 的 `volume`，和系统音量互不影响。

## 10. 另一个项目如何引用

推荐只依赖这些稳定入口：

```text
/usr/bin/taiji
/usr/share/wayland-sessions/taiji-wayland.desktop
/usr/share/xsessions/taiji-x11.desktop
/etc/sddm.conf.d/taiji.conf
/usr/lib/systemd/user/taiji-hotkeyd.service
/usr/libexec/taiji/taiji-power
/etc/systemd/logind.conf.d/taiji-power.conf
```

示例：在另一个启动器里直接启动太极：

```bash
exec /usr/bin/taiji --kiosk
```

示例：只安装会话，不接管 SDDM 主题：

```bash
cmake -S . -B build \
  -DTAIJI_INSTALL_SDDM_THEME=OFF \
  -DTAIJI_INSTALL_WAYLAND_SESSION=ON
```

示例：把太极放进 rootfs 但不启用 SDDM：

```bash
tar -xzf taiji-1.0.0-aarch64.tar.gz -C "$ROOTFS"
# 由上层项目决定是否启用 sddm / NetworkManager / power-profiles-daemon
```

## 11. 验收清单

```bash
taiji --version
ldd /usr/bin/taiji | grep "not found"
systemctl status sddm
systemctl status NetworkManager
powerprofilesctl get
nmcli general status
systemctl --user status taiji-hotkeyd
evtest
```

显示：

```bash
SDL_VIDEODRIVER=wayland SDL_RENDER_DRIVER=opengl taiji --kiosk
SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=opengl taiji --kiosk
```

## 12. 已知限制

- 没有公共库 API
- 官方只支持 SDDM，不支持 LightDM
- 官方 Wayland 会话基于 sway；其他合成器需要自行适配
- X11 模式需要窗口管理器
- 预编译 aarch64 payload 依赖 glibc 2.34+ 和 SDL2 ABI
- 原始 Taiji/NovaRetro 素材的再分发条款需要单独确认
