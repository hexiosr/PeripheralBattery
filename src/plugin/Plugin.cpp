#include "Plugin.h"
#include "PluginConfig.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cstdio>

extern HMODULE g_hDllModule; // dllmain.cpp：本 DLL 模块句柄

// ============ 设备条目 ============
// 新增设备：1) 实现对应 Reader 并加入 Plugin 2) kDeviceCount +1
// 显示名不再写死在这里，改为玩家可在“显示设置”里修改（默认值见 PluginConfig）。
// 接收器 HID 产品字符串不一定是型号（G87 的接收器只报 "MCHOSE 2.4G Wireless"），故用固定名。

// 悬停提示的设备图标前缀（GDI 字体链接渲染为单色符号；
// emoji 为 UTF-16 代理对，\u 转义不允许代理区字符，须用 \x 逐值书写）
static const wchar_t* const kTipIconKb  = L"\u2328 ";       // ⌨
static const wchar_t* const kTipIconMs  = L"\xD83D\xDDB1 "; // 🖱
static const wchar_t* const kTipIconPad = L"\xD83C\xDFAE "; // 🎮

// 全部取消勾选：条目不能真的消失，否则任务栏上没有点击区域，右键进不来设置
static const wchar_t* const kPlaceholderText = L"外设电量";
static const wchar_t* const kPlaceholderTip =
    L"四台设备都未勾选，电量暂不显示。\n右键打开「显示设置」重新勾选。";

Plugin Plugin::m_instance;

Plugin::Plugin()
{
    // 预填显示数组：首次 DataRequired 之前，自绘也能按完整布局计算宽度
    for (int i = 0; i < kDeviceCount; ++i)
    {
        m_display[i].name = PluginConfig::Instance().Name(i);
        m_order[i] = kDefaultOrder[i];
    }
    m_display_count = kDeviceCount;
}

Plugin& Plugin::Instance()
{
    return m_instance;
}

IPluginItem* Plugin::GetItem(int index)
{
    switch (index)
    {
    case 0: return &m_item;    // 外设电量显示（组合键盘 / 鼠标 / 手柄）
    default: return nullptr;
    }
}

void Plugin::DataRequired()
{
    // 勾选才查询：按当前勾选启动各设备的后台读取线程
    SyncReaders();

    RefreshDisplay();
}

void Plugin::RefreshDisplay()
{
    const BatterySnapshot kb = m_key_reader.GetSnapshot();
    const BatterySnapshot ms = m_mouse_reader.GetSnapshot();
    const BatterySnapshot g7 = m_g7_reader.GetSnapshot();
    const BatterySnapshot ds = m_ds_reader.GetSnapshot();
    const TextConfig tc = PluginConfig::Instance().Text();
    const DisplayConfig dc = PluginConfig::Instance().Display();

    std::wstring kb_txt, ms_txt, g7_txt, ds_txt;
    BuildValueText(kb, kb_txt, dc.no_read);
    BuildValueText(ms, ms_txt, dc.no_read);
    BuildG7ValueText(g7, g7_txt, dc.no_read); // G7 用电池图标（4 档）
    BuildValueText(ds, ds_txt, dc.no_read);

    // 组合显示一块，按选项设置的勾选与顺序过滤设备。
    // 每个条目勾选即显示（含手柄）：已连接显示电量，未连接显示 --（与键鼠一致）。
    std::wstring val;
    auto append = [&](const std::wstring& seg) {
        if (!val.empty())
            val += L" | ";
        val += seg;
    };
    const std::wstring* txts[kDeviceCount] = { &kb_txt, &ms_txt, &g7_txt, &ds_txt };
    for (int i = 0; i < kDeviceCount; ++i)
    {
        int dev = m_order[i];
        if (!m_show[dev])
            continue;
        append(tc.name[dev] + L": " + *txts[dev]);
    }
    m_value_text = val;

    // 宽度测量用示例文本：设备名同步为当前自定义名称
    m_sample_text = tc.name[0] + L": 100%\u26A1 | " + tc.name[1] + L": 100%\u26A1 | " + tc.name[2] +
                    L": \u25AE\u25AE\u25AE\u25AE\u26A1 | " + tc.name[3] + L": 100%\u26A1";

    std::wstring tip;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        int dev = m_order[i];
        if (!m_show[dev])
            continue;
        switch (dev)
        {
        case 0: BuildKeyboardTooltip(kb, tip, tc.name[0]); break;
        case 1: BuildMouseTooltip(ms, tip, tc.name[1]); break;
        case 2: BuildPadTooltip(tc.name[2], g7, tip); break;
        case 3: BuildPadTooltip(tc.name[3], ds, tip); break;
        }
        tip += L'\n';
    }
    if (!tip.empty())
        tip.pop_back();
    m_tooltip = tip;

    // 自绘显示数据（与上面文本同序：仅勾选设备）
    // 未连接 / 暂无读数统一为：空圆环 + --（手柄与键鼠一致）
    m_display_count = 0;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        int dev = m_order[i];
        if (!m_show[dev])
            continue;
        DeviceDisplay& d = m_display[m_display_count++];
        d = DeviceDisplay{};
        d.name = tc.name[dev];
        if (dev == 2 || dev == 3)
        {
            const BatterySnapshot& s = (dev == 2) ? g7 : ds;
            if (s.valid)
            {
                d.percent = s.percent;
                d.charging = s.charging;
            }
            continue;
        }
        const BatterySnapshot& s = (dev == 0) ? kb : ms;
        if (s.has_device && s.online && s.valid)
        {
            d.percent = s.percent;
            d.charging = s.charging;
        }
    }

    // 全部取消勾选：保留占位项，任务栏上仍有点击区域可右键打开设置
    m_placeholder = (m_display_count == 0);
    if (m_placeholder)
    {
        m_value_text = kPlaceholderText;
        m_tooltip = kPlaceholderTip;
    }
}

// G7 粗档：电池图标（▮ 实心 / ▯ 空心），4 格表示 4 档
//  ▮▮▮▮ = 满   ▮▮▮▯ = 高   ▮▮▯▯ = 中   ▮▯▯▯ = 低   ⚡ 后缀 = 充电中
void Plugin::BuildG7ValueText(const BatterySnapshot& snap, std::wstring& out, const std::wstring& no_read)
{
    if (!snap.valid || snap.percent < 0)
    {
        out = no_read; // 未连接 / 暂无读数：与键鼠一致
        return;
    }

    int bars = 1; // 0% → 1 格
    if (snap.percent >= 100) bars = 4;
    else if (snap.percent >= 66) bars = 3;
    else if (snap.percent >= 33) bars = 2;

    out = L"";
    for (int i = 0; i < 4; ++i)
        out += (i < bars) ? L"\u25AE" : L"\u25AF"; // ▮ 实心 / ▯ 空心
    if (snap.charging)
        out += L"\u26A1"; // ⚡
}

void Plugin::BuildValueText(const BatterySnapshot& snap, std::wstring& out, const std::wstring& no_read)
{
    if (!snap.has_device || !snap.online || !snap.valid)
    {
        out = no_read; // 未找到设备 / 离线 / 暂无有效读数
        return;
    }

    wchar_t buf[32];
    if (snap.charging)
        swprintf_s(buf, L"%d%%\u26A1", snap.percent); // ⚡ 充电中
    else
        swprintf_s(buf, L"%d%%", snap.percent);
    out = buf;
}

// 悬停行尾部：秒级直显；分钟档带剩余秒；更大量级自动换算
static void AppendUpdatedAgo(const BatterySnapshot& snap, std::wstring& out)
{
    if (snap.last_update_ms == 0)
        return;
    ULONGLONG now = GetTickCount64();
    ULONGLONG diff = (now >= snap.last_update_ms) ? (now - snap.last_update_ms) / 1000 : 0;
    wchar_t buf[32];
    if (diff < 60)
        swprintf_s(buf, L" · %llu 秒前", diff);
    else if (diff < 3600)
        swprintf_s(buf, L" · %llu 分 %llu 秒前", diff / 60, diff % 60);
    else if (diff < 86400)
        swprintf_s(buf, L" · %llu 小时前", diff / 3600);
    else
        swprintf_s(buf, L" · %llu 天前", diff / 86400);
    out += buf;
}

// 悬停提示为一行式：图标 型号：状态 · 更新时间
void Plugin::BuildKeyboardTooltip(const BatterySnapshot& snap, std::wstring& out, const std::wstring& label)
{
    out += kTipIconKb;
    out += label;

    // 未检测到 / 离线 / 尚无上报，统一显示"未连接"
    if (!snap.has_device || !snap.online || !snap.valid)
        out += L"：未连接";
    else
    {
        wchar_t buf[32];
        if (snap.charging)
            swprintf_s(buf, L"：%d%% \u26A1充电中", snap.percent); // ⚡
        else
            swprintf_s(buf, L"：%d%%", snap.percent);
        out += buf;
        AppendUpdatedAgo(snap, out);
    }
}

void Plugin::BuildMouseTooltip(const BatterySnapshot& snap, std::wstring& out, const std::wstring& label)
{
    out += kTipIconMs;
    out += label;

    if (!snap.has_device || !snap.online || !snap.valid)
        out += L"：未连接";
    else
    {
        wchar_t buf[32];
        if (snap.charging)
            swprintf_s(buf, L"：%d%% \u26A1充电中", snap.percent); // ⚡
        else
            swprintf_s(buf, L"：%d%%", snap.percent);
        out += buf;
        AppendUpdatedAgo(snap, out);
    }
}

// 手柄悬停提示：图标 名称：状态 · 更新时间
void Plugin::BuildPadTooltip(const std::wstring& label, const BatterySnapshot& snap, std::wstring& out)
{
    out += kTipIconPad;
    out += label;

    // 未检测到接收器 / 离线 / 已断开，统一显示"未连接"
    if (!snap.has_device || !snap.online || !snap.valid)
        out += L"：未连接";
    else
    {
        wchar_t buf[32];
        if (snap.charging)
            swprintf_s(buf, L"：%d%% \u26A1充电中", snap.percent); // ⚡
        else
            swprintf_s(buf, L"：%d%%", snap.percent);
        out += buf;
        AppendUpdatedAgo(snap, out);
    }
}

