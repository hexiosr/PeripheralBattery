#pragma once
// ============================================================
// BatteryDraw.h —— 任务栏电池图形绘制与布局（纯 GDI，无插件依赖）
// 一段 = 名称: [电池图形（内含电量数字与 ⚡ 充电，无百分号）]；设备间细竖线分隔。
//  - 电量配色：≥60% 绿 / 20~59% 黄 / <20% 红（淡色填充 + 同色系数字）
//  - 无读数 → 空电池 + --；未连接 → 整段弱化灰，显示"未连接"
//  - 电池宽度按 "100" 数字固定，读数变化不引起任务栏宽度抖动
// ============================================================
#include <windows.h>
#include <cstdio>
#include <cwchar>

// 单个设备的显示状态（由插件主类在 UI 线程更新）
struct DeviceDisplay
{
    const wchar_t* name = L"";  // 前缀名
    int  percent = -1;          // 电量百分比；-1 = 暂无读数
    bool charging = false;      // 充电中（闪电嵌在电池内数字右侧）
    bool absent = false;        // 未连接（整段弱化灰，只显示"未连接"）
};

namespace batdraw {

constexpr int kPadLR = 1;    // 左右内边距（TrafficMonitor 自带条目文字从槽位左缘起始，内边距越大插件条目越显右移）
constexpr int kGap = 3;      // 名称与电池的间距
constexpr int kBoltPad = 2;  // 电池内闪电与数字的间距
constexpr int kSepW = 17;    // 设备分隔宽度（8 + 1px 竖线 + 8）

struct BatMetrics
{
    int body_w;       // 电池壳宽
    int body_h;       // 电池壳高
    int nub_w;        // 电极宽
    int total_w;      // 壳 + 电极总宽
    int bolt_w = 0;   // 电池内闪电宽（仅充电段计入壳宽）
    int bolt_h = 0;   // 电池内闪电高
};

inline int TextW(HDC hdc, const wchar_t* s)
{
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

// 弱化灰（未连接 / 无读数）
inline COLORREF MutedColor(bool dark)
{
    return dark ? RGB(152, 152, 158) : RGB(118, 118, 124);
}

// 电池内数字用的小号字体（跟随当前字体族，按给定像素高度创建）
inline HFONT MakeDigitFont(HDC hdc, int pixel_h)
{
    wchar_t face[64];
    GetTextFaceW(hdc, 64, face);
    return CreateFontW(-pixel_h, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// 电池尺寸按当前字体缩放（兼容不同 DPI / 任务栏字体）：
// 高度约为文字行高的 3/4（比整行文字小一档）；宽度按小号字体最宽读数 "100" 定宽
inline BatMetrics BatSize(HDC hdc)
{
    TEXTMETRIC tm{};
    GetTextMetrics(hdc, &tm);
    BatMetrics m;
    m.body_h = tm.tmHeight * 3 / 4;
    if (m.body_h < 11) m.body_h = 11;
    m.bolt_h = (m.body_h - 4) * 3 / 4;
    m.bolt_w = m.bolt_h * 7 / 10;
    if (m.bolt_w < 4) m.bolt_w = 4;
    HFONT df = MakeDigitFont(hdc, m.body_h - 4);
    HGDIOBJ of = SelectObject(hdc, df);
    m.body_w = TextW(hdc, L"100") + 12;
    SelectObject(hdc, of);
    DeleteObject(df);
    int min_w = m.body_h * 3 / 2;
    if (m.body_w < min_w) m.body_w = min_w;
    m.nub_w = 2;
    m.total_w = m.body_w + m.nub_w;
    return m;
}

inline COLORREF LevelColor(int percent, bool dark)
{
    if (percent >= 60) return dark ? RGB(112, 205, 126) : RGB(56, 158, 68);
    if (percent >= 20) return dark ? RGB(255, 194, 64)  : RGB(210, 136, 0);
    return                     dark ? RGB(255, 116, 122) : RGB(214, 55, 66);
}

// 电量填充底色：LevelColor 的淡色版，保证电池内数字（LevelColor）在填充上仍可读
inline COLORREF TintColor(int percent, bool dark)
{
    if (percent >= 60) return dark ? RGB(38, 84, 52)  : RGB(198, 239, 206);
    if (percent >= 20) return dark ? RGB(96, 74, 22)  : RGB(248, 222, 160);
    return                     dark ? RGB(92, 38, 44)  : RGB(250, 205, 210);
}

// 充电闪电颜色：跟随电量色（与数字同色）；无读数时金色兜底
inline COLORREF BoltColor(int percent, bool dark)
{
    return percent >= 0 ? LevelColor(percent, dark)
                        : (dark ? RGB(255, 213, 90) : RGB(232, 164, 0));
}

struct SegMetrics
{
    BatMetrics bat;
    int name_w = 0;   // 名称宽
    int colon_w = 0;  // 名称后冒号宽
    int value_w = 0;  // "未连接"文字宽（仅未连接段使用）
    int total = 0;    // 段总宽
};

inline SegMetrics MeasureSeg(HDC hdc, const DeviceDisplay& d)
{
    SegMetrics s;
    s.bat = BatSize(hdc);
    s.name_w = TextW(hdc, d.name);
    s.colon_w = TextW(hdc, L":");
    if (d.absent)
    {
        // 未连接：只有 名称: + "未连接" 文字，无电池图形
        s.value_w = TextW(hdc, L"未连接");
        s.total = s.name_w + s.colon_w + kGap + s.value_w;
        return s;
    }
    if (d.charging)
    {
        // 充电：闪电嵌在电池内，壳宽随之加宽
        s.bat.body_w += s.bat.bolt_w + kBoltPad;
        s.bat.total_w = s.bat.body_w + s.bat.nub_w;
    }
    s.total = s.name_w + s.colon_w + kGap + s.bat.total_w;
    return s;
}

// 整个显示项的宽度（与 DrawSegments 的布局完全一致）
inline int LayoutWidth(HDC hdc, const DeviceDisplay* list, int n)
{
    if (n <= 0)
        return TextW(hdc, L"已全部关闭") + kPadLR * 2 + 2;
    int w = kPadLR * 2;
    for (int i = 0; i < n; ++i)
    {
        if (i) w += kSepW;
        w += MeasureSeg(hdc, list[i]).total;
    }
    return w;
}

// ⚡ 闪电（归一化轮廓 0~1，缩放到给定矩形）
inline void DrawBolt(HDC dc, int x, int y, int w, int h, COLORREF col)
{
    static const double pts[][2] = {
        { 0.62, 0.00 }, { 0.20, 0.56 }, { 0.47, 0.56 }, { 0.38, 1.00 },
        { 0.84, 0.34 }, { 0.56, 0.34 }, { 0.74, 0.00 },
    };
    POINT p[7];
    for (int i = 0; i < 7; ++i)
    {
        p[i].x = x + (int)(pts[i][0] * (w - 1) + 0.5);
        p[i].y = y + (int)(pts[i][1] * (h - 1) + 0.5);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, br);
    Polygon(dc, p, 7);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

// 电池图形：圆角外壳 + 右侧电极 + 按电量比例淡色填充 + 电池内 电量数字 [⚡]（无百分号）
inline void DrawBattery(HDC dc, int left, int cy, const BatMetrics& bm,
                        const DeviceDisplay& d, bool dark)
{
    int top = cy - bm.body_h / 2;
    int bottom = top + bm.body_h;
    COLORREF frame = dark ? RGB(150, 150, 155) : RGB(125, 125, 130);

    // 电极（右侧凸头）
    HBRUSH nbr = CreateSolidBrush(frame);
    RECT nub{ left + bm.body_w, cy - bm.body_h / 4, left + bm.body_w + bm.nub_w, cy + bm.body_h / 4 + 1 };
    FillRect(dc, &nub, nbr);
    DeleteObject(nbr);

    // 外壳（1px 圆角描边）
    HPEN pen = CreatePen(PS_SOLID, 1, frame);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, left, top, left + bm.body_w + 1, bottom + 1, 2, 2);
    SelectObject(dc, ob);

    if (d.percent >= 0)
    {
        // 电量填充（内缩 2px，最低 2px 保证低电量可见；淡色，不压数字）
        int fill_w = (bm.body_w - 3) * d.percent / 100;
        if (fill_w < 2) fill_w = 2;
        HBRUSH fb = CreateSolidBrush(TintColor(d.percent, dark));
        RECT rc{ left + 2, top + 2, left + 2 + fill_w, bottom - 1 };
        FillRect(dc, &rc, fb);
        DeleteObject(fb);
    }

    SelectObject(dc, op);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);

    // 电量数字 + ⚡ 作为一组整体居中（无读数显示 --）
    wchar_t buf[8];
    const wchar_t* s = L"--";
    if (d.percent >= 0)
    {
        swprintf_s(buf, L"%d", d.percent);
        s = buf;
    }
    SetTextColor(dc, d.percent >= 0 ? LevelColor(d.percent, dark) : MutedColor(dark));
    HFONT df = MakeDigitFont(dc, bm.body_h - 4);
    HGDIOBJ of = SelectObject(dc, df);
    int text_w = TextW(dc, s);
    int group_w = text_w;
    if (d.charging) group_w += kBoltPad + bm.bolt_w;
    int group_left = left + (bm.body_w - group_w) / 2;
    RECT rcText{ group_left, top, group_left + text_w, bottom };
    DrawTextW(dc, s, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(df);

    if (d.charging)
        DrawBolt(dc, group_left + text_w + kBoltPad, cy - bm.bolt_h / 2,
                 bm.bolt_w, bm.bolt_h, BoltColor(d.percent, dark));
}

// 绘制整个显示项：竖排居中，段间细分隔线
inline void DrawSegments(HDC dc, int x, int y, int w, int h, bool dark,
                         const DeviceDisplay* list, int n)
{
    SetBkMode(dc, TRANSPARENT);
    COLORREF text_col  = dark ? RGB(235, 235, 235) : RGB(30, 30, 30);
    COLORREF muted_col = MutedColor(dark);

    if (n <= 0)
    {
        const wchar_t* s = L"已全部关闭";
        SetTextColor(dc, muted_col);
        RECT rcAll{ x, y, x + w, y + h };
        DrawTextW(dc, s, -1, &rcAll, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }

    TEXTMETRIC tm{};
    GetTextMetrics(dc, &tm);
    // 文字用 DT_VCENTER 与 TrafficMonitor 自带条目同一绘制路径，保证基线一致；
    // 电池/闪电以文字行中心对齐（不绑定条目矩形中心，避免混合基准错位）
    int text_top = y + (h - tm.tmHeight) / 2;
    int cy = text_top + tm.tmHeight / 2;

    int cx = x + kPadLR;
    for (int i = 0; i < n; ++i)
    {
        const DeviceDisplay& d = list[i];
        SegMetrics m = MeasureSeg(dc, d);

        if (i > 0) // 设备间细竖线（与文字同色，视觉上融入文本行）
        {
            HPEN pen = CreatePen(PS_SOLID, 1, text_col);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx + kSepW / 2, y + 4, nullptr);
            LineTo(dc, cx + kSepW / 2, y + h - 4);
            SelectObject(dc, op);
            DeleteObject(pen);
            cx += kSepW;
        }

        if (d.absent)
        {
            // 未连接：整段弱化灰，只显示 名称: 未连接
            SetTextColor(dc, muted_col);
            RECT rcName{ cx, y, cx + m.name_w, y + h };
            DrawTextW(dc, d.name, -1, &rcName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT rcColon{ cx + m.name_w, y, cx + m.name_w + m.colon_w, y + h };
            DrawTextW(dc, L":", 1, &rcColon, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT rcVal{ cx + m.name_w + m.colon_w + kGap, y, cx + m.name_w + m.colon_w + kGap + m.value_w, y + h };
            DrawTextW(dc, L"未连接", 3, &rcVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            cx += m.total;
            continue;
        }

        // 名称: （与数值同色，跟任务栏文字保持一致）
        SetTextColor(dc, text_col);
        {
            RECT rcName{ cx, y, cx + m.name_w, y + h };
            DrawTextW(dc, d.name, -1, &rcName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT rcColon{ cx + m.name_w, y, cx + m.name_w + m.colon_w, y + h };
            DrawTextW(dc, L":", 1, &rcColon, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        cx += m.name_w + m.colon_w + kGap;

        // 电池图形（⚡ 与电量数字画在电池内，无百分号）
        DrawBattery(dc, cx, cy, m.bat, d, dark);
        cx += m.bat.total_w;
    }
}

} // namespace batdraw
