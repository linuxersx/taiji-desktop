// SPDX-License-Identifier: GPL-3.0-or-later
#include "i18n.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_lang;

typedef struct {
    const char *zh;
    const char *en;
} Entry;

/* 常用界面文案。未收录的字符串保留中文，避免出现空白。 */
static const Entry ENTRIES[] = {
    /* 导航 / 首页 / 底部 */
    {"游戏库", "Library"},
    {"收藏", "Favorites"},
    {"应用", "Apps"},
    {"文件", "Files"},
    {"设置", "Settings"},
    {"我的收藏", "My Favorites"},
    {"最近游玩", "Recent"},
    {"全部游戏", "All Games"},
    {"还没有游戏", "No games yet"},
    {"添加 ROM 根目录，太极会自动识别平台封面与游戏资料。",
     "Add a ROM folder and Taiji will detect platforms, covers and metadata."},
    {"添加 ROM 根目录，太极会自动识别平台、封面与游戏资料。",
     "Add a ROM folder and Taiji will detect platforms, covers and metadata."},
    {"太极游戏库", "Taiji Library"},
    {"添加游戏目录", "Add game folder"},
    {"按 Y 打开工具菜单", "Press Y for tools"},
    {"A 添加目录", "A Add folder"},
    {"A 进入", "A Open"},
    {"B 退出", "B Exit"},
    {"X 搜索", "X Search"},
    {"Y 工具", "Y Tools"},
    {"A 打开   B 上级   X 多选   Y 菜单   Start 操作",
     "A Open   B Up   X Select   Y Menu   Start Actions"},
    {"A 打开  B 上级  Y 菜单", "A Open  B Up  Y Menu"},
    {"A 打开  B 上级  X 多选  Y 菜单", "A Open  B Up  X Select  Y Menu"},
    {"B 返回", "B Back"},
    {"B / Esc 返回", "B / Esc Back"},
    {"A 确认  B 返回", "A Confirm  B Back"},
    {"A 选择  B 上级", "A Select  B Up"},
    {"左右 切换列表    A 展开/连接    X 开关    Y 刷新    B 返回",
     "Left/Right Switch    A Expand/Connect    X On/Off    Y Refresh    B Back"},
    {"A 选择  X 开关  Y 扫描  B 返回", "A Select  X On/Off  Y Scan  B Back"},

    /* 设置分类 */
    {"模拟器", "Emulators"},
    {"显示", "Display"},
    {"手柄", "Controller"},
    {"系统", "System"},
    {"关于", "About"},
    {"游戏目录与扫描工具", "Game folders and scanning"},
    {"启动命令与模拟器", "Launch commands and emulators"},
    {"主题、缩放与界面效果", "Theme, scaling and effects"},
    {"确认键、互换与手柄测试", "Buttons, swaps and controller test"},
    {"默认运行方式与应用列表", "Run mode and app list"},
    {"时区与语言", "Time zone and language"},
    {"版本与运行信息", "Version and runtime info"},

    /* 显示设置 */
    {"界面缩放", "UI scale"},
    {"主题预设", "Theme"},
    {"游戏网格列数", "Game grid columns"},
    {"显示封面侧栏", "Cover side panel"},
    {"减少动效", "Reduce motion"},
    {"界面音效", "UI sounds"},
    {"界面音效音量", "UI volume"},
    {"全屏显示", "Fullscreen"},
    {"SDL 渲染器", "SDL renderer"},
    {"低内存模式", "Low memory mode"},
    {"自动深色时间", "Auto dark time"},
    {"自动浅色时间", "Auto light time"},
    {"自动（深色/浅色）", "Auto (dark/light)"},
    {"19:00（自动）", "19:00 (auto)"},
    {"07:00（自动）", "07:00 (auto)"},
    {"开启", "On"},
    {"关闭", "Off"},
    {"自动", "Auto"},
    {"自动 (", "Auto ("},

    /* 系统设置 */
    {"时区", "Time zone"},
    {"语言", "Language"},
    {"中文", "Chinese"},
    {"英文", "English"},
    {"当前时区", "Current time zone"},

    /* 主题 / 选择弹窗 */
    {"选择主题", "Choose theme"},
    {"选择语言", "Choose language"},
    {"确认", "Confirm"},
    {"取消", "Cancel"},
    {"返回", "Back"},
    {"操作帮助", "Help"},
    {"立即扫描游戏库", "Scan game library"},
    {"添加应用", "Add app"},
    {"扫描桌面应用", "Scan desktop apps"},
    {"排序方式", "Sort"},
    {"筛选游戏", "Filter"},
    {"界面缩放已更新", "UI scale updated"},
    {"主题已切换", "Theme changed"},
    {"已切换为自动主题", "Automatic theme enabled"},

    /* 网络与蓝牙 */
    {"网络与蓝牙", "Network & Bluetooth"},
    {"Wi-Fi", "Wi-Fi"},
    {"蓝牙", "Bluetooth"},
    {"可用网络", "Networks"},
    {"蓝牙设备", "Bluetooth devices"},
    {"已连接", "Connected"},
    {"未连接", "Not connected"},
    {"已关闭", "Off"},
    {"已配对", "Paired"},
    {"已连", "On"},
    {"配对", "Paired"},
    {"Y 刷新", "Y Refresh"},
    {"未发现设备，按 Y 重新扫描", "No devices. Press Y to scan again"},
    {"未发现网络，按 Y 重新扫描", "No networks. Press Y to scan again"},
    {"NetworkManager 不可用", "NetworkManager unavailable"},
    {"BlueZ 不可用", "BlueZ unavailable"},
    {"正在扫描 Wi-Fi...", "Scanning Wi-Fi..."},
    {"正在扫描蓝牙...", "Scanning Bluetooth..."},
    {"正在扫描 Wi-Fi 与蓝牙...", "Scanning Wi-Fi and Bluetooth..."},
    {"设备已连接", "Device connected"},
    {"正在扫描蓝牙", "Scanning Bluetooth"},
    {"正在扫描 Wi-Fi", "Scanning Wi-Fi"},
    {"Wi-Fi 已开启", "Wi-Fi on"},
    {"Wi-Fi 已关闭", "Wi-Fi off"},
    {"蓝牙已开启", "Bluetooth on"},
    {"蓝牙已关闭", "Bluetooth off"},

    /* 文件管理器 */
    {"个人收藏", "Favorites"},
    {"位置", "Locations"},
    {"共享", "Shares"},
    {"主目录", "Home"},
    {"桌面", "Desktop"},
    {"下载", "Downloads"},
    {"文档", "Documents"},
    {"图片", "Pictures"},
    {"音乐", "Music"},
    {"影片", "Movies"},
    {"根目录", "Root"},
    {"连接服务器…", "Connect to server…"},
    {"文件夹", "Folder"},
    {"名称", "Name"},
    {"修改日期", "Modified"},
    {"大小", "Size"},
    {"种类", "Type"},
    {"已在根目录", "Already at root"},
    {"目录不可用", "Folder unavailable"},
    {"目录不存在，请检查路径", "Folder does not exist"},
    {"新建文件夹", "New folder"},
    {"重命名", "Rename"},
    {"复制", "Copy"},
    {"剪切", "Cut"},
    {"粘贴", "Paste"},
    {"移到回收站", "Move to trash"},
    {"永久删除", "Delete permanently"},
    {"属性", "Properties"},
    {"正在挂载...", "Mounting..."},
    {"正在挂载设备", "Mount in progress"},

    /* 通用状态 */
    {"未安装可用的关联程序", "No associated application installed"},
    {"当前默认", "Default"},
    {"设为默认", "Set default"},
    {"上下 选择 / A 仅本次打开 / Y 设为默认 / B 返回",
     "Arrows Select / A Open once / Y Set default / B Back"},
    {"已用 %s 打开", "Opened with %s"},
    {"已用 %s 打开，并设为默认打开方式", "Opened with %s and set as default"},
    {"无法打开文件", "Cannot open file"},
    {"打开方式...", "Open with..."},
    {"打开方式", "Open with"},
    {"没有可用的应用", "No applications available"},
    {"文件夹不能选择打开方式", "Folders do not have an Open with action"},
    {"打开", "Open"},
    {"解压到此处", "Extract here"},
    {"压缩...", "Compress..."},
    {"压缩格式", "Archive format"},
    {"压缩包", "Archive"},
    {"请选择一个压缩文件", "Select an archive file"},
    {"未找到 7-Zip（7zz / 7z）", "7-Zip (7zz / 7z) not found"},
    {"无法创建解压目录", "Cannot create extraction folder"},
    {"正在解压...", "Extracting..."},
    {"正在压缩...", "Compressing..."},
    {"静音", "Mute"},
    {"未连接手柄", "No controller"},
    {"按下手柄按键", "Press a controller button"},
    {"没有游戏", "No games"},
    {"这个目录是空的", "This folder is empty"},
    {"输入 IANA 时区，例如 Asia/Shanghai；留空取消。",
     "Enter an IANA time zone, e.g. Asia/Shanghai. Leave empty to cancel."},
    {"时区已设置为 %s", "Time zone set to %s"},
    {"语言已切换为中文，太极和新启动的应用已生效。",
     "Language set to Chinese for Taiji and newly launched apps."},
    {"不可用", "Unavailable"},
    {"未知", "Unknown"},
    {"失败", "failed"},
    {"已保存", "Saved"},
    {"退出太极？", "Exit Taiji?"},
    {"当前设置已经自动保存。", "Your settings are saved automatically."},
    {"没有可恢复的应用", "No app to resume"},
    {"正在恢复应用", "Resuming app"},
    {"文件管理器未安装", "File manager is not installed"},
    {"正在扫描游戏库...", "Scanning game library..."},
    {"正在更新游戏库...", "Updating game library..."},
    /* OOBE */
    {"初始设置", "Initial setup"},
    {"第一次启动需要完成几项设置", "Finish a few settings to get started"},
    {"设置时区", "Set time zone"},
    {"让时间和所在地区一致", "Match the clock to your region"},
    {"选择喜欢的配色，稍后也可以修改", "Pick a color scheme, changeable later"},
    {"连接网络", "Connect to the network"},
    {"连接 Wi-Fi 后才能使用应用商店", "Wi-Fi is needed for the app store"},
    {"选择存放游戏 ROM 的目录", "Choose where your ROMs are stored"},
    {"其他设置", "Other settings"},
    {"最后确认显示与应用设置", "Confirm display and app settings"},
    {"未检测到手柄，可使用触摸操作", "No controller detected; touch works too"},
    {"按 {A} 开始", "Press {A} to start"},
    {"当前时间", "Current time"},
    {"当前网络", "Current network"},
    {"游戏目录", "Game folder"},
    {"已识别手柄：", "Controller detected: "},
    {"日期与时间", "Date and time"},
    {"自动时间同步", "Automatic time sync"},
    {"系统不支持时间同步", "Time sync is not supported"},
    {"自动时间同步已开启", "Automatic time sync enabled"},
    {"自动时间同步已关闭", "Automatic time sync disabled"},
    {"修改自动时间同步失败", "Failed to change automatic time sync"},
    {"修改自动时间同步失败，需要系统权限",
     "Failed to change automatic time sync; system permission is required"},
    {"已清除游戏目录", "Game folder cleared"},
    {"时区设置失败，已跳过", "Time zone failed; skipped"},
    {"输入 ROM 根目录，留空则清除；例如 /home/armdeck/ROMs",
     "Enter a ROM folder; leave empty to clear, e.g. /home/armdeck/ROMs"},
    {"所有设置会恢复默认，应用列表、文件收藏、共享书签和本地共享都会清空，游戏文件不会被删除。",
     "All settings return to defaults. The app list, file favorites, share bookmarks and local shares are cleared; game files are kept."},

    {"设置日期与时间", "Set date and time"},
    {"左右选择要修改的部分，上下调整数值；应用后会关闭自动时间同步。",
     "Left/Right choose a field, Up/Down change the value. Applying turns off automatic time sync."},
    {"年", "Year"},
    {"月", "Month"},
    {"日", "Day"},
    {"时", "Hour"},
    {"分", "Minute"},
    {"应用修改", "Apply"},
    {"左右 选择 / 上下 调整 / {A} 应用 / {B} 取消",
     "Left/Right select / Up/Down change / {A} Apply / {B} Cancel"},
    {"日期与时间已设置", "Date and time updated"},
    {"当前日期与时间", "Current date and time"},
    {"按 {Y} 调整", "Press {Y} to adjust"},
    {"所选时区", "Selected time zone"},
    {"北京", "Beijing"},
    {"东京", "Tokyo"},
    {"伦敦", "London"},
    {"纽约", "New York"},
    {"上下 选择 / {A} 应用 / {Y} 日期 / {B} 上一步",
     "Up/Down select / {A} Apply / {Y} Date / {B} Back"},
    {"说明", "Details"},
    {"当前值", "Current value"},
    {"完成后保存所有设置并进入太极。",
     "Saves all settings and enters Taiji."},
    {"自动会按屏幕尺寸选择合适缩放；手动可固定 100% / 125% / 150% 等档位。",
     "Auto picks a scale for the screen size; manual fixes it at 100%, 125%, 150% and so on."},
    {"限制帧率可以省电；自动会跟随屏幕刷新率。",
     "Limiting the frame rate saves power; Auto follows the display refresh rate."},
    {"降低内部渲染分辨率能提升性能，画面会略微变软。",
     "A lower internal render resolution improves performance with slightly softer images."},
    {"切到其他应用时最多保留几个在后台，超出后最早的会被关闭。",
     "Keep this many apps in the background when switching; the oldest are closed first."},
    {"时区与系统语言", "Time zone and system language"},
    {"系统与版本信息", "System and build information"},
    {"关于本机", "About this device"},
    {"开源许可", "Open source licenses"},
    {"还原太极设置", "Reset Taiji settings"},
    {"恢复默认设置并重走初始设置", "Restore defaults and run setup again"},
    {"还原太极设置？", "Reset Taiji settings?"},
    {"所有设置会恢复默认，游戏库缓存会清空并重走初始设置。游戏文件不会被删除。",
     "All settings return to defaults, the library cache is cleared and setup runs again. Your game files are kept."},
    {"输入 ROM 根目录，例如 /home/armdeck/ROMs",
     "Enter a ROM folder, e.g. /home/armdeck/ROMs"},
    {"未选择", "Not selected"},
    {"连接后才能使用应用商店", "Connect to use the app store"},
    {"扫描并连接无线网络", "Scan and join a wireless network"},
    {"下一步", "Next"},
    {"稍后也可以在控制中心连接", "You can also connect later from Control Center"},
    {"打开 Wi-Fi 设置", "Open Wi-Fi settings"},
    {"选择目录", "Choose folder"},
    {"输入路径，或按 {Y} 浏览选择", "Enter a path, or press {Y} to browse"},
    {"稍后也可以在文件页添加", "You can also add it later from Files"},
    {"完成后会扫描这个目录", "This folder will be scanned when you finish"},
    {"选择存放 ROM 的文件夹", "Choose the folder that holds your ROMs"},
    {"界面与系统语言", "Interface and system language"},
    {"界面和系统语言", "Interface and system language"},
    {"浅色", "Light"},
    {"深色", "Dark"},
    {"刷新率", "Refresh rate"},
    {"渲染分辨率", "Render resolution"},
    {"最大暂停应用", "Max paused apps"},
    {"个", "paused"},
    {"进入太极", "Enter Taiji"},
    {"设置已完成", "Setup complete"},
    {"游戏目录已选择", "Game folder selected"},
    {"输入 ROM 根目录，按 Y 可浏览选择", "Enter a ROM root, press Y to browse"},
    {"重新运行初始设置", "Run initial setup again"},
    {"重新设置语言、时区与显示", "Reconfigure language, time zone and display"},
    {"重新运行初始设置？", "Run initial setup again?"},
    {"会重新走一遍语言、时区、主题和目录设置，完成后保存。",
     "Walk through language, time zone, theme and folders again, then save."},
    {"{A} 开始", "{A} Start"},
    {"左右 选择 / {A} 下一步 / {B} 上一步", "Left/Right select / {A} Next / {B} Back"},
    {"上下 选择 / {A} 应用 / {B} 上一步", "Up/Down select / {A} Apply / {B} Back"},
    {"上下 选择 / {A} 确认 / {B} 上一步", "Up/Down select / {A} Confirm / {B} Back"},
    {"上下 选择 / 左右 修改 / {A} 确认 / {B} 上一步",
     "Up/Down select / Left/Right change / {A} Confirm / {B} Back"},
    {"上一步", "Back"},};