const wchar_t* Plugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return L"外设电量显示";
    case TMI_DESCRIPTION:
        return L"外设电量显示";
    case TMI_AUTHOR:
        return L"ziphooo";
    case TMI_COPYRIGHT:
        return L"MIT License";
    case TMI_VERSION:
        return L"0.4.0";
    default:
        break;
    }
    return L"";
}

const wchar_t* Plugin::GetTooltipInfo()
{
    return m_tooltip.c_str();
}

void Plugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    LoadConfig();
}

// ============ 选项配置（勾选显示哪些设备，存 ini） ============

// 老版本写出的 ini 没有 BOM，Windows 会按 ANSI（GBK）读写，UTF-8 编辑器打开时中文全是乱码。
// 首次加载时把文件整体转成带 BOM 的 UTF-16LE，之后 WritePrivateProfileStringW 也会按 Unicode 写。
static void EnsureIniUnicode(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // 文件还不存在：先建一个只有 BOM 的空文件，后续写入即为 UTF-16
        h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, 2, &written, nullptr);
        CloseHandle(h);
        return;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(h);
        return;
    }
    std::string raw((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &raw[0], (DWORD)raw.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || read < 2)
        return;

    const unsigned char b0 = (unsigned char)raw[0];
    const unsigned char b1 = (unsigned char)raw[1];
    if (b0 == 0xFF && b1 == 0xFE)
        return; // 已经是 UTF-16LE

    UINT cp = CP_ACP;
    int skip = 0;
    if (read >= 3 && b0 == 0xEF && b1 == 0xBB && (unsigned char)raw[2] == 0xBF)
    {
        cp = CP_UTF8;
        skip = 3;
    }
    int n = MultiByteToWideChar(cp, 0, raw.data() + skip, (int)read - skip, nullptr, 0);
    if (n <= 0)
        return;
    std::wstring text((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, raw.data() + skip, (int)read - skip, &text[0], n);
    text.insert(text.begin(), L'\uFEFF');

    h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

void Plugin::LoadConfig()
{
    if (!m_app)
        return;
    std::wstring path = m_app->GetPluginConfigDir();
    path += L"\\PeripheralBattery.ini";

    EnsureIniUnicode(path);

    // 玩家自定义的刷新 / 外观 / 文案参数（[Advanced] 段）
    PluginConfig::Instance().SetIniPath(path);
    PluginConfig::Instance().Load();

    // 勾选：show{id}=0/1（按设备 id 持久化，新增设备自动多一个 key）
    wchar_t buf[16];
    for (int d = 0; d < kDeviceCount; ++d)
    {
        wchar_t key[16];
        swprintf_s(key, L"show%d", d);
        m_show[d] = true;
        if (GetPrivateProfileStringW(L"Options", key, L"1", buf, 16, path.c_str()) > 0)
            m_show[d] = (buf[0] == L'1');
    }

    // 显示顺序：order=1,3,2,0（设备索引，逗号分隔；默认按型号字母序）
    wchar_t ord[64];
    for (int i = 0; i < kDeviceCount; ++i)
        m_order[i] = kDefaultOrder[i];
    if (GetPrivateProfileStringW(L"Options", L"order", L"", ord, 64, path.c_str()) > 0)
    {
        int idx = 0;
        wchar_t* ctx = nullptr;
        wchar_t* tok = wcstok_s(ord, L",", &ctx);
        while (tok && idx < kDeviceCount)
        {
            int dev = _wtoi(tok);
            if (dev >= 0 && dev < kDeviceCount)
                m_order[idx++] = dev;
            tok = wcstok_s(nullptr, L",", &ctx);
        }
        // 去重补全（保证每个设备恰好出现一次）
        bool seen[kDeviceCount] = {};
        int cnt = 0;
        for (int i = 0; i < kDeviceCount; ++i)
            if (!seen[m_order[i]])
            {
                seen[m_order[i]] = true;
                m_order[cnt++] = m_order[i];
            }
        for (int d = 0; d < kDeviceCount; ++d)
            if (!seen[d])
                m_order[cnt++] = d;
    }
}

void Plugin::SaveConfig()
{
    if (!m_app)
        return;
    std::wstring path = m_app->GetPluginConfigDir();
    path += L"\\PeripheralBattery.ini";
    EnsureIniUnicode(path); // 兜底：运行期间文件被删掉时重建，保证仍以 UTF-16 写入
    for (int d = 0; d < kDeviceCount; ++d)
    {
        wchar_t key[16];
        swprintf_s(key, L"show%d", d);
        WritePrivateProfileStringW(L"Options", key, m_show[d] ? L"1" : L"0", path.c_str());
    }
    wchar_t ord[64];
    swprintf_s(ord, L"%d", m_order[0]);
    for (int i = 1; i < kDeviceCount; ++i)
    {
        wchar_t seg[16];
        swprintf_s(seg, L",%d", m_order[i]);
        wcscat_s(ord, seg);
    }
    WritePrivateProfileStringW(L"Options", L"order", ord, path.c_str());
    PluginConfig::Instance().Save();
}

// ============ 选项对话框工作副本 ============

void Plugin::DlgBegin()
{
    memcpy(m_dlg_show, m_show, sizeof(m_show));
    memcpy(m_dlg_order, m_order, sizeof(m_order));
}

void Plugin::DlgApply()
{
    memcpy(m_show, m_dlg_show, sizeof(m_show));
    memcpy(m_order, m_dlg_order, sizeof(m_order));
    SaveConfig();
    ApplyOptionsNow();
    SyncReaders(); // 勾选才查询：按最新勾选启动/停止设备 Reader
}

void Plugin::SyncReaders()
{
    for (int d = 0; d < kDeviceCount; ++d)
    {
        bool want = m_show[d];
        bool running = m_reader_started[d];
        if (want && !running)
        {
            switch (d)
            {
            case 0: m_key_reader.Start(); break;
            case 1: m_mouse_reader.Start(); break;
            case 2: m_g7_reader.Start(); break;
            case 3: m_ds_reader.Start(); break;
            }
            m_reader_started[d] = true;
        }
        else if (!want && running)
        {
            switch (d)
            {
            case 0: m_key_reader.Stop(); break;
            case 1: m_mouse_reader.Stop(); break;
            case 2: m_g7_reader.Stop(); break;
            case 3: m_ds_reader.Stop(); break;
            }
            m_reader_started[d] = false;
        }
    }
}

void Plugin::DlgMoveUp(int dev)
{
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (m_dlg_order[i] == dev && i > 0)
        {
            std::swap(m_dlg_order[i], m_dlg_order[i - 1]);
            return;
        }
    }
}

void Plugin::DlgMoveDown(int dev)
{
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (m_dlg_order[i] == dev && i < kDeviceCount - 1)
        {
            std::swap(m_dlg_order[i], m_dlg_order[i + 1]);
            return;
        }
    }
}

void Plugin::DlgRestoreDefault()
{
    for (int d = 0; d < kDeviceCount; ++d)
    {
        m_dlg_show[d] = true;
        m_dlg_order[d] = kDefaultOrder[d];
    }
}

// ============ 选项设置对话框（Win11 风格自绘） ============
// 上部：设备（左半，白色复选列表，右侧 上移 / 下移 / 改名）与
//       外观（右半，阈值/倍率/间距/配色/开关/无读数文本）左右并列、等高
// 下部：刷新策略（每台设备一张卡片，标题为设备名，内含该设备的各项间隔，单位秒）
// 底部：恢复默认（左，连同顺序与勾选一起还原）/ 确定 / 应用 / 取消（右对齐）
// 按钮自绘（圆角、悬停/按下态，默认按钮主题蓝）；Win11 下窗口圆角
// 所有文本按当前 DPI 与实际字体度量排版，字体放大不会裁切。

