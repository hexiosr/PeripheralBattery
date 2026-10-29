#include "Plugin.h"
#include <windows.h>
#include <windowsx.h>
#include <cstdio>

// ============ 设备条目表 ============
// 新增设备：1) 在下面表里加一行 2) 实现对应 Reader 并加入 Plugin 3) kDeviceCount +1
// 选项对话框会自动按此表生成勾选项，无需改界面代码。
struct DeviceEntry {
    const wchar_t* name; // 任务栏前缀名
    const wchar_t* desc; // 选项列表里显示的全名
};
const DeviceEntry kDevices[kDeviceCount] = {
    { L"键盘", L"键盘电量（迈从 G87）" },
    { L"鼠标", L"鼠标电量（迈从 A5 Ultra）" },
    { L"G7 Pro", L"G7 Pro 手柄电量（盖世小鸡）" },
    { L"DS5",  L"PS5 手柄电量（DualSense）" },
};

// 悬停提示的设备图标前缀（GDI 字体链接渲染为单色符号；
// emoji 为 UTF-16 代理对，\u 转义不允许代理区字符，须用 \x 逐值书写）
static const wchar_t* const kTipIconKb  = L"\u2328 ";       // ⌨
static const wchar_t* const kTipIconMs  = L"\xD83D\xDDB1 "; // 🖱
static const wchar_t* const kTipIconPad = L"\xD83C\xDFAE "; // 🎮

Plugin Plugin::m_instance;

Plugin::Plugin()
{
    // 预填显示数组：首次 DataRequired 之前，自绘也能按完整布局计算宽度
    for (int i = 0; i < kDeviceCount; ++i)
        m_display[i].name = kDevices[i].name;
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
    case 0: return &m_item;    // 外设电量显示（组合键盘 / 鼠标 / G7 Pro / PS5）
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
    std::wstring kb_txt, ms_txt, g7_txt, ds_txt;
    BuildValueText(m_key_reader.GetSnapshot(), kb_txt);
    BuildValueText(m_mouse_reader.GetSnapshot(), ms_txt);
    BuildG7ValueText(m_g7_reader.GetSnapshot(), g7_txt); // G7 用电池图标（4 档）
    BuildValueText(m_ds_reader.GetSnapshot(), ds_txt);
    if (ds_txt == L"--%")
        ds_txt = L"未连接"; // PS5 断开（PnP 检测）时显示"未连接"

    // 组合显示一块，按选项设置的勾选与顺序过滤设备
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
        if (m_show[dev])
            append(std::wstring(kDevices[dev].name) + L": " + *txts[dev]);
    }
    if (val.empty())
        val = L"已全部关闭";
    m_value_text = val;

    std::wstring tip;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        int dev = m_order[i];
        if (!m_show[dev])
            continue;
        switch (dev)
        {
        case 0: BuildKeyboardTooltip(m_key_reader.GetSnapshot(), tip); break;
        case 1: BuildMouseTooltip(m_mouse_reader.GetSnapshot(), tip); break;
        case 2: BuildG7Tooltip(m_g7_reader.GetSnapshot(), tip); break;
        case 3: BuildDualSenseTooltip(m_ds_reader.GetSnapshot(), tip); break;
        }
        tip += L'\n';
    }
    if (!tip.empty())
        tip.pop_back();
    m_tooltip = tip;

    // 自绘显示数据（与上面文本同序：仅勾选设备）
    // 键盘/鼠标无读数 → 空电池 + --；G7/DS5 无读数 → 未连接（×）
    m_display_count = 0;
    for (int i = 0; i < kDeviceCount; ++i)
    {
        int dev = m_order[i];
        if (!m_show[dev])
            continue;
        DeviceDisplay& d = m_display[m_display_count++];
        d = DeviceDisplay{};
        d.name = kDevices[dev].name;
        switch (dev)
        {
        case 0: {
            const BatterySnapshot s = m_key_reader.GetSnapshot();
            if (s.has_device && s.online && s.valid)
            {
                d.percent = s.percent;
                d.charging = s.charging;
            }
            break;
        }
        case 1: {
            const BatterySnapshot s = m_mouse_reader.GetSnapshot();
            if (s.has_device && s.online && s.valid)
            {
                d.percent = s.percent;
                d.charging = s.charging;
            }
            break;
        }
        case 2: {
            const BatterySnapshot s = m_g7_reader.GetSnapshot();
            if (s.valid)
            {
                d.percent = s.percent;
                d.charging = s.charging;
            }
            else
                d.absent = true;
            break;
        }
        case 3: {
            const BatterySnapshot s = m_ds_reader.GetSnapshot();
            if (s.valid)
            {
                d.percent = s.percent;
                d.charging = s.charging;
            }
            else
                d.absent = true;
            break;
        }
        }
    }
}

