#pragma once
// ============================================================
// PluginConfig.h —— 玩家可自定义的运行参数（存 PeripheralBattery.ini [Advanced]）
//  - RefreshConfig：四类设备的刷新 / 休眠判定间隔（毫秒）
//  - DisplayConfig：电量阈值、配色、圆环倍率、间距、开关、空状态文案
//  - TextConfig：任务栏设备显示名
// 读取线程每轮取一次快照，界面修改后最迟一个等待周期生效。
// ============================================================
#include <windows.h>
#include <string>
#include <mutex>

struct RefreshConfig
{
    // 键盘
    int kb_wake_ms = 60000;      // 闲置超过该时长后的第一次按键视为唤醒
    int kb_active_ms = 30000;    // 最近该时长内有按键视为“使用中”
    int kb_refresh_ms = 5000;    // 使用中刷新间隔
    int kb_cooldown_ms = 2000;   // 两次查询之间的最小间隔
    int kb_reconnect_ms = 3000;  // 打不开设备时的重试间隔
    // 鼠标
    int ms_wake_ms = 120000;
    int ms_active_ms = 30000;
    int ms_refresh_ms = 5000;
    int ms_cooldown_ms = 2000;
    // G7 Pro
    int g7_offline_ms = 90000;      // 无消息超过该时长视为断开
    int g7_reset_ms = 60000;        // 无消息超过该时长重新学习设备 ID
    int g7_reconnect_ms = 3000;     // 打不开 GIP 总线的重试间隔
    int g7_read_timeout_ms = 1000;  // 单次总线读超时
    // DualSense
    int ds_pnp_ms = 2000;    // 在位检查间隔
    int ds_read_ms = 60000;  // 电量读取间隔
};

struct DisplayConfig
{
    int level_high = 60;       // ≥ 该值为高电量色
    int level_low = 20;        // ≥ 该值为中电量色，低于为低电量色
    int ring_scale_x10 = 15;   // 数字/圆环缩放 = 字高 × ring_scale_x10 / 10（显示设置里叫「数字缩放」）
    int sep_w = 14;            // 设备之间的间距
    int pad_lr = 1;            // 左右内边距
    int num_gap = 2;           // 紧凑布局时圆环与右侧数字的间隙
    bool show_sep = true;      // 是否画设备之间的分隔线
    bool show_pct_sign = true; // 紧凑布局时环外数字是否带 %

    COLORREF green_dark = RGB(112, 205, 126);
    COLORREF green_light = RGB(56, 158, 68);
    COLORREF yellow_dark = RGB(255, 194, 64);
    COLORREF yellow_light = RGB(210, 136, 0);
    COLORREF red_dark = RGB(255, 116, 122);
    COLORREF red_light = RGB(214, 55, 66);

    std::wstring no_read = L"--";            // 无读数 / 未连接时的数值文本
};

struct TextConfig
{
    std::wstring name[4] = { L"G87", L"A5 Ultra", L"G7 Pro", L"DS5" };
};

class PluginConfig
{
public:
    static PluginConfig& Instance();

    void SetIniPath(const std::wstring& path);
    void Load();
    void Save();

    // 读取线程 / 绘制层使用：每次返回一份独立快照
    RefreshConfig Refresh() const;
    DisplayConfig Display() const;
    TextConfig Text() const;
    std::wstring Name(int dev) const;

    // 显示设置对话框应用时整体替换（内部会做范围校验）
    void SetAll(const RefreshConfig& refresh, const DisplayConfig& display, const TextConfig& text);

    // 颜色以 RRGGBB / #RRGGBB 文本表示
    static bool ParseColor(const std::wstring& text, COLORREF& out);
    static std::wstring FormatColor(COLORREF color);

private:
    PluginConfig() = default;
    PluginConfig(const PluginConfig&) = delete;

    void SanitizeLocked();

    mutable std::mutex m_mutex;
    std::wstring m_path;
    RefreshConfig m_refresh;
    DisplayConfig m_display;
    TextConfig m_text;
};