namespace {

constexpr UINT IDC_DEVLIST = 500;  // 设备复选列表（自绘行）
constexpr UINT IDC_DEVWARN = 569;  // 全部取消勾选时的提示
constexpr UINT IDC_RENAMEHINT = 570; // 改名进行中的操作提示（与上面的提示同位置、互斥显示）
constexpr UINT IDC_UP = 600;
constexpr UINT IDC_DOWN = 601;
constexpr UINT IDC_RENAME = 603;
constexpr UINT IDC_DEVNAME = 590;  // 就地改名输入框（叠在选中行上）
constexpr UINT IDC_RESET = 602;
constexpr UINT IDC_OK = 700;
constexpr UINT IDC_CANCEL = 701;
constexpr UINT IDC_APPLY = 702;
constexpr UINT IDC_REF0 = 800;   // 800..814：刷新策略（秒）
constexpr UINT IDC_NUM0 = 820;   // 820..825：外观数值（阈值 / 倍率 / 间距）
constexpr UINT IDC_NOREAD = 834; // 无数据时显示的文本
constexpr UINT IDC_SHOW_SEP = 840;
constexpr UINT IDC_SHOW_PCT = 841;
constexpr UINT IDC_COLOR0 = 850; // 850..863：外观配色（RRGGBB）
constexpr UINT IDC_REFDEV0 = 870; // 870..873：刷新策略的四张设备卡片（标题 = 设备名）

constexpr COLORREF kDlgBg = RGB(247, 247, 248);

// 布局按 DPI 与实际字体度量计算：字体放大（125%/150%）时标签与输入框同步加宽加高，
// 不会出现文字被裁切。g_lay 在打开对话框时由 ComputeLayout 填充，绘制过程也读它。
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

int Max2(int a, int b) { return a > b ? a : b; }

// 一组「标签 + 输入框」的列网格，坐标相对分组框左上角
struct GridCol
{
    int lbl_x = 0, lbl_w = 0, edit_x = 0, edit_w = 0;
};

struct Grid
{
    int ncols = 1;
    int rows = 1;
    GridCol col[3];
};

struct DlgLayout
{
    int w = 0, h = 0;
    int margin = 14;
    int btn_w = 96, btn_action_w = 76, btn_h = 28;
    int group_x = 10, group_w = 0;
    int field_h = 22, field_step = 24, title_h = 26;
    // 设备：左侧自绘复选列表，右侧一列 上移 / 下移 / 改名（与外观组左右并列，各占半宽）
    int dev_x = 0, dev_w = 0, dev_y = 0, dev_h = 0;
    int dev_list_x = 0, dev_list_w = 0, dev_list_h = 0, dev_row_h = 0;
    int dev_btn_x = 0, dev_btn_y = 0, dev_btn_w = 0, dev_btn_h = 0;
    int dev_warn_x = 0, dev_warn_y = 0, dev_warn_w = 0; // 「全部取消勾选」提示
    // 外观：与设备组左右并列（半宽），数值 2 列 × 3 行，下面依次是配色、开关、无读数文本
    int app_x = 0, app_w = 0, app_num_y = 0, sw_y = 0;
    int app_txt_x = 0, app_txt_lbl_w = 0, app_txt_edit_x = 0, app_txt_edit_w = 0;
    int ref_y = 0, ref_h = 0, app_y = 0, app_h = 0, bottom_y = 0;
    int color_gap = 0; // 数值区与配色区之间的额外留白
    int color_y0 = 0, color_row_y = 0;
    int color_lbl_col_w = 0, color_chip_w = 0;
    int color_chip_off[2] = {};
    int color_blk_x = 0;
    // 刷新策略：四张等宽设备卡片（标题即设备名），每张卡片内一列「标签 + 输入框」
    int ref_card_w = 0, ref_card_h = 0, ref_card_gap = 0, ref_card_x = 0, ref_card_y = 0;
    int ref_pad = 0, ref_lbl_gap = 0, ref_lbl_w[kDeviceCount] = {};
    Grid num;           // 外观数值
    int chk_w1 = 0, chk_w2 = 0; // 外观两个复选框（右半第一、二行）
};
DlgLayout g_lay;

struct RefField
{
    const wchar_t* label;
    int lo, hi;
    int RefreshConfig::* member;
    int dev;             // 所属设备（0..3），决定落在哪一张设备卡片里
    const wchar_t* tip;  // 鼠标悬停时的说明
};

// 刷新策略：每个设备一张卡片，卡片标题用玩家自定义的设备名，其中每行一项
const RefField kRefFields[15] = {
    { L"休眠判定", 1, 86400, &RefreshConfig::kb_wake_ms, 0,
      L"闲置满这么多秒后的第一次按键算唤醒，并立刻查询一次电量；连续打字时不会误触发。默认 60 秒。" },
    { L"使用判定", 1, 86400, &RefreshConfig::kb_active_ms, 0,
      L"最近这么多秒内有按键就算正在使用，期间按「刷新间隔」查询。默认 30 秒。" },
    { L"刷新间隔", 1, 86400, &RefreshConfig::kb_refresh_ms, 0,
      L"使用中每隔这么多秒查询一次电量。默认 5 秒。" },
    { L"最短查询间隔", 1, 86400, &RefreshConfig::kb_cooldown_ms, 0,
      L"两次查询之间至少隔这么多秒，防止刚唤醒就连续查询。默认 2 秒。" },
    { L"重试间隔", 1, 86400, &RefreshConfig::kb_reconnect_ms, 0,
      L"设备打不开（接收器没插好或被占用）时隔多久重试；期间保留最后一次读数。默认 3 秒。" },
    { L"休眠判定", 1, 86400, &RefreshConfig::ms_wake_ms, 1,
      L"这么久没有移动或点击后的第一次操作算唤醒，并立刻采集一次电量。默认 120 秒。" },
    { L"使用判定", 1, 86400, &RefreshConfig::ms_active_ms, 1,
      L"最近这么多秒内有移动、滚轮或点击就算正在使用，期间按「刷新间隔」采集。默认 30 秒。" },
    { L"刷新间隔", 1, 86400, &RefreshConfig::ms_refresh_ms, 1,
      L"使用中每隔这么多秒采集一次电量。默认 5 秒。" },
    { L"最短查询间隔", 1, 86400, &RefreshConfig::ms_cooldown_ms, 1,
      L"两次采集之间至少隔这么多秒。默认 2 秒。" },
    { L"断开判定", 1, 86400, &RefreshConfig::g7_offline_ms, 2,
      L"超过这么多秒收不到手柄消息就认为已断开，任务栏显示 --。默认 90 秒。" },
    { L"重新识别", 1, 86400, &RefreshConfig::g7_reset_ms, 2,
      L"超过这么多秒没消息就重新识别手柄（换接收器或换手柄后能重新认上）。应小于「断开判定」。默认 60 秒。" },
    { L"重试间隔", 1, 86400, &RefreshConfig::g7_reconnect_ms, 2,
      L"GIP 总线打不开（驱动未就绪）时隔多久重试。默认 3 秒。" },
    { L"读取超时", 1, 60, &RefreshConfig::g7_read_timeout_ms, 2,
      L"单次等待手柄消息的最长时间，超时即视为这一轮没有消息。默认 1 秒。" },
    { L"连接检查", 1, 600, &RefreshConfig::ds_pnp_ms, 3,
      L"每隔这么多秒检查手柄是否还在系统中，不在则显示 --。默认 2 秒。" },
    { L"读取间隔", 1, 86400, &RefreshConfig::ds_read_ms, 3,
      L"手柄在位时每隔这么多秒读一次电量；刚接入会立刻读一次。默认 60 秒。" },
};

struct NumField
{
    const wchar_t* label;
    int lo, hi;
    int DisplayConfig::* member;
    const wchar_t* tip;
};

// 外观数值：2 列 × 3 行
const NumField kNumFields[6] = {
    { L"高电量分界 %", 1, 100, &DisplayConfig::level_high,
      L"电量大于等于该值时使用高电量颜色。默认 60。" },
    { L"中电量分界 %", 0, 99, &DisplayConfig::level_low,
      L"电量大于等于该值（且低于高电量分界）时使用中电量颜色，再低为低电量颜色。默认 20。" },
    { L"数字缩放 ×0.1", 8, 40, &DisplayConfig::ring_scale_x10,
      L"数字大小相对字高的比例，数值 ×0.1；单行时圆环会随数字一起缩放，双行时圆环最大到任务栏行高。默认 15。" },
    { L"设备间距", 0, 60, &DisplayConfig::sep_w,
      L"相邻设备之间的间距，分隔线画在中间；设为 0 时设备紧挨在一起。默认 14。" },
    { L"左右边距", 0, 20, &DisplayConfig::pad_lr,
      L"插件条目左右两侧的留白，用来与相邻任务栏项隔开。默认 1。" },
    { L"环与数字间距", 0, 20, &DisplayConfig::num_gap,
      L"任务栏分两行显示时，圆环与右侧数字的间隙。默认 2。" },
};

// 外观配色：电量三档一种元素一行，深色与浅色任务栏各一列。
// IDC_COLOR0 起的顺序与 kColorMembers 一致：行优先，每行先深色后浅色。
const wchar_t* const kColorRows[3] = { L"高电量", L"中电量", L"低电量" };
constexpr int kColorRowsCount = 3;
const wchar_t* const kColorHeads[2] = { L"深色任务栏", L"浅色任务栏" };
constexpr int kColorCount = 6;

COLORREF DisplayConfig::* const kColorMembers[kColorCount] = {
    &DisplayConfig::green_dark, &DisplayConfig::green_light,
    &DisplayConfig::yellow_dark, &DisplayConfig::yellow_light,
    &DisplayConfig::red_dark, &DisplayConfig::red_light,
};

const wchar_t* const kNoReadLabel = L"无数据时显示";
constexpr size_t kNoReadMax = 15;
constexpr size_t kDevNameMax = 31;

// 对话框期间的设备名工作副本：列表行与「刷新策略」卡片标题都从这里取，
// 确定 / 应用时才随其余参数一起写回，取消则整体丢弃。
std::wstring g_dlg_names[kDeviceCount];
int g_rename_dev = -1;  // 正在改名的设备，-1 表示没有
HWND g_name_edit = nullptr;
HFONT g_list_font = nullptr;

// 四台都取消勾选时任务栏只剩占位项，设备区右侧给出提示，免得被当成插件失效
const wchar_t* const kWarnAllUnchecked = L"已全部取消勾选，任务栏仅剩占位项";
// 改名进行中占用同一位置，说明当前状态和如何结束
const wchar_t* const kRenameHint = L"输入新名称，回车确认，Esc 取消";

int MeasureText(HDC hdc, HFONT font, const wchar_t* s, int* out_h)
{
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
    SelectObject(hdc, old);
    if (out_h)
        *out_h = sz.cy;
    return sz.cx;
}

int GridColOf(int i, int rows, int ncols, bool column_major)
{
    return column_major ? i / rows : i % ncols;
}

// 该组网格的最小宽度：每列按最长标签，输入框取 edit_min
int GridNaturalWidth(HDC hdc, HFONT font, const wchar_t* const* labels, int n, int ncols,
                     bool column_major, int pad_x, int lbl_gap, int col_gap, int edit_min)
{
    const int rows = (n + ncols - 1) / ncols;
    int lbl[3] = {};
    bool used[3] = {};
    for (int i = 0; i < n; ++i)
    {
        const int c = GridColOf(i, rows, ncols, column_major);
        used[c] = true;
        const int w = MeasureText(hdc, font, labels[i], nullptr);
        if (w > lbl[c])
            lbl[c] = w;
    }
    int total = pad_x * 2;
    int nc = 0;
    for (int c = 0; c < ncols; ++c)
        if (used[c])
        {
            ++nc;
            total += lbl[c] + S(10) + lbl_gap;
        }
    total += (nc - 1) * col_gap;
    return total + nc * edit_min;
}

// 在给定组宽度内均分输入框（标签列宽按各自最长文字固定）
void BuildGrid(HDC hdc, HFONT font, const wchar_t* const* labels, int n, int ncols,
               bool column_major, int group_w, int pad_x, int lbl_gap, int col_gap,
               int edit_min, Grid& g)
{
    const int rows = (n + ncols - 1) / ncols;
    int lbl[3] = {};
    bool used[3] = {};
    for (int i = 0; i < n; ++i)
    {
        const int c = GridColOf(i, rows, ncols, column_major);
        used[c] = true;
        const int w = MeasureText(hdc, font, labels[i], nullptr);
        if (w > lbl[c])
            lbl[c] = w;
    }
    int nc = 0;
    int fixed = pad_x * 2;
    for (int c = 0; c < ncols; ++c)
        if (used[c])
        {
            ++nc;
            fixed += lbl[c] + S(10) + lbl_gap;
        }
    fixed += (nc - 1) * col_gap;
    int edit_w = nc ? (group_w - fixed) / nc : edit_min;
    if (edit_w < edit_min)
        edit_w = edit_min;

    g = Grid{};
    g.ncols = ncols;
    g.rows = rows;
    int x = pad_x;
    for (int c = 0; c < ncols; ++c)
    {
        if (!used[c])
            continue;
        g.col[c].lbl_x = x;
        g.col[c].lbl_w = lbl[c] + S(10);
        g.col[c].edit_x = g.col[c].lbl_x + g.col[c].lbl_w + lbl_gap;
        g.col[c].edit_w = edit_w;
        x = g.col[c].edit_x + edit_w + col_gap;
    }
}

void ComputeLayout(HWND hDlg, HFONT font)
{
    DlgLayout& L = g_lay;
    L.margin = S(14);
    L.btn_w = S(112);
    L.btn_h = S(32);

    HDC hdc = GetDC(hDlg);
    int text_h = 0;
    MeasureText(hdc, font, L"国Ag", &text_h);

    L.field_h = Max2(text_h + S(8), S(22));
    L.field_step = Max2(text_h + S(11), S(26));
    L.title_h = text_h + S(11);

    const int pad_x = S(12);
    const int lbl_gap = S(6);
    const int col_gap = S(16);
    const int chip_min = Max2(MeasureText(hdc, font, L"FFFFFF", nullptr) + S(20), S(76));

    // 设备与外观左右并列，各占半宽；设备组内是复选列表 + 右侧按钮列
    const int dev_btn_w = S(96);
    const int dev_btn_h = S(32);
    const int dev_row_h = S(36);
    const int dev_name_w = S(190);
    const int dev_min_w = pad_x * 2 + S(40) + dev_name_w + S(12) + dev_btn_w;
    const int group_gap = S(10);

    // 刷新策略：四张等宽卡片，每张一列「标签 + 输入框」，卡片标题是设备名
    const int card_edit_min = Max2(MeasureText(hdc, font, L"86400", nullptr) + S(20), S(72));
    L.ref_pad = S(10);
    L.ref_lbl_gap = S(6);
    L.ref_card_gap = S(8);
    int card_w = 0;
    for (int d = 0; d < kDeviceCount; ++d)
    {
        int lbl = 0;
        bool any = false;
        for (int i = 0; i < 15; ++i)
            if (kRefFields[i].dev == d)
            {
                any = true;
                const int w = MeasureText(hdc, font, kRefFields[i].label, nullptr);
                if (w > lbl)
                    lbl = w;
            }
        L.ref_lbl_w[d] = any ? lbl + S(8) : S(8);
        card_w = Max2(card_w, L.ref_pad * 2 + L.ref_lbl_w[d] + L.ref_lbl_gap + card_edit_min);
    }
    L.ref_card_w = card_w;
    L.ref_card_h = L.title_h + S(4) + 5 * L.field_step + S(8); // 行数以最多的设备（5 项）为准
    const int ref_group_w = L.ref_pad * 2 + kDeviceCount * card_w + (kDeviceCount - 1) * L.ref_card_gap;

    const wchar_t* numLabels[6];
    for (int i = 0; i < 6; ++i)
        numLabels[i] = kNumFields[i].label;

    // 各分组宽度取最宽者，保证边框左右对齐（设备 / 刷新 4 张卡片 / 外观）
    int color_lbl = MeasureText(hdc, font, L"电量颜色", nullptr);
    for (int i = 0; i < kColorRowsCount; ++i)
        color_lbl = Max2(color_lbl, MeasureText(hdc, font, kColorRows[i], nullptr));
    color_lbl = Max2(color_lbl, MeasureText(hdc, font, kNoReadLabel, nullptr));
    L.color_lbl_col_w = color_lbl + S(8);

    // 外观占右半列：数值 2 列 × 3 行，下面依次是配色、两个开关、无读数文本
    L.color_gap = S(4);
    L.chk_w1 = MeasureText(hdc, font, L"显示设备分隔线", nullptr) + S(28);
    L.chk_w2 = MeasureText(hdc, font, L"显示百分号", nullptr) + S(28);
    const int app_edit_min = S(64);
    const int app_num_min = GridNaturalWidth(hdc, font, numLabels, 6, 2, false, pad_x, lbl_gap,
                                             col_gap, app_edit_min);
    const int color_min_w = pad_x * 2 + L.color_lbl_col_w + lbl_gap + chip_min * 2 + S(10);
    const int sw_min_w = pad_x * 2 + L.chk_w1 + S(24) + L.chk_w2;
    L.app_txt_lbl_w = L.color_lbl_col_w;
    L.app_txt_edit_w = Max2(MeasureText(hdc, font, L"--", nullptr) + S(20), S(80));
    const int noread_min_w = pad_x * 2 + L.color_lbl_col_w + lbl_gap + L.app_txt_edit_w;
    const int app_min_w = Max2(app_num_min, Max2(color_min_w, Max2(sw_min_w, noread_min_w)));

    // 整组宽度取「刷新策略四张卡片」与「左右两半各自最小宽度」的最大值
    L.group_w = Max2(ref_group_w, dev_min_w * 2 + group_gap);
    L.group_w = Max2(L.group_w, app_min_w * 2 + group_gap);

    L.group_x = S(10);
    L.w = L.group_x * 2 + L.group_w;
    const int col_w = (L.group_w - group_gap) / 2;
    L.app_x = L.group_x + col_w + group_gap;
    L.app_w = L.group_w - col_w - group_gap;

    // 设备 = 分组标题 + 左侧四行复选列表 + 右侧按钮列（与列表顶部对齐）
    L.dev_x = L.group_x;
    L.dev_w = col_w;
    L.dev_y = L.margin;
    L.dev_row_h = dev_row_h;
    L.dev_list_x = L.group_x + pad_x;
    L.dev_list_h = kDeviceCount * dev_row_h;
    L.dev_btn_w = dev_btn_w;
    L.dev_btn_h = dev_btn_h;
    L.dev_btn_x = L.group_x + col_w - pad_x - dev_btn_w;
    L.dev_list_w = L.dev_btn_x - S(12) - L.dev_list_x;
    // 提示放在分组标题行右侧，不额外占高度；改名提示与「全部取消」提示共用这块位置
    L.dev_warn_w = Max2(MeasureText(hdc, font, kWarnAllUnchecked, nullptr),
                        MeasureText(hdc, font, kRenameHint, nullptr)) + S(4);
    L.dev_warn_x = L.group_x + col_w - pad_x - L.dev_warn_w;
    L.dev_warn_y = L.dev_y + S(6);

    // 外观组在右侧，标题与设备组对齐；数值网格、配色块、开关依次向下排
    L.app_y = L.dev_y;
    BuildGrid(hdc, font, numLabels, 6, 2, false, L.app_w, pad_x, lbl_gap, col_gap, app_edit_min,
              L.num);
    L.app_num_y = L.app_y + L.title_h + S(4);
    L.color_blk_x = L.app_x + pad_x;
    const int app_content_w = L.app_w - pad_x * 2;
    L.color_chip_w = Max2(chip_min, (app_content_w - L.color_lbl_col_w - lbl_gap - S(10)) / 2);
    L.color_chip_off[0] = L.color_lbl_col_w + lbl_gap;
    L.color_chip_off[1] = L.color_chip_off[0] + L.color_chip_w + S(10);
    L.color_y0 = L.app_num_y + 3 * L.field_step + L.color_gap; // 列头行
    L.color_row_y = L.color_y0 + L.field_step;
    L.app_txt_x = L.app_x + pad_x;
    L.app_txt_edit_x = L.app_txt_x + L.color_lbl_col_w + lbl_gap;
    L.sw_y = L.color_row_y + kColorRowsCount * L.field_step + S(6);
    L.app_h = L.title_h + S(4) + 3 * L.field_step + L.color_gap +
              L.field_step + kColorRowsCount * L.field_step + S(6) + 2 * L.field_step + S(8);

    // 两组等高：外观内容更多，设备列表与按钮靠上排列，多余空间留在组底部
    const int dev_nat_h = L.title_h + S(4) + L.dev_list_h + S(8);
    const int row_h = Max2(dev_nat_h, L.app_h);
    L.dev_h = row_h;
    L.app_h = row_h;
    L.dev_btn_y = L.dev_y + L.title_h + S(4);

    // 刷新策略 = 分组标题 + 一排设备卡片（两组之下，占满整行）
    L.ref_y = L.dev_y + row_h + S(10);
    L.ref_h = L.title_h + S(6) + L.ref_card_h + S(8);
    L.ref_card_x = L.group_x + L.ref_pad;
    L.ref_card_y = L.ref_y + L.title_h + S(6);

    // 底部按钮：按文字定宽，动作钮短小紧凑
    L.btn_h = L.field_h;
    L.btn_w = Max2(MeasureText(hdc, font, L"恢复默认", nullptr) + S(24), S(90));
    L.btn_action_w = Max2(MeasureText(hdc, font, L"取消", nullptr) + S(36), S(72));
    L.bottom_y = L.ref_y + L.ref_h + S(10);
    L.h = L.bottom_y + L.btn_h + L.margin;
    ReleaseDC(hDlg, hdc);
}

WNDPROC g_origBtnProc = nullptr;  // BUTTON 原窗口过程（子类化用）
HWND g_hover_btn = nullptr;       // 当前悬停的按钮（自绘悬停态）

// 勾选状态变化后刷新「全部取消勾选」提示的显隐
static void UpdateDeviceWarning(HWND hDlg)
{
    HWND warn = GetDlgItem(hDlg, IDC_DEVWARN);
    if (!warn)
        return;
    Plugin* self = (Plugin*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    bool any = false;
    for (int d = 0; d < kDeviceCount && !any; ++d)
        any = self && self->DlgShowFor(d);
    ShowWindow(warn, (any || g_name_edit) ? SW_HIDE : SW_SHOWNA);
    // 隐藏时静态控件不会自己擦除，就地重绘这块背景
    const RECT rc{ g_lay.dev_warn_x, g_lay.dev_warn_y,
                   g_lay.dev_warn_x + g_lay.dev_warn_w, g_lay.dev_warn_y + g_lay.field_h };
    InvalidateRect(hDlg, &rc, TRUE);
}

// 改名进行中的状态提示：标题行右侧换成操作说明，「改名」按钮置灰，
// 结束（提交 / 放弃）后还原。两个提示静态控件位置相同，任一时刻只显示一个。
static void SetRenameChrome(HWND hDlg, bool on)
{
    if (on)
        ShowWindow(GetDlgItem(hDlg, IDC_DEVWARN), SW_HIDE);
    ShowWindow(GetDlgItem(hDlg, IDC_RENAMEHINT), on ? SW_SHOWNA : SW_HIDE);
    EnableWindow(GetDlgItem(hDlg, IDC_RENAME), on ? FALSE : TRUE);
    if (!on)
        UpdateDeviceWarning(hDlg);
    const RECT rc{ g_lay.dev_warn_x, g_lay.dev_warn_y,
                   g_lay.dev_warn_x + g_lay.dev_warn_w, g_lay.dev_warn_y + g_lay.field_h };
    InvalidateRect(hDlg, &rc, TRUE);
}

// 按当前顺序重建列表，并选中指定设备（-1 则保留首行）
static void RebuildDevList(HWND hDlg, int selectDev)
{
    Plugin* self = (Plugin*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!self || !hList)
        return;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (int row = 0; row < kDeviceCount; ++row)
    {
        const int dev = self->DlgOrderAt(row);
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)g_dlg_names[dev].c_str());
        SendMessageW(hList, LB_SETITEMDATA, row, dev);
        if (dev == selectDev)
            sel = row;
    }
    SendMessageW(hList, LB_SETCURSEL, sel, 0);
    EnableWindow(GetDlgItem(hDlg, IDC_UP), sel > 0);
    EnableWindow(GetDlgItem(hDlg, IDC_DOWN), sel < kDeviceCount - 1);
    UpdateDeviceWarning(hDlg);
}

