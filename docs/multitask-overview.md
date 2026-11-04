# 太极多任务 Overview 设计文档

## 1. 目标

长按手柄西瓜键（Guide / Home）进入太极内的多任务 Overview：

- 顶部导航栏和状态栏保持原样
- 主内容区域显示任务卡片
- 中间卡片显示当前选中任务的真实截图
- 左右卡片缩小、变暗并露出边缘
- 手柄左右键切换任务，A 切换前台，B 退出 Overview
- 后续支持多个暂停的前台应用，以及非太极启动的外部 sway 窗口

## 2. 当前架构与限制

### 2.1 当前会话模型

- `taiji-run-app` 只管理一个外部应用
- 状态文件 `$XDG_RUNTIME_DIR/taiji/state` 只有一组：
  - `foreground`
  - `app_id`
  - `app_state`
  - `pid`
  - `pgid`
  - `on_return`
- `launcher` 在 `active` 时拒绝启动第二个应用
- `max_paused_apps` 目前只是设置值，还没有真正的多任务实现
- 外部应用使用固定 workspace：`app`
- 后台音乐使用固定 workspace：`music`
- 太极使用 workspace：`taiji`

### 2.2 当前返回逻辑

- `taiji-hotkeyd` 监听 `BTN_MODE` / `KEY_HOME` / `KEY_POWER`
- 西瓜键当前是按下立即触发 `taiji-return`
- `taiji-return` 会暂停/关闭当前应用，然后切换到 `taiji` workspace
- 已修复：没有 `SWAYSOCK` 时，会从 `$XDG_RUNTIME_DIR/sway-ipc.*` 自动寻找 sway socket

### 2.3 当前截图能力

- 太极本身没有外部窗口截图能力
- Wayland 需要 `grim`
- 后台 workspace 的窗口通常没有可读取的画面
- 只有当前可见窗口可以可靠实时截图

## 3. 任务数据模型

每个任务统一建模，不区分“太极启动的应用”和“外部窗口”。

```text
Task
  id                 稳定标识
  kind               taiji | app | game | music | external
  con_id             sway 容器 ID（外部窗口）
  pid                进程 PID
  pgid               进程组
  app_id             .desktop ID 或 sway app_id
  title              显示名称
  workspace          sway workspace 名称
  icon               图标路径
  screenshot         缩略图路径
  state              foreground | paused | background | closed
  on_return          pause | run | close
  focused            是否当前前台
```

### 3.1 任务来源

1. **太极自身**
   - 固定一张“主界面”卡片
2. **太极启动的应用/游戏**
   - 由 `taiji-run-app` 维护
3. **后台音乐**
   - 由会话脚本维护
4. **外部窗口**
   - 通过 `swaymsg -t get_tree` 枚举
   - 按 PID 与太极启动的任务合并，避免重复卡片

### 3.2 外部窗口字段

从 sway tree 提取：

- `con_id`
- `pid`
- `app_id`
- `name`
- `rect`
- `workspace`
- `focused`
- `visible`

过滤：

- 太极自己的窗口
- SDDM / greeter / 桌面壳
- 无效或隐藏的无标题容器
- 同一个 PID 的多余子窗口（按主窗口去重）

## 4. 多暂停应用模型

### 4.1 Workspace 分配

不再让所有外部应用共用 `app` workspace。

建议：

```text
taiji
taiji-app-1
taiji-app-2
taiji-app-3
music
```

- 每个太极启动的应用分配独立 workspace
- workspace 名称写入任务记录
- 切换任务时：
  1. 暂停当前受管理应用
  2. 恢复目标任务
  3. 切换到目标 workspace

### 4.2 状态文件

从单文件单任务扩展为任务目录：

```text
$XDG_RUNTIME_DIR/taiji/tasks/
  <task-id>.task
```

每个 `.task` 使用简单 `key=value` 格式，shell 和 C 都容易读写。

或者保留一个聚合 `tasks.tsv`，但并发写入风险更高。优先使用一任务一文件。

### 4.3 暂停/恢复

- 太极启动的应用：使用现有 `SIGSTOP` / `SIGCONT` 机制
- 外部非太极窗口：不强制暂停，直接通过 sway focus 切换
- 需要限制最大暂停数时，使用 `max_paused_apps` 淘汰最久未使用的任务

## 5. 截图方案

### 5.1 依赖

- `grim`：Wayland 截图
- `jq`：解析 `swaymsg -t get_tree`
- 可选 `imagemagick`：缩放/裁剪兜底

### 5.2 截图时机

原则：在窗口可见时截图。

- 长按西瓜键时：截当前前台窗口
- 从受管理应用返回太极时：截当前应用
- 切走一个受管理应用时：如果还没有缩略图，则截图
- 外部窗口第一次被选中时：截图并缓存
- 没有缩略图时：回退到应用图标 + 名称

### 5.3 截图实现

1. `swaymsg -t get_tree` 获取当前 focused 容器 rect
2. `grim -g "x,y wxh" <thumb>` 截窗口区域
3. 缩略图存入：

```text
$XDG_RUNTIME_DIR/taiji/thumbs/<task-id>.png
```

### 5.4 已知限制

- 其他 workspace 的窗口无法直接实时截图
- 可选方案：
  - 缓存上次可见时的截图（推荐）
  - 临时切 workspace 截图后再切回（会闪，速度慢）
- 截图可能包含用户隐私内容，缓存目录必须是 `0700`，退出/登出时清理

## 6. 西瓜键短按/长按

### 6.1 按键行为