static const Entry PHRASES[] = {
    /* 长词优先，避免把“文件夹”拆成 File 夹 */
    {"连接服务器", "Connect to server"},
    {"文件夹", "Folder"},
    {"正在扫描", "Scanning"},
    {"正在恢复", "Resuming"},
    {"正在挂载", "Mounting"},
    {"已连接", "Connected"},
    {"未连接", "Not connected"},
    {"已关闭", "Off"},
    {"已开启", "On"},
    {"已配对", "Paired"},
    {"已保存", "Saved"},
    {"已切换", "Switched"},
    {"已更新", "Updated"},
    {"不可用", "Unavailable"},
    {"失败", "failed"},
    {"个目录", "folders"},
    {"个游戏", "games"},
    {"个应用", "apps"},
    {"个结果", "results"},
    {"个项目", "items"},
    {"个", ""},
    {"正在", ""},
    {"添加目录", "Add folder"},
    {"个任务", "tasks"},
    {"个文件", "files"},
    {"工具", "Tools"},
    {"刷新", "Refresh"},
    {"扫描", "Scan"},
    {"返回", "Back"},
    {"确认", "Confirm"},
    {"取消", "Cancel"},
    {"开启", "On"},
    {"关闭", "Off"},
    {"自动", "Auto"},
    {"主题", "Theme"},
    {"设置", "Settings"},
    {"时区", "Time zone"},
    {"语言", "Language"},
    {"文件", "File"},
    {"游戏", "Game"},
    {"应用", "App"},
    {"搜索", "Search"},
    {"名称", "Name"},
    {"大小", "Size"},
    {"属性", "Properties"},
    {"删除", "Delete"},
    {"复制", "Copy"},
    {"剪切", "Cut"},
    {"粘贴", "Paste"},
};