static int SelectedDev(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!hList)
        return -1;
    const int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return -1;
    return (int)SendMessageW(hList, LB_GETITEMDATA, sel, 0);
}

// 就地改名结束：keep=true 提交（空名 / 超长按边界处理），否则放弃
static void EndRename(HWND hDlg, bool keep)
{
    if (!g_name_edit)
        return;
    HWND e = g_name_edit;
    const int dev = g_rename_dev;
    g_name_edit = nullptr;
    g_rename_dev = -1;
    if (keep && dev >= 0)
    {
        wchar_t buf[128]{};
        GetWindowTextW(e, buf, 128);
        std::wstring v(buf);
        size_t b = 0, n = v.size();
        while (b < n && (v[b] == L' ' || v[b] == L'\t'))
            ++b;
        while (n > b && (v[n - 1] == L' ' || v[n - 1] == L'\t'))
            --n;
        v = v.substr(b, n - b);
        if (v.size() > kDevNameMax)
            v.resize(kDevNameMax);
        if (!v.empty())
            g_dlg_names[dev] = v;
        // 刷新策略卡片标题与设备名同源，改名后立即同步
        if (HWND card = GetDlgItem(hDlg, IDC_REFDEV0 + dev))
            SetWindowTextW(card, g_dlg_names[dev].c_str());
    }
    DestroyWindow(e);
    SetRenameChrome(hDlg, false);
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (hList)
    {
        // 重建列表让行文字与工作副本一致，并保持原选中行
        RebuildDevList(hDlg, dev >= 0 ? dev : SelectedDev(hDlg));
        SetFocus(hList);
        InvalidateRect(hList, nullptr, TRUE); // 去掉改名行的蓝框
    }
}