- 短按（< 600ms，松手触发）：返回太极，保持现有行为
- 长按（≥ 600ms）：进入 Overview

### 6.2 hotkeyd 改造

不能继续按下立即触发返回，否则长按会被短按逻辑吃掉。

建议状态：

```text
guide_down_at
guide_long_fired
```

流程：

1. 按下 Guide：
   - 记录时间
   - 不立刻执行返回
2. 按住超过 600ms：
   - 标记 long fired
   - 执行 overview 流程
3. 松手：
   - 如果 long fired：什么都不做
   - 否则：执行现有 `taiji-return`

### 6.3 Overview 进入流程

1. 识别当前前台窗口
2. 截当前窗口缩略图（grim）
3. 如果是受管理应用，按现有规则暂停
4. 切换到 `taiji` workspace
5. 写入：

```text
$XDG_RUNTIME_DIR/taiji/overview_request
```

6. 太极主循环轮询到请求后进入 Overview

## 7. Overview UI

### 7.1 布局

- 保持顶部导航栏和状态栏
- 状态栏仍可独立导航（左右选择、A 打开）
- 主内容区域：
  - 背景：太极现有壁纸/渐变
  - 中间：选中任务大卡片
  - 左右：相邻任务卡片缩小、半透明
  - 底部：任务名称、状态、操作提示

### 7.2 卡片内容

- 真实截图（优先）
- 应用图标 + 名称（无截图时）
- 状态角标：
  - 前台
  - 已暂停
  - 后台运行
- 当前选中卡片使用高亮边框/发光

### 7.3 操作

- 左右：切换焦点卡片
- A：切换到该任务
- B：退出 Overview，返回太极主界面
- 长按 Guide：从外部应用进入 Overview

### 7.4 动画

- 进入：背景淡入，卡片缩放 + 淡入
- 切换：卡片平滑位移/缩放
- 退出：淡出
- `reduce_motion` 开启时跳过动画

状态栏不参与缩放/淡出，始终固定。

## 8. 任务激活

### 8.1 太极卡片

关闭 Overview，显示主界面。

### 8.2 太极启动的应用

- 恢复目标任务：`SIGCONT`
- 暂停当前任务：`SIGSTOP`
- 切换到任务 workspace
- 更新 state

### 8.3 外部 sway 窗口

- 使用 `swaymsg [con_id=<id>] focus`
- 必要时先切换 workspace
- 不修改太极的 `launcher` 状态

### 8.4 后台音乐

- 切换到 `music` workspace
- 保持后台播放状态

## 9. 需要改动的文件

### 脚本

- `scripts/taiji-hotkeyd` / `src/hotkeyd.c`
  - Guide 短按/长按
  - overview 请求
- `scripts/taiji-return`
  - 保留返回逻辑
  - 可选截图
- `scripts/taiji-run-app`
  - 独立 workspace
  - 多任务状态文件
- `scripts/taiji-resume`
  - 按 task-id 恢复
- 新增 `scripts/taiji-tasks`
  - sway tree + 状态合并
- 新增 `scripts/taiji-overview`
  - 截图 + 切换 workspace + 写请求

### C 应用

- `src/app.c`
  - Overview 状态机
  - 请求文件轮询
  - 任务切换输入
- `src/ui_overview.c`（建议新增）
  - 卡片轮播 UI
  - 截图/图标显示
  - 动画
- `src/ui.h` / `src/app.h`
  - Overview 状态和控件
- `src/config.*`
  - 可选：`overview_animations`、任务数量限制
- `src/network` 无关

### 打包

- `PKGBUILD` 增加依赖：
  - `grim`
  - `jq`
- 保留 `imagemagick` 作为可选项

## 10. 实施阶段

### 阶段 1：当前应用 Overview

- 长按 Guide
- 截当前前台窗口
- 进入 Overview，只显示：
  - 太极
  - 当前应用
  - 后台音乐
- 左右切换、A/B
- 状态栏保持

### 阶段 2：多暂停应用

- 独立 workspace
- 任务目录
- 多任务恢复/暂停
- 卡片显示多个受管理应用

### 阶段 3：外部窗口

- `swaymsg -t get_tree`
- 外部窗口任务发现
- 与受管理任务去重
- 外部窗口截图/缓存
- 通过 con_id 切换

### 阶段 4：打磨

- 动画和低性能模式
- 无截图 fallback
- 任务上限和清理
- X11 fallback（可选）

## 11. 验证清单

- [ ] 短按 Guide 正常返回太极
- [ ] 长按 Guide 进入 Overview
- [ ] 状态栏仍然可用
- [ ] 当前应用有正确缩略图
- [ ] 左右切换卡片，卡片居中/缩放正确
- [ ] A 能切到浏览器/应用/音乐/太极
- [ ] B 能退出 Overview
- [ ] 多个暂停应用都能恢复
- [ ] 外部浏览器窗口能显示并切换
- [ ] 无截图时回退图标
- [ ] 320×240 / 480×320 / 1280×720 布局正常
- [ ] `reduce_motion` 生效
- [ ] 退出/登出后临时截图清理

## 12. 风险

- sway 后台窗口无法直接截图，必须依赖缓存或切 workspace
- 多任务需要重写当前单任务状态和 session 脚本
- 外部窗口 PID 与太极记录可能不一致，需要按 PID/con_id 双重去重
- 暂停浏览器等外部进程可能影响其子进程，外部窗口默认不暂停
- 截图包含隐私内容，必须使用私有运行时目录并及时清理