void i18n_set_language(int lang)
{
    g_lang = lang == 1 ? 1 : 0;
}

int i18n_language(void)
{
    return g_lang;
}

const char *i18n_language_name(int lang)
{
    return lang == 1 ? "English" : "中文";
}

static const char *lookup_exact(const char *text)
{
    for (size_t i = 0; i < sizeof(ENTRIES) / sizeof(ENTRIES[0]); i++) {
        if (strcmp(text, ENTRIES[i].zh) == 0) return ENTRIES[i].en;
    }
    return NULL;
}

static bool has_cjk(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0xE4 && c <= 0xE9) return true;
    }
    return false;
}

const char *tr(const char *text)
{
    if (!text || !*text || g_lang != 1) return text;
    /* 图标符号 + 空格 + 文案：保留图标，只翻译后半段。 */
    const char *space = strchr(text, ' ');
    if (space && !has_cjk(text, (size_t)(space - text))) {
        const char *suffix = space;
        while (*suffix == ' ') suffix++;
        if (*suffix) {
            const char *sub = tr(suffix);
            if (sub != suffix) {
                static char with_icon[2048];
                size_t prefix = (size_t)(suffix - text);
                if (prefix + strlen(sub) < sizeof(with_icon)) {
                    memcpy(with_icon, text, prefix);
                    snprintf(with_icon + prefix, sizeof(with_icon) - prefix, "%s", sub);
                    return with_icon;
                }
            }
        }
    }
    const char *hit = lookup_exact(text);
    if (hit) return hit;

    static char buf[2048];
    if (strlen(text) >= sizeof(buf)) return text;
    snprintf(buf, sizeof(buf), "%s", text);

    bool changed = false;
    for (size_t i = 0; i < sizeof(PHRASES) / sizeof(PHRASES[0]); i++) {
        const char *zh = PHRASES[i].zh;
        const char *en = PHRASES[i].en;
        size_t zl = strlen(zh);
        if (zl == 0) continue;
        char *p;
        while ((p = strstr(buf, zh)) != NULL) {
            size_t tail = strlen(p + zl);
            size_t el = strlen(en);
            if ((size_t)(p - buf) + el + tail + 1 > sizeof(buf)) break;
            memmove(p + el, p + zl, tail + 1);
            memcpy(p, en, el);
            changed = true;
            p += el;
        }
    }
    return changed ? buf : text;
}