// G7 粗档：电池图标（▮ 实心 / ▯ 空心），4 格表示 4 档
//  ▮▮▮▮ = 满   ▮▮▮▯ = 高   ▮▮▯▯ = 中   ▮▯▯▯ = 低   ⚡ 后缀 = 充电中
void Plugin::BuildG7ValueText(const BatterySnapshot& snap, std::wstring& out)
{
    if (!snap.valid || snap.percent < 0)
    {
        out = L"未连接";
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

void Plugin::BuildValueText(const BatterySnapshot& snap, std::wstring& out)
{
    if (!snap.has_device || !snap.online || !snap.valid)
    {
        out = L"--%"; // 未找到设备 / 离线 / 暂无有效读数
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

// 悬停提示为一行式：图标 名称（产品名）：状态 · 更新时间
void Plugin::BuildKeyboardTooltip(const BatterySnapshot& snap, std::wstring& out)
{
    out += kTipIconKb;
    out += L"键盘";
    if (!snap.product.empty())
        out += L"（" + snap.product + L"）";

    if (!snap.has_device)
        out += L"：未检测到接收器";
    else if (!snap.online)
        out += L"：离线/无响应";
    else if (!snap.valid)
        out += L"：等待上报…";
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

void Plugin::BuildMouseTooltip(const BatterySnapshot& snap, std::wstring& out)
{
    out += kTipIconMs;
    out += L"鼠标";
    if (!snap.product.empty())
        out += L"（" + snap.product + L"）";

    if (!snap.has_device)
        out += L"：未检测到接收器";
    else if (!snap.online)
        out += L"：离线/无响应";
    else if (!snap.valid)
        out += L"：等待上报…";
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

void Plugin::BuildG7Tooltip(const BatterySnapshot& snap, std::wstring& out)
{
    out += kTipIconPad;
    out += L"G7 Pro";

    if (!snap.has_device)
        out += L"：未检测到接收器";
    else if (!snap.valid)
        out += L"：未连接";
    else
    {
        int bars = 1;
        if (snap.percent >= 100) bars = 4;
        else if (snap.percent >= 66) bars = 3;
        else if (snap.percent >= 33) bars = 2;
        out += L"：";
        for (int i = 0; i < 4; ++i)
            out += (i < bars) ? L"\u25AE" : L"\u25AF"; // ▮ / ▯
        wchar_t buf[32];
        swprintf_s(buf, L" %d%%", snap.percent);
        out += buf;
        if (snap.charging)
            out += L" \u26A1充电中"; // ⚡
        AppendUpdatedAgo(snap, out);
    }
}

void Plugin::BuildDualSenseTooltip(const BatterySnapshot& snap, std::wstring& out)
{
    out += kTipIconPad;
    out += L"PS5";

    if (!snap.has_device)
        out += L"：未检测到 DualSense（DS5Dongle 桥接）";
    else if (!snap.valid)
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
        return L"0.2.1";
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

void Plugin::LoadConfig()
{
    if (!m_app)
        return;
    std::wstring path = m_app->GetPluginConfigDir();
    path += L"\\PeripheralBattery.ini";

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

    // 显示顺序：order=0,2,1,3（设备索引，逗号分隔）
    wchar_t ord[64];
    for (int i = 0; i < kDeviceCount; ++i)
        m_order[i] = i;
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
        m_dlg_order[d] = d;
    }
}

// ============ 选项设置对话框（Win11 风格自绘） ============
// 左侧：设备复选列表（白卡片、圆角行高亮、悬停态、圆角复选框）
// 右侧：上移 / 下移 / 恢复默认；底部：确定 / 应用 / 取消（右对齐）
// 按钮与列表全部自绘（圆角、悬停/按下态，默认按钮主题蓝）；Win11 下窗口圆角

namespace {

constexpr UINT IDC_LIST = 500;
constexpr UINT IDC_UP = 600;
constexpr UINT IDC_DOWN = 601;
constexpr UINT IDC_RESET = 602;
constexpr UINT IDC_OK = 700;
constexpr UINT IDC_CANCEL = 701;
constexpr UINT IDC_APPLY = 702;

// 布局（客户区坐标系）：列表白色卡片 + 右侧按钮列 + 底部确定/应用/取消
constexpr int kDlgW = 520;                                        // 客户区宽
constexpr int kListX = 16, kListY = 16, kListW = 360;             // 列表卡片位置
constexpr int kRowHeight = 40;                                    // 列表行高
constexpr int kBtnW = 112, kBtnH = 32;                            // 按钮尺寸
constexpr int kBottomY = kListY + kDeviceCount * kRowHeight + 16; // 底部按钮行 y
constexpr int kDlgH = kBottomY + kBtnH + 16;                      // 客户区高
constexpr int kCheckZoneW = 40;                                   // 复选框点击热区宽（12..32 + 边距）

WNDPROC g_origListProc = nullptr; // ListBox 原窗口过程（子类化用）
WNDPROC g_origBtnProc = nullptr;  // BUTTON 原窗口过程（子类化用）
HWND g_hover_btn = nullptr;       // 当前悬停的按钮（自绘悬停态）
int  g_hover_row = -1;            // 列表当前悬停行（自绘悬停态）

// 按当前顺序重建列表内容（自绘行数据 = 设备 id）
static void RebuildList(HWND hList)
{
    Plugin* self = (Plugin*)GetWindowLongPtrW(GetParent(hList), GWLP_USERDATA);
    if (!self)
        return;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (int row = 0; row < kDeviceCount; ++row)
    {
        int dev = self->DlgOrderAt(row);
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)kDevices[dev].desc);
        SendMessageW(hList, LB_SETITEMDATA, row, dev);
    }
    SendMessageW(hList, LB_SETCURSEL, 0, 0); // 默认选中第一行
}

// 选中指定设备所在行
static void SelectDev(HWND hList, int dev)
{
    for (int row = 0; row < kDeviceCount; ++row)
        if ((int)SendMessageW(hList, LB_GETITEMDATA, row, 0) == dev)
        {
            SendMessageW(hList, LB_SETCURSEL, row, 0);
            break;
        }
}

// ListBox 子类化：悬停跟踪 + 点击复选框热区切换勾选（其余走默认选中逻辑）
LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        {
            int rowH = (int)SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
            if (rowH <= 0)
                rowH = kRowHeight;
            int row = (int)(GET_Y_LPARAM(lp) / rowH);
            if (row < 0 || row >= kDeviceCount)
                row = -1;
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
        g_hover_row = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
        {
            Plugin* self = (Plugin*)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
            int rowH = (int)SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
            if (rowH <= 0)
                rowH = kRowHeight;
            int row = (int)(GET_Y_LPARAM(lp) / rowH);
            if (row >= 0 && row < kDeviceCount)
            {
                int x = GET_X_LPARAM(lp);
                if (x < kCheckZoneW) // 复选框热区
                {
                    int dev = (int)SendMessageW(hwnd, LB_GETITEMDATA, row, 0);
                    if (self)
                    {
                        self->DlgToggle(dev);
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                    return 0;
                }
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
    case WM_ERASEBKGND:
        // 浅灰画布，列表作为白色卡片浮在上面
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(247, 247, 248));
            FillRect((HDC)wp, &rc, br);
            DeleteObject(br);
            return 1;
        }
    case WM_PAINT:
        {
            // 列表卡片 1px 扁平边框（画在对话框表面，紧贴列表窗口外圈）
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HWND hList = GetDlgItem(hwnd, IDC_LIST);
            RECT rl;
            GetWindowRect(hList, &rl);
            MapWindowPoints(nullptr, hwnd, (POINT*)&rl, 2);
            InflateRect(&rl, 1, 1);
            HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, RGB(216, 216, 220)));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rl.left, rl.top, rl.right, rl.bottom);
            DeleteObject(SelectObject(hdc, op));
            SelectObject(hdc, ob);
            EndPaint(hwnd, &ps);
            return 0;
        }
    case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;

            // ---- 列表行：白卡片 + 圆角行高亮（选中淡蓝 / 悬停浅灰）+ 圆角复选框 ----
            if (dis->CtlID == IDC_LIST && dis->itemID != (UINT)-1)
            {
                int dev = (int)dis->itemData;
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                bool sel = (dis->itemState & ODS_SELECTED) != 0;
                bool hov = (g_hover_row == (int)dis->itemID) && !sel;

                COLORREF row_bg = sel ? RGB(233, 243, 255) : (hov ? RGB(244, 244, 246) : RGB(255, 255, 255));
                RECT rr = rc;
                InflateRect(&rr, -2, -2);
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, row_bg));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(row_bg));
                RoundRect(hdc, rr.left, rr.top, rr.right, rr.bottom, 6, 6);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                // 复选框（20×20、圆角 4px：未选白底灰边、选中主题蓝底白勾）
                int bh = rc.bottom - rc.top;
                int cs = 20;
                bool checked = self && self->DlgShowFor(dev);
                RECT box{ rc.left + 12, rc.top + (bh - cs) / 2, rc.left + 12 + cs, rc.top + (bh - cs) / 2 + cs };
                COLORREF fillCol = checked ? RGB(0, 103, 192) : RGB(255, 255, 255);
                COLORREF brdCol = checked ? RGB(0, 103, 192) : RGB(130, 130, 135);
                op = SelectObject(hdc, CreatePen(PS_SOLID, 1, brdCol));
                ob = SelectObject(hdc, CreateSolidBrush(fillCol));
                RoundRect(hdc, box.left, box.top, box.right, box.bottom, 4, 4);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));
                if (checked)
                {
                    // 白勾：两条线段（2px）
                    HGDIOBJ oh = SelectObject(hdc, CreatePen(PS_SOLID, 2, RGB(255, 255, 255)));
                    MoveToEx(hdc, box.left + 4, box.top + 10, nullptr);
                    LineTo(hdc, box.left + 8, box.top + 14);
                    LineTo(hdc, box.left + 15, box.top + 6);
                    DeleteObject(SelectObject(hdc, oh));
                }

                // 文本（深色，选中行不变白字）
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(20, 20, 20));
                RECT rcTxt = rc;
                rcTxt.left += 44;
                DrawTextW(hdc, kDevices[dev].desc, -1, &rcTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                return 0;
            }

            // ---- 按钮：圆角 4px，白底灰边；默认（确定）主题蓝底白字 ----
            if (dis->CtlType == ODT_BUTTON)
            {
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                bool defBtn = (dis->CtlID == IDC_OK);
                bool hover = (g_hover_btn == dis->hwndItem);
                bool pressed = (dis->itemState & ODS_SELECTED) != 0;
                COLORREF fill, border, txt;
                if (defBtn)
                {
                    fill = pressed ? RGB(0, 89, 169) : (hover ? RGB(26, 120, 208) : RGB(0, 103, 192));
                    border = RGB(0, 80, 158);
                    txt = RGB(255, 255, 255);
                }
                else
                {
                    fill = pressed ? RGB(231, 231, 234) : (hover ? RGB(243, 243, 243) : RGB(255, 255, 255));
                    border = RGB(186, 186, 190);
                    txt = RGB(20, 20, 20);
                }
                HGDIOBJ op = SelectObject(hdc, CreatePen(PS_SOLID, 1, border));
                HGDIOBJ ob = SelectObject(hdc, CreateSolidBrush(fill));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 5, 5);
                DeleteObject(SelectObject(hdc, op));
                DeleteObject(SelectObject(hdc, ob));

                wchar_t caption[64];
                int n = GetWindowTextW(dis->hwndItem, caption, 64);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, txt);
                DrawTextW(hdc, caption, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return 0;
            }
        }
    case WM_COMMAND:
        {
            UINT id = LOWORD(wp);
            HWND hList = GetDlgItem(hwnd, IDC_LIST);
            if (id == IDC_UP || id == IDC_DOWN)
            {
                int cur = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                if (cur != LB_ERR)
                {
                    int dev = (int)SendMessageW(hList, LB_GETITEMDATA, cur, 0);
                    if (id == IDC_UP) self->DlgMoveUp(dev);
                    else self->DlgMoveDown(dev);
                    RebuildList(hList);
                    SelectDev(hList, dev);
                    SetFocus(hList);
                }
                return 0;
            }
            if (id == IDC_RESET)
            {
                if (self)
                    self->DlgRestoreDefault();
                RebuildList(hList);
                SetFocus(hList);
                return 0;
            }
            switch (id)
            {
            case IDC_APPLY:
                // 立即保存并刷新显示，对话框保持打开（方便预览效果）
                if (self)
                {
                    self->DlgApply();
                    self->SetOptionChanged(true);
                }
                return 0;
            case IDC_OK:
                if (self)
                {
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
                return 0;
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
                return 0;
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
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

ITMPlugin::OptionReturn Plugin::ShowOptionsDialog(void* hParent)
{
    HWND parent = (HWND)hParent;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 由客户区尺寸反推整窗尺寸（标题栏/边框由系统计算，不手工留余量）
    RECT wnd{ 0, 0, kDlgW, kDlgH };
    AdjustWindowRectEx(&wnd, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, FALSE, 0);
    const int W = wnd.right - wnd.left, H = wnd.bottom - wnd.top;

    // 创建系统对话框窗口（#32770），再替换窗口过程接管消息
    HWND hDlg = CreateWindowExW(0, L"#32770", L"显示设置",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
                                CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                                parent, nullptr, hInst, nullptr);
    if (!hDlg)
        return OR_OPTION_UNCHANGED;
    SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)OptionDlgProc);
    SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)this);

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

    // 系统 UI 字体（Segoe UI / 微软雅黑），对话框与所有控件统一设置
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(hDlg, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 列表字体：比系统字体大两档（18px），选项文字更醒目
    LOGFONTW lfList = ncm.lfMessageFont;
    lfList.lfHeight = -18;
    HFONT hListFont = CreateFontIndirectW(&lfList);

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

    // 左侧设备列表：白色卡片、1px 扁平边框（对话框 WM_PAINT 绘制）、行高自适配无滚动条
    // 点击左侧复选框切换勾选，点击行选中（支持上移/下移）
    HWND hList = CreateWindowExW(0, L"LISTBOX", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                     LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY,
                                 kListX, kListY, kListW, kDeviceCount * kRowHeight,
                                 hDlg, (HMENU)(UINT_PTR)IDC_LIST, hInst, nullptr);
    SendMessageW(hList, WM_SETFONT, (WPARAM)hListFont, TRUE);
    SendMessageW(hList, LB_SETITEMHEIGHT, 0, kRowHeight);
    g_origListProc = (WNDPROC)SetWindowLongPtrW(hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
    RebuildList(hList);

    // 右侧操作按钮列（与列表顶部对齐）
    mk(IDC_UP, L"上移", kDlgW - 16 - kBtnW, kListY, kBtnW, kBtnH, BS_OWNERDRAW);
    mk(IDC_DOWN, L"下移", kDlgW - 16 - kBtnW, kListY + kBtnH + 8, kBtnW, kBtnH, BS_OWNERDRAW);
    mk(IDC_RESET, L"恢复默认", kDlgW - 16 - kBtnW, kListY + (kBtnH + 8) * 2, kBtnW, kBtnH, BS_OWNERDRAW);

    // 底部：确定 / 应用 / 取消（右对齐）
    mk(IDC_OK, L"确定", kDlgW - 16 - kBtnW * 3 - 16, kBottomY, kBtnW, kBtnH, BS_OWNERDRAW | BS_DEFPUSHBUTTON);
    mk(IDC_APPLY, L"应用", kDlgW - 16 - kBtnW * 2 - 8, kBottomY, kBtnW, kBtnH, BS_OWNERDRAW);
    mk(IDC_CANCEL, L"取消", kDlgW - 16 - kBtnW, kBottomY, kBtnW, kBtnH, BS_OWNERDRAW);

    g_hover_row = -1; // 悬停状态复位（自绘悬停态）
    g_hover_btn = nullptr;

    m_option_changed = false;
    if (parent)
        EnableWindow(parent, FALSE);

    // 屏幕工作区居中（不依赖父窗口位置，避免对话框被摆到屏幕外）
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
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
    DeleteObject(hListFont);
    return m_option_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

ITMPlugin* TMPluginGetInstance()
{
    return &Plugin::Instance();
}