// 改名按钮：把输入框叠在选中行上，回车 / 点到别处提交，Esc 放弃
static void BeginRename(HWND hDlg)
{
    if (g_name_edit)
        return;
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!hList)
        return;
    const int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;
    RECT rc{};
    if (SendMessageW(hList, LB_GETITEMRECT, sel, (LPARAM)&rc) == LB_ERR)
        return;
    MapWindowPoints(hList, hDlg, (POINT*)&rc, 2);
    const int dev = (int)SendMessageW(hList, LB_GETITEMDATA, sel, 0);
    const int x = rc.left + S(40);
    const int inset = S(5);
    g_rename_dev = dev;
    g_name_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlg_names[dev].c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  x, rc.top + inset, rc.right - S(10) - x,
                                  (rc.bottom - rc.top) - inset * 2, hDlg,
                                  (HMENU)(UINT_PTR)IDC_DEVNAME, GetModuleHandleW(nullptr), nullptr);
    if (!g_name_edit)
    {
        g_rename_dev = -1;
        return;
    }
    HFONT f = g_list_font ? g_list_font : (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
    SendMessageW(g_name_edit, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_name_edit, EM_SETLIMITTEXT, (WPARAM)kDevNameMax, 0);
    SendMessageW(g_name_edit, EM_SETSEL, 0, -1);
    // 蓝框 + 浅蓝输入框 + 标题行操作说明，明确进入改名状态
    SetRenameChrome(hDlg, true);
    InvalidateRect(hList, nullptr, TRUE);
    SetFocus(g_name_edit);
}

// 对话框底色画刷：分组框 / 标签的背景与 WM_ERASEBKGND 保持一致
HBRUSH DlgBgBrush()
{
    static HBRUSH br = CreateSolidBrush(kDlgBg);
    return br;
}

void SetIntText(HWND edit, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    SetWindowTextW(edit, buf);
}

int SecFromMs(int ms)
{
    int s = (ms + 500) / 1000;
    return s < 1 ? 1 : s;
}

// 配色输入框以其颜色作为背景（自动选黑白文字）；输入到一半无法解析时保留上一个有效色
struct ChipState
{
    HWND edit[kColorCount] = {};
    HBRUSH brush[kColorCount] = {};
    COLORREF color[kColorCount] = {};
};
ChipState g_chips;

void ReleaseChips()
{
    for (int i = 0; i < kColorCount; ++i)
    {
        if (g_chips.brush[i])
            DeleteObject(g_chips.brush[i]);
        g_chips.brush[i] = nullptr;
        g_chips.edit[i] = nullptr;
    }
}

void SyncChip(int i)
{
    if (!g_chips.edit[i])
        return;
    wchar_t buf[16]{};
    GetWindowTextW(g_chips.edit[i], buf, 16);
    COLORREF c = 0;
    if (!PluginConfig::ParseColor(buf, c))
        return;
    if (g_chips.brush[i])
        DeleteObject(g_chips.brush[i]);
    g_chips.color[i] = c;
    g_chips.brush[i] = CreateSolidBrush(c);
    InvalidateRect(g_chips.edit[i], nullptr, TRUE);
}

COLORREF ChipTextColor(COLORREF c)
{
    const int luma = ((int)GetRValue(c) * 299 + (int)GetGValue(c) * 587 + (int)GetBValue(c) * 114) / 1000;
    return luma > 150 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// 用指定配置填充全部输入框
void FillAdvancedFields(HWND hDlg, const RefreshConfig& r, const DisplayConfig& d, const TextConfig& t)
{
    for (int i = 0; i < 15; ++i)
        SetIntText(GetDlgItem(hDlg, IDC_REF0 + i), SecFromMs(r.*kRefFields[i].member));

    for (int i = 0; i < 6; ++i)
        SetIntText(GetDlgItem(hDlg, IDC_NUM0 + i), d.*kNumFields[i].member);
    CheckDlgButton(hDlg, IDC_SHOW_SEP, d.show_sep ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_SHOW_PCT, d.show_pct_sign ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i < kColorCount; ++i)
        SetWindowTextW(GetDlgItem(hDlg, IDC_COLOR0 + i),
                       PluginConfig::FormatColor(d.*kColorMembers[i]).c_str());
    for (int i = 0; i < kColorCount; ++i)
        SyncChip(i);

    for (int i = 0; i < kDeviceCount; ++i)
        g_dlg_names[i] = t.name[i];
    SetWindowTextW(GetDlgItem(hDlg, IDC_NOREAD), d.no_read.c_str());
}

// 用当前生效的配置填充
void FillAdvancedFields(HWND hDlg)
{
    FillAdvancedFields(hDlg, PluginConfig::Instance().Refresh(), PluginConfig::Instance().Display(),
                       PluginConfig::Instance().Text());
}

void WarnField(HWND hDlg, HWND edit, const std::wstring& msg)
{
    MessageBoxW(hDlg, msg.c_str(), L"显示设置", MB_OK | MB_ICONWARNING);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}

// 卡片标题跟随设备名（应用 / 确定 / 恢复默认后刷新）
void UpdateRefCardTitles(HWND hDlg, const TextConfig& t)
{
    for (int d = 0; d < kDeviceCount; ++d)
        SetWindowTextW(GetDlgItem(hDlg, IDC_REFDEV0 + d), t.name[d].c_str());
}

void UpdateRefCardTitles(HWND hDlg)
{
    UpdateRefCardTitles(hDlg, PluginConfig::Instance().Text());
}

bool ReadIntField(HWND hDlg, HWND edit, const wchar_t* label, int lo, int hi,
                  const std::wstring& unit, int& out)
{
    wchar_t buf[32]{};
    GetWindowTextW(edit, buf, 32);
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (end == buf || *end != L'\0' || v < lo || v > hi)
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"”" + unit + L"需要是 " +
                                   std::to_wstring(lo) + L" ~ " + std::to_wstring(hi) +
                                   L" 的整数。");
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool ReadTextField(HWND hDlg, HWND edit, const wchar_t* label, size_t maxlen, std::wstring& out)
{
    wchar_t buf[128]{};
    GetWindowTextW(edit, buf, 128);
    std::wstring v(buf);
    size_t b = 0;
    size_t e = v.size();
    while (b < e && (v[b] == L' ' || v[b] == L'\t'))
        ++b;
    while (e > b && (v[e - 1] == L' ' || v[e - 1] == L'\t'))
        --e;
    v = v.substr(b, e - b);
    if (v.empty())
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"” 不能为空。");
        return false;
    }
    if (v.size() > maxlen)
    {
        WarnField(hDlg, edit, std::wstring(L"“") + label + L"” 最多 " +
                                   std::to_wstring(maxlen) + L" 个字符。");
        return false;
    }
    out = v;
    return true;
}

bool ReadColorField(HWND hDlg, int i, COLORREF& out)
{
    HWND edit = GetDlgItem(hDlg, IDC_COLOR0 + i);
    wchar_t buf[16]{};
    GetWindowTextW(edit, buf, 16);
    if (!PluginConfig::ParseColor(buf, out))
    {
        WarnField(hDlg, edit, std::wstring(L"“") + kColorRows[i / 2] + L"·" + kColorHeads[i % 2] +
                                   L"” 需要是 6 位 RGB 十六进制颜色（如 70CD7E）。");
        return false;
    }
    return true;
}

// 校验并写回配置
bool ReadAdvancedFields(HWND hDlg)
{
    RefreshConfig r;
    int sec[15]{};
    for (int i = 0; i < 15; ++i)
    {
        const std::wstring label =
            PluginConfig::Instance().Name(kRefFields[i].dev) + L" " + kRefFields[i].label;
        if (!ReadIntField(hDlg, GetDlgItem(hDlg, IDC_REF0 + i), label.c_str(),
                          kRefFields[i].lo, kRefFields[i].hi, L"（秒）", sec[i]))
            return false;
    }
    for (int i = 0; i < 15; ++i)
        r.*kRefFields[i].member = sec[i] * 1000;

    DisplayConfig d = PluginConfig::Instance().Display();
    for (int i = 0; i < 6; ++i)
        if (!ReadIntField(hDlg, GetDlgItem(hDlg, IDC_NUM0 + i), kNumFields[i].label,
                          kNumFields[i].lo, kNumFields[i].hi, L" ", d.*kNumFields[i].member))
            return false;
    if (d.level_low >= d.level_high)
    {
        WarnField(hDlg, GetDlgItem(hDlg, IDC_NUM0 + 1),
                  L"“中电量分界 %” 必须小于 “高电量分界 %”。");
        return false;
    }
    d.show_sep = IsDlgButtonChecked(hDlg, IDC_SHOW_SEP) == BST_CHECKED;
    d.show_pct_sign = IsDlgButtonChecked(hDlg, IDC_SHOW_PCT) == BST_CHECKED;
    for (int i = 0; i < kColorCount; ++i)
        if (!ReadColorField(hDlg, i, d.*kColorMembers[i]))
            return false;

    TextConfig t;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        if (g_dlg_names[i].empty())
        {
            MessageBoxW(hDlg, (L"“" + PluginConfig::Instance().Name(i) + L"” 的名称不能为空。").c_str(),
                        L"显示设置", MB_OK | MB_ICONWARNING);
            return false;
        }
        t.name[i] = g_dlg_names[i];
    }

    if (!ReadTextField(hDlg, GetDlgItem(hDlg, IDC_NOREAD), kNoReadLabel, kNoReadMax, d.no_read))
        return false;

    PluginConfig::Instance().SetAll(r, d, t);
    return true;
}

WNDPROC g_origListProc = nullptr; // LISTBOX 原窗口过程（子类化用）
int g_hover_row = -1;             // 列表当前悬停行（自绘悬停态）

// LISTBOX 子类化：悬停跟踪 + 点击左侧复选框热区切换勾选（其余走默认选中逻辑）
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        {
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
            int row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
            if (row != g_hover_row)
            {
                g_hover_row = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover_row != -1)
        {
            g_hover_row = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN:
        {
            Plugin* self = (Plugin*)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
            const int row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
            if (self && row >= 0 && row < kDeviceCount && GET_X_LPARAM(lp) < S(40))
            {
                self->DlgToggle((int)SendMessageW(hwnd, LB_GETITEMDATA, row, 0));
                UpdateDeviceWarning(GetParent(hwnd));
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }
    return CallWindowProcW(g_origListProc, hwnd, msg, wp, lp);
}

// BUTTON 子类化：自绘按钮的悬停状态跟踪（WM_DRAWITEM 绘制时使用）
LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (g_hover_btn != hwnd)
        {
            g_hover_btn = hwnd;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover_btn == hwnd)
        {
            g_hover_btn = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    return CallWindowProcW(g_origBtnProc, hwnd, msg, wp, lp);
}

LRESULT CALLBACK OptionDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Plugin* self = (Plugin*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_INITDIALOG:
        // 创建参数即 Plugin 实例；本过程经 DefDlgProc 转发，未处理的消息返回 FALSE
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, lp);
        return TRUE;
    case WM_ERASEBKGND:
        // 浅灰画布，分组框与下拉式控件浮在上面
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, DlgBgBrush());
            return 1;
        }
    case WM_PAINT:
        {
            // 设备列表卡片：1px 扁平边框，紧贴列表窗口外圈
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HWND hList = GetDlgItem(hwnd, IDC_DEVLIST);
            if (hList)
            {
                RECT rl;
                GetWindowRect(hList, &rl);
                MapWindowPoints(nullptr, hwnd, (POINT*)&rl, 2);
                InflateRect(&rl, 1, 1);
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, RGB(216, 216, 220)));
                HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rl.left, rl.top, rl.right, rl.bottom);
                DeleteObject(SelectObject(hdc, op));
                SelectObject(hdc, ob);
            }
            EndPaint(hwnd, &ps);
            return TRUE;
        }
    case WM_CTLCOLORSTATIC:
        // 分组框 / 标签与画布同色，避免默认系统灰底
        {
            HDC hdc = (HDC)wp;
            const UINT sid = GetDlgCtrlID((HWND)lp);
            if (sid == IDC_DEVWARN)
                SetTextColor(hdc, RGB(176, 90, 28)); // 提示用暖色，与普通标签区分
            else if (sid == IDC_RENAMEHINT)
                SetTextColor(hdc, RGB(0, 90, 158)); // 改名提示用主题蓝
            SetBkColor(hdc, kDlgBg);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)DlgBgBrush();
        }
    case WM_CTLCOLOREDIT:
        // 配色输入框以该颜色作背景，文字按亮度取黑白
        {
            HWND e = (HWND)lp;
            if (e == g_name_edit)
            {
                // 改名输入框：浅蓝底，配合行上的蓝框表明正在编辑
                static HBRUSH br = CreateSolidBrush(RGB(232, 243, 255));
                HDC hdc = (HDC)wp;
                SetTextColor(hdc, RGB(20, 20, 20));
                SetBkColor(hdc, RGB(232, 243, 255));
                return (LRESULT)br;
            }
            for (int i = 0; i < kColorCount; ++i)
                if (g_chips.edit[i] == e && g_chips.brush[i])
                {
                    HDC hdc = (HDC)wp;
                    SetTextColor(hdc, ChipTextColor(g_chips.color[i]));
                    SetBkColor(hdc, g_chips.color[i]);
                    return (LRESULT)g_chips.brush[i];
                }
        }
        break;
    case WM_DESTROY:
        g_name_edit = nullptr; // 子控件随对话框一起销毁，留指针只为避免误用
        g_rename_dev = -1;
        g_hover_row = -1;
        ReleaseChips();
        break;
    case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;

            // ---- 设备列表行：白卡片 + 圆角行高亮（选中淡蓝 / 悬停浅灰）+ 圆角复选框 ----
            if (dis->CtlID == IDC_DEVLIST && dis->itemID != (UINT)-1)
            {
                const int dev = (int)dis->itemData;
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                const bool sel = (dis->itemState & ODS_SELECTED) != 0;
                const bool hov = (g_hover_row == (int)dis->itemID) && !sel;

                const COLORREF bg =
                    sel ? RGB(233, 243, 255) : (hov ? RGB(244, 244, 246) : RGB(255, 255, 255));
                RECT rr = rc;
                InflateRect(&rr, -S(2), -S(2));
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, bg));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(bg));
                RoundRect(hdc, rr.left, rr.top, rr.right, rr.bottom, S(6), S(6));
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                // 复选框：未选白底灰边、选中主题蓝底白勾
                const int bh = rc.bottom - rc.top;
                const int cs = S(18);
                const bool checked = self && self->DlgShowFor(dev);
                const RECT box{ rc.left + S(12), rc.top + (bh - cs) / 2,
                                rc.left + S(12) + cs, rc.top + (bh - cs) / 2 + cs };
                const COLORREF fillCol = checked ? RGB(0, 103, 192) : RGB(255, 255, 255);
                const COLORREF brdCol = checked ? RGB(0, 103, 192) : RGB(130, 130, 135);
                op = SelectObject(hdc, CreatePen(PS_SOLID, 1, brdCol));
                ob = SelectObject(hdc, CreateSolidBrush(fillCol));
                RoundRect(hdc, box.left, box.top, box.right, box.bottom, S(4), S(4));
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));
                if (checked)
                {
                    HGDIOBJ oh = SelectObject(hdc, CreatePen(PS_SOLID, Max2(S(2), 1), RGB(255, 255, 255)));
                    MoveToEx(hdc, box.left + cs * 4 / 20, box.top + cs * 9 / 20, nullptr);
                    LineTo(hdc, box.left + cs * 8 / 20, box.top + cs * 14 / 20);
                    LineTo(hdc, box.left + cs * 15 / 20, box.top + cs * 6 / 20);
                    DeleteObject(SelectObject(hdc, oh));
                }

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(20, 20, 20));
                RECT rcTxt = rc;
                rcTxt.left += S(40);
                DrawTextW(hdc, g_dlg_names[dev].c_str(), -1, &rcTxt,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // 正在改名的行：主题蓝描边，即使输入框还没拿到焦点也能一眼看出
                if (g_rename_dev == dev)
                {
                    RECT rf = rc;
                    InflateRect(&rf, -S(2), -S(2));
                    op = SelectObject(hdc, CreatePen(PS_SOLID, Max2(S(2), 2), RGB(0, 103, 192)));
                    ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                    RoundRect(hdc, rf.left, rf.top, rf.right, rf.bottom, S(6), S(6));
                    DeleteObject(SelectObject(hdc, op));
                    SelectObject(hdc, ob);
                }
                return TRUE;
            }

            // ---- 按钮：圆角 4px，白底灰边；默认（确定）主题蓝底白字 ----
            if (dis->CtlType == ODT_BUTTON)
            {
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                bool defBtn = (dis->CtlID == IDC_OK);
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                bool hover = (g_hover_btn == dis->hwndItem) && !disabled;
                bool pressed = (dis->itemState & ODS_SELECTED) != 0;

                COLORREF fill, border, txt;
                if (defBtn)
                {
                    fill = pressed ? RGB(0, 89, 169) : (hover ? RGB(26, 120, 208) : RGB(0, 103, 192));
                    border = RGB(0, 80, 158);
                    txt = RGB(255, 255, 255);
                }
                else if (disabled)
                {
                    fill = kDlgBg;
                    border = RGB(225, 225, 228);
                    txt = RGB(178, 178, 184);
                }
                else
                {
                    fill = pressed ? RGB(229, 236, 245) : (hover ? RGB(245, 249, 255) : RGB(255, 255, 255));
                    border = hover || pressed ? RGB(0, 103, 192) : RGB(198, 198, 203);
                    txt = RGB(38, 38, 42);
                }

                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, border));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(fill));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                wchar_t caption[64];
                int n = GetWindowTextW(dis->hwndItem, caption, 64);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, txt);
                DrawTextW(hdc, caption, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }
    case WM_COMMAND:
        {
            UINT id = LOWORD(wp);
            if (HIWORD(wp) == EN_CHANGE && id >= IDC_COLOR0 && id < IDC_COLOR0 + kColorCount)
            {
                SyncChip((int)(id - IDC_COLOR0));
                return TRUE;
            }
            // 回车 / Esc 在改名输入框里先结束改名，不触发确定 / 关窗
            if (id == IDOK && g_name_edit)
            {
                EndRename(hwnd, true);
                return TRUE;
            }
            if (id == IDCANCEL && g_name_edit)
            {
                EndRename(hwnd, false);
                return TRUE;
            }
            if (id == IDC_DEVNAME && HIWORD(wp) == EN_KILLFOCUS)
            {
                EndRename(hwnd, true);
                return TRUE;
            }
            // 设备列表：换行时同步 上移 / 下移 的可用性
            if (id == IDC_DEVLIST && HIWORD(wp) == LBN_SELCHANGE)
            {
                HWND hList = GetDlgItem(hwnd, IDC_DEVLIST);
                const int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                EnableWindow(GetDlgItem(hwnd, IDC_UP), sel > 0);
                EnableWindow(GetDlgItem(hwnd, IDC_DOWN), sel >= 0 && sel < kDeviceCount - 1);
                return TRUE;
            }
            if ((id == IDC_UP || id == IDC_DOWN) && HIWORD(wp) == BN_CLICKED)
            {
                const int dev = SelectedDev(hwnd);
                if (self && dev >= 0)
                {
                    if (id == IDC_UP)
                        self->DlgMoveUp(dev);
                    else
                        self->DlgMoveDown(dev);
                    RebuildDevList(hwnd, dev);
                }
                SetFocus(GetDlgItem(hwnd, IDC_DEVLIST));
                return TRUE;
            }
            if (id == IDC_RENAME && HIWORD(wp) == BN_CLICKED)
            {
                BeginRename(hwnd);
                return TRUE;
            }
            if (id == IDC_RESET)
            {
                // 一键还原：勾选、显示顺序、刷新策略、外观、文案全部回到出厂值（仍待确定 / 应用）
                if (self)
                {
                    EndRename(hwnd, false);
                    self->DlgRestoreDefault();
                    FillAdvancedFields(hwnd, RefreshConfig{}, DisplayConfig{}, TextConfig{});
                    UpdateRefCardTitles(hwnd, TextConfig{});
                    RebuildDevList(hwnd, -1);
                }
                return TRUE;
            }
            switch (id)
            {
            case IDC_APPLY:
                // 立即保存并刷新显示，对话框保持打开（方便预览效果）
                EndRename(hwnd, true);
                if (self && ReadAdvancedFields(hwnd))
                {
                    self->DlgApply();
                    self->SetOptionChanged(true);
                    UpdateRefCardTitles(hwnd); // 设备名可能已修改：同步卡片标题
                    InvalidateRect(GetDlgItem(hwnd, IDC_DEVLIST), nullptr, TRUE);
                }
                return TRUE;
            case IDC_OK:
                EndRename(hwnd, true);
                if (self)
                {
                    if (!ReadAdvancedFields(hwnd))
                        return TRUE; // 输入非法：保持对话框打开
                    self->DlgApply(); // 写回配置、保存并立即刷新显示
                    self->SetOptionChanged(true);
                }
                {
                    HWND p = GetParent(hwnd);
                    if (p)
                    {
                        EnableWindow(p, TRUE);
                        SetActiveWindow(p);
                        SetForegroundWindow(p);
                    }
                }
                DestroyWindow(hwnd);
                return TRUE;
            case IDC_CANCEL:
                {
                    HWND p = GetParent(hwnd);
                    if (p)
                    {
                        EnableWindow(p, TRUE);
                        SetActiveWindow(p);
                        SetForegroundWindow(p);
                    }
                }
                DestroyWindow(hwnd);
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        {
            HWND p = GetParent(hwnd);
            if (p)
            {
                EnableWindow(p, TRUE);
                SetActiveWindow(p);
                SetForegroundWindow(p);
            }
        }
        DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;
}

} // namespace

ITMPlugin::OptionReturn Plugin::ShowOptionsDialog(void* hParent)
{
    HWND parent = (HWND)hParent;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 用内存中的空对话框模板建窗（无控件、无图标）：模板对话框不会回退到系统默认图标，
    // 与其他插件的选项页一致；控件仍按像素坐标手工创建，最终尺寸由布局计算后设定。
    struct
    {
        DLGTEMPLATE tpl{};
        WORD menu = 0;
        WORD cls = 0;
        wchar_t title[16]{};
    } tmpl;
    tmpl.tpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    tmpl.tpl.cx = 420;
    tmpl.tpl.cy = 600;
    lstrcpynW(tmpl.title, L"显示设置", 16);

    HWND hDlg = CreateDialogIndirectParamW(hInst, &tmpl.tpl, parent, OptionDlgProc, (LPARAM)this);
    if (!hDlg)
        return OR_OPTION_UNCHANGED;

    // 按窗口所在显示器的 DPI 计算布局：字体放大时控件同步放大，文字不会被裁切
    {
        UINT dpi = 0;
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
        {
            typedef UINT(WINAPI * PFN_GDFW)(HWND);
            if (auto p = (PFN_GDFW)GetProcAddress(u32, "GetDpiForWindow"))
                dpi = p(hDlg);
        }
        if (!dpi)
        {
            HDC dc = GetDC(nullptr);
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
        }
        g_dpi = dpi ? (int)dpi : 96;
    }

    // Win11 圆角窗口（DWMWA_WINDOW_CORNER_PREFERENCE=33 / DWMWCP_ROUND=2，旧系统失败自动忽略）
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
    {
        typedef HRESULT(__stdcall* PFN_DSA)(HWND, DWORD, const void*, DWORD);
        if (auto pDSA = (PFN_DSA)GetProcAddress(dwm, "DwmSetWindowAttribute"))
        {
            int pref = 2; // DWMWCP_ROUND
            pDSA(hDlg, 33, &pref, sizeof(pref));
        }
    }

    // 工作副本：对话框期间所有勾选/排序只改副本，确定才写回
    DlgBegin();

    // 系统 UI 字体（Segoe UI / 微软雅黑），按当前显示器 DPI 取，对话框与所有控件统一设置
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    bool got_ncm = false;
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI * PFN_SPIFD)(UINT, UINT, void*, UINT, UINT);
        if (auto p = (PFN_SPIFD)GetProcAddress(u32, "SystemParametersInfoForDpi"))
            got_ncm = p(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi) != FALSE;
    }
    if (!got_ncm)
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(hDlg, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 设备列表字体比系统字体大两档，行文字更醒目
    LOGFONTW lfList = ncm.lfMessageFont;
    lfList.lfHeight = -S(16);
    g_list_font = CreateFontIndirectW(&lfList);

    // 控件位置依赖字体度量，字体建好后再算
    ComputeLayout(hDlg, hFont);
    const DlgLayout& L = g_lay;

    // 由客户区尺寸反推整窗尺寸（标题栏/边框由系统计算，不手工留余量）
    RECT wnd{ 0, 0, L.w, L.h };
    AdjustWindowRectEx(&wnd, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, 0);
    const int W = wnd.right - wnd.left, H = wnd.bottom - wnd.top;

    auto mk = [&](UINT id, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
                                    WS_CHILD | WS_VISIBLE | style,
                                    x, y, w, h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
        {
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
            // 自绘按钮的悬停状态需要子类化跟踪
            WNDPROC prev = (WNDPROC)SetWindowLongPtrW(ctrl, GWLP_WNDPROC, (LONG_PTR)BtnProc);
            if (!g_origBtnProc)
                g_origBtnProc = prev;
        }
    };

    // 参数悬停提示：鼠标停在参数名或输入框上显示该参数的含义
    HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                0, 0, 0, 0, hDlg, nullptr, hInst, nullptr);
    if (hTip)
    {
        SendMessageW(hTip, WM_SETFONT, (WPARAM)hFont, FALSE);
        SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, S(380)); // 超宽自动换行
        SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_RESHOW, 80);
    }
    auto addTip = [&](HWND ctl, const wchar_t* text) {
        if (!hTip || !ctl || !text)
            return;
        TOOLINFOW ti{};
        ti.cbSize = TTTOOLINFOW_V1_SIZE; // 宿主无 v6 清单时是 comctl32 v5，只认 v1 大小
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hDlg;
        ti.uId = (UINT_PTR)ctl;
        ti.lpszText = const_cast<LPWSTR>(text);
        SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };

    // 分组框（系统绘制边框，标签/底色由 WM_CTLCOLORSTATIC 统一）
    auto mkGroup = [&](const wchar_t* text, int x, int y, int w, int h) {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                    x, y, w, h, hDlg, nullptr, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
    };
    // SS_NOTIFY：静态文本默认不接收鼠标消息，接受后提示才能响应悬停
    auto mkLabel = [&](const wchar_t* text, int x, int y, int w) {
        HWND ctrl = CreateWindowExW(0, L"STATIC", text,
                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                                    x, y, w, L.field_h, hDlg, nullptr, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };
    auto mkEdit = [&](UINT id, int x, int y, int w) {
        HWND ctrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    x, y, w, L.field_h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
        if (ctrl)
            SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };

    // 设备：左侧白色复选列表（每行 = 勾选框 + 设备名），选中行后用右侧按钮调序 / 改名
    mkGroup(L"设备", L.dev_x, L.dev_y, L.dev_w, L.dev_h);
    {
        HWND hList = CreateWindowExW(0, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY |
                                         LBS_NOINTEGRALHEIGHT,
                                     L.dev_list_x, L.dev_btn_y, L.dev_list_w, L.dev_list_h,
                                     hDlg, (HMENU)(UINT_PTR)IDC_DEVLIST, hInst, nullptr);
        if (hList)
        {
            SendMessageW(hList, WM_SETFONT, (WPARAM)g_list_font, TRUE);
            SendMessageW(hList, LB_SETITEMHEIGHT, 0, L.dev_row_h);
            g_origListProc = (WNDPROC)SetWindowLongPtrW(hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
        }

        // 右侧按钮列：与列表等高，三个按钮均分
        const int btnGap = (L.dev_list_h - L.dev_btn_h * 3) / 2;
        mk(IDC_UP, L"上移", L.dev_btn_x, L.dev_btn_y, L.dev_btn_w, L.dev_btn_h,
           BS_OWNERDRAW | WS_TABSTOP);
        mk(IDC_DOWN, L"下移", L.dev_btn_x, L.dev_btn_y + L.dev_btn_h + btnGap,
           L.dev_btn_w, L.dev_btn_h, BS_OWNERDRAW | WS_TABSTOP);
        mk(IDC_RENAME, L"改名", L.dev_btn_x, L.dev_btn_y + (L.dev_btn_h + btnGap) * 2,
           L.dev_btn_w, L.dev_btn_h, BS_OWNERDRAW | WS_TABSTOP);

        // 全部取消勾选时才显示的提示（初始显隐由 UpdateDeviceWarning 决定）
        HWND warn = CreateWindowExW(0, L"STATIC", kWarnAllUnchecked,
                                    WS_CHILD | SS_RIGHT | SS_CENTERIMAGE,
                                    L.dev_warn_x, L.dev_warn_y, L.dev_warn_w, L.field_h,
                                    hDlg, (HMENU)(UINT_PTR)IDC_DEVWARN, hInst, nullptr);
        if (warn)
            SendMessageW(warn, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 改名进行中的操作说明（与上面的提示在同一位置，默认隐藏）
        HWND hint = CreateWindowExW(0, L"STATIC", kRenameHint,
                                    WS_CHILD | SS_RIGHT | SS_CENTERIMAGE,
                                    L.dev_warn_x, L.dev_warn_y, L.dev_warn_w, L.field_h,
                                    hDlg, (HMENU)(UINT_PTR)IDC_RENAMEHINT, hInst, nullptr);
        if (hint)
            SendMessageW(hint, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    // 刷新策略：分组框内四张设备卡片，卡片标题就是（可自定义的）设备名
    mkGroup(L"刷新策略（单位：秒）", L.group_x, L.ref_y, L.group_w, L.ref_h);
    {
        int cardX = L.ref_card_x;
        for (int d = 0; d < kDeviceCount; ++d)
        {
            HWND card = CreateWindowExW(0, L"BUTTON", PluginConfig::Instance().Name(d).c_str(),
                                        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                        cardX, L.ref_card_y, L.ref_card_w, L.ref_card_h,
                                        hDlg, (HMENU)(UINT_PTR)(IDC_REFDEV0 + d), hInst, nullptr);
            if (card)
                SendMessageW(card, WM_SETFONT, (WPARAM)hFont, TRUE);

            const int lblX = cardX + L.ref_pad;
            const int editX = lblX + L.ref_lbl_w[d] + L.ref_lbl_gap;
            const int editW = L.ref_card_w - L.ref_pad * 2 - L.ref_lbl_w[d] - L.ref_lbl_gap;
            int rowY = L.ref_card_y + L.title_h + S(4);
            for (int i = 0; i < 15; ++i)
            {
                if (kRefFields[i].dev != d)
                    continue;
                HWND lbl = mkLabel(kRefFields[i].label, lblX, rowY, L.ref_lbl_w[d]);
                HWND ed = mkEdit(IDC_REF0 + i, editX, rowY, editW);
                addTip(lbl, kRefFields[i].tip);
                addTip(ed, kRefFields[i].tip);
                rowY += L.field_step;
            }
            cardX += L.ref_card_w + L.ref_card_gap;
        }
    }

    // 外观（右半列）：数值 2 列 × 3 行；下面是电量配色（深/浅两列）3 行 + 列头；
    // 再下面是两个显示开关与无读数文本
    mkGroup(L"外观", L.app_x, L.app_y, L.app_w, L.app_h);
    for (int i = 0; i < 6; ++i)
    {
        const int c = i % 2;
        const int y = L.app_num_y + (i / 2) * L.field_step;
        HWND lbl = mkLabel(kNumFields[i].label, L.app_x + L.num.col[c].lbl_x, y,
                           L.num.col[c].lbl_w);
        HWND ed = mkEdit(IDC_NUM0 + i, L.app_x + L.num.col[c].edit_x, y, L.num.col[c].edit_w);
        addTip(lbl, kNumFields[i].tip);
        addTip(ed, kNumFields[i].tip);
    }
    {
        // 电量配色（三行 × 深/浅两列）
        const int bx = L.color_blk_x;
        mkLabel(L"电量颜色", bx, L.color_y0, L.color_lbl_col_w);
        for (int c = 0; c < 2; ++c)
            mkLabel(kColorHeads[c], bx + L.color_chip_off[c], L.color_y0, L.color_chip_w);

        int y = L.color_row_y;
        for (int row = 0; row < kColorRowsCount; ++row)
        {
            mkLabel(kColorRows[row], bx, y, L.color_lbl_col_w);
            for (int c = 0; c < 2; ++c)
            {
                const int i = row * 2 + c;
                HWND e = mkEdit(IDC_COLOR0 + i, bx + L.color_chip_off[c], y, L.color_chip_w);
                if (e)
                {
                    SendMessageW(e, EM_SETLIMITTEXT, 7, 0); // #RRGGBB 上限
                    g_chips.edit[i] = e;
                }
            }
            y += L.field_step;
        }
    }
    {
        // 两个开关排一行，无读数文本在下一行
        auto mkCheck = [&](UINT id, const wchar_t* text, int x, int y, int w) {
            HWND ctrl = CreateWindowExW(0, L"BUTTON", text,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                        x, y, w, L.field_h, hDlg, (HMENU)(UINT_PTR)id, hInst, nullptr);
            if (ctrl)
                SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
            return ctrl;
        };
        const int cx = L.app_x + S(12);
        mkCheck(IDC_SHOW_SEP, L"显示设备分隔线", cx, L.sw_y, L.chk_w1);
        if (HWND pct = mkCheck(IDC_SHOW_PCT, L"显示百分号", cx + L.chk_w1 + S(24), L.sw_y, L.chk_w2))
            addTip(pct, L"任务栏双行显示时生效，默认开启");

        const int y = L.sw_y + L.field_step;
        mkLabel(kNoReadLabel, L.app_txt_x, y, L.app_txt_lbl_w);
        HWND e = mkEdit(IDC_NOREAD, L.app_txt_edit_x, y, L.app_txt_edit_w);
        if (e)
            SendMessageW(e, EM_SETLIMITTEXT, (WPARAM)kNoReadMax, 0);
    }

    FillAdvancedFields(hDlg);
    RebuildDevList(hDlg, -1);

    // 底部：左侧恢复默认（连同顺序、勾选一起还原），右侧确定 / 应用 / 取消
    const int btnGap = S(8);
    const int footX = L.w - L.margin - L.btn_action_w;
    mk(IDC_RESET, L"恢复默认", L.margin, L.bottom_y, L.btn_w, L.btn_h, BS_OWNERDRAW);
    mk(IDC_OK, L"确定", footX - (L.btn_action_w + btnGap) * 2, L.bottom_y, L.btn_action_w, L.btn_h,
       BS_OWNERDRAW | BS_DEFPUSHBUTTON);
    mk(IDC_APPLY, L"应用", footX - (L.btn_action_w + btnGap), L.bottom_y, L.btn_action_w, L.btn_h,
       BS_OWNERDRAW);
    mk(IDC_CANCEL, L"取消", footX, L.bottom_y, L.btn_action_w, L.btn_h, BS_OWNERDRAW);

    g_hover_btn = nullptr; // 悬停状态复位（自绘悬停态）

    m_option_changed = false;
    if (parent)
        EnableWindow(parent, FALSE);

    // 屏幕工作区居中（不依赖父窗口位置，避免对话框被摆到屏幕外）
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    // 模板尺寸以对话框单位计，这里按像素设定精确窗口尺寸后显示
    SetWindowPos(hDlg, nullptr, x, y, W, H, SWP_NOZORDER | SWP_SHOWWINDOW);
    UpdateWindow(hDlg);

    // 模态消息循环：取本线程所有消息（子控件消息也在此队列），
    // 子控件导航交给 IsDialogMessage（Tab 切换、回车确认、Esc 取消），对话框销毁即退出。
    MSG msg;
    int gm = -1;
    while ((gm = (int)GetMessageW(&msg, nullptr, 0, 0)) > 0)
    {
        if (!IsDialogMessageW(hDlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hDlg))
            break;
    }
    // gm == 0：收到 WM_QUIT（主程序退出中）；gm == -1：GetMessage 出错。
    // 这两条路径循环提前结束，对话框可能尚未销毁，兜底 Destroy 防句柄泄漏。
    if (IsWindow(hDlg))
        DestroyWindow(hDlg);

    // 取消 / 叉掉：丢弃工作副本，不写回配置（任务栏不受影响）

    if (parent)
    {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent); // 关闭后把前台还给父窗口，避免焦点跳到其他应用
    }
    if (gm == 0)
        PostQuitMessage((int)msg.wParam); // 透传收到的 WM_QUIT，主程序的退出流程得以继续

    DeleteObject(hFont);
    if (g_list_font)
    {
        DeleteObject(g_list_font);
        g_list_font = nullptr;
    }
    return m_option_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

void* Plugin::GetPluginIcon()
{
    // 任务栏插件管理列表按小图标尺寸显示；按当前 DPI 取对应尺寸避免缩放发虚
    static HICON icon = (HICON)LoadImageW(g_hDllModule, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON),
                                         LR_DEFAULTCOLOR);
    return icon;
}

ITMPlugin* TMPluginGetInstance()
{
    return &Plugin::Instance();
}
