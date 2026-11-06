#pragma once
// ============================================================
// BatteryDraw.h —— 任务栏外设电量绘制（GDI + GDI+ 圆环）
// 一段 = 名称: [电量圆环(内含百分比/充电图标)]；设备间细竖线分隔。
//  - 圆环用 GDI+ 抗锯齿绘制（连续曲率，无锯齿），按比例顺时针描弧
//  - 配色：≥60% 绿 / 20~59% 黄 / <20% 红（淡灰轨道）
//  - 百分比加粗画在圆环内孔中央（不显示 %）；充电时环内改画 ⚡
//  - 无读数 / 未连接 → 空圆环 + --（所有设备一致）
//  - 圆环直径固定，读数 / 充电切换不引起任务栏宽度抖动
// ============================================================
#include <windows.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdio>
#include <cwchar>

// 单个设备的显示状态（由插件主类在 UI 线程更新）
struct DeviceDisplay
{
    const wchar_t* name = L"";  // 前缀名
    int  percent = -1;          // 电量百分比；-1 = 暂无读数 / 未连接
    bool charging = false;      // 充电中（环内画 ⚡，不显示数字）
};

namespace batdraw {

constexpr int kPadLR = 1;      // 左右内边距（TrafficMonitor 自带条目文字从槽位左缘起始，内边距越大插件条目越显右移）
constexpr int kSepW = 17;      // 设备分隔宽度（8 + 1px 竖线 + 8）

// 段内间距：随字号缩放（高 DPI 下不显得拥挤）
inline int GapFor(HDC hdc)
{
    TEXTMETRIC tm{};
    GetTextMetrics(hdc, &tm);
    int g = tm.tmHeight / 7;
    return g < 3 ? 3 : g;
}

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

// 跟随当前字体族、按给定像素高度/字重创建字体
inline HFONT MakeFont(HDC hdc, int pixel_h, int weight)
{
    wchar_t face[64];
    GetTextFaceW(hdc, 64, face);
    return CreateFontW(-pixel_h, 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// 圆环尺寸按当前字体缩放（兼容不同 DPI / 任务栏字体）
// 外径放大到约 1.5 倍字高，内孔才容得下百分比数字
struct RingMetrics
{
    int dia;     // 外径（含描边）
    int stroke;  // 环宽
    int inner;   // 内孔直径
};

inline RingMetrics RingSize(HDC hdc)
{
    TEXTMETRIC tm{};
    GetTextMetrics(hdc, &tm);
    RingMetrics r;
    r.dia = tm.tmHeight * 3 / 2;
    if (r.dia < 17) r.dia = 17;
    r.stroke = r.dia / 8;
    if (r.stroke < 2) r.stroke = 2;
    r.inner = r.dia - r.stroke * 2;
    return r;
}

// 环内百分比排版：加粗数字，字号收窄到内孔装得下 "100"
struct NumMetrics
{
    int num_h = 0;
};

inline NumMetrics NumSize(HDC hdc, const RingMetrics& rm)
{
    NumMetrics n;
    int nh = rm.inner * 3 / 5;
    if (nh < 8) nh = 8;
    const int limit = rm.inner * 92 / 100;
    for (;;)
    {
        HFONT nf = MakeFont(hdc, nh, FW_BOLD);
        HGDIOBJ of = SelectObject(hdc, nf);
        int w = TextW(hdc, L"100");
        SelectObject(hdc, of);
        DeleteObject(nf);
        if (w <= limit || nh <= 8)
        {
            n.num_h = nh;
            break;
        }
        --nh;
    }
    return n;
}

struct BoltMetrics
{
    int w = 0;
    int h = 0;
};

// 充电闪电：嵌在圆环内孔中央（充电时替代百分比数字）
inline BoltMetrics BoltSize(const RingMetrics& rm)
{
    BoltMetrics b;
    b.h = rm.inner * 3 / 4;
    if (b.h < 6) b.h = 6;
    b.w = b.h * 7 / 10;
    if (b.w < 4) b.w = 4;
    return b;
}

inline COLORREF LevelColor(int percent, bool dark)
{
    if (percent >= 60) return dark ? RGB(112, 205, 126) : RGB(56, 158, 68);
    if (percent >= 20) return dark ? RGB(255, 194, 64)  : RGB(210, 136, 0);
    return                     dark ? RGB(255, 116, 122) : RGB(214, 55, 66);
}

// 圆环轨道色：比背景略亮的灰，不抢电量弧
inline COLORREF TrackColor(bool dark)
{
    return dark ? RGB(82, 82, 88) : RGB(206, 206, 212);
}

struct SegMetrics
{
    RingMetrics ring;
    NumMetrics num;
    int gap_colon = 1;  // 中文冒号自带右侧留白，后面只补很小间距
    int name_w = 0;     // 名称宽
    int colon_w = 0;    // 名称后冒号宽
    int total = 0;     // 段总宽
};

inline SegMetrics MeasureSeg(HDC hdc, const DeviceDisplay& d)
{
    SegMetrics s;
    s.gap_colon = GapFor(hdc) / 3;
    if (s.gap_colon < 1) s.gap_colon = 1;
    s.name_w = TextW(hdc, d.name);
    s.colon_w = TextW(hdc, L"：");
    s.ring = RingSize(hdc);
    s.num = NumSize(hdc, s.ring);
    s.total = s.name_w + s.colon_w + s.gap_colon + s.ring.dia;
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

// GDI+ 启动（首次绘制时初始化；插件进程内长期有效，无需 Shutdown）
inline ULONG_PTR GdiplusToken()
{
    static ULONG_PTR token = 0;
    static bool ok = [] {
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ok ? token : 0;
}

inline Gdiplus::Color GpColor(COLORREF c)
{
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// 圆环用 GDI+ 抗锯齿绘制：连续曲率、圆头端点，无锯齿
inline void DrawRingGdiplus(HDC dc, int cx, int cy, const RingMetrics& rm, int percent, bool dark)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const Gdiplus::REAL r = (rm.dia - rm.stroke) / 2.0f;
    const Gdiplus::REAL x = (Gdiplus::REAL)cx - r;
    const Gdiplus::REAL y = (Gdiplus::REAL)cy - r;
    const Gdiplus::REAL d = r * 2.0f;

    Gdiplus::Pen track(GpColor(TrackColor(dark)), (Gdiplus::REAL)rm.stroke);
    g.DrawEllipse(&track, x, y, d, d);

    if (percent > 0)
    {
        Gdiplus::Pen arc(GpColor(LevelColor(percent, dark)), (Gdiplus::REAL)rm.stroke);
        arc.SetStartCap(Gdiplus::LineCapRound);
        arc.SetEndCap(Gdiplus::LineCapRound);
        if (percent >= 100)
            g.DrawEllipse(&arc, x, y, d, d);
        else
            g.DrawArc(&arc, x, y, d, d, -90.0f, 3.6f * percent); // -90°=12 点，顺时针
    }
}

// 电量圆环：淡灰轨道 + 按比例顺时针描弧（从 12 点方向起）
// 优先 GDI+ 抗锯齿；极端情况下 GDI+ 启动失败时退回折线描边
inline void DrawRing(HDC dc, int cx, int cy, const RingMetrics& rm, int percent, bool dark)
{
    if (GdiplusToken())
    {
        DrawRingGdiplus(dc, cx, cy, rm, percent, dark);
        return;
    }

    int r = (rm.dia - rm.stroke) / 2;
    if (r < 3) r = 3;
    LOGBRUSH lb{ BS_SOLID, TrackColor(dark), 0 };
    HPEN track = ExtCreatePen(PS_GEOMETRIC | PS_SOLID, rm.stroke, &lb, 0, nullptr);
    HGDIOBJ op = SelectObject(dc, track);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    DeleteObject(track);
    if (percent > 0)
    {
        LOGBRUSH ab{ BS_SOLID, LevelColor(percent, dark), 0 };
        HPEN arc = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND, rm.stroke, &ab, 0, nullptr);
        op = SelectObject(dc, arc);
        if (percent >= 100)
            Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        else
        {
            POINT pts[65];
            int steps = 64 * percent / 100;
            if (steps < 4) steps = 4;
            for (int i = 0; i <= steps; ++i)
            {
                double a = 3.6 * percent * i / steps * 3.14159265358979 / 180.0;
                pts[i].x = cx + (int)lround(r * std::sin(a));
                pts[i].y = cy - (int)lround(r * std::cos(a));
            }
            Polyline(dc, pts, steps + 1);
        }
        SelectObject(dc, op);
        DeleteObject(arc);
    }
    SelectObject(dc, ob);
}

// 百分比画在圆环内孔中央（不显示 %）：数值等宽居中，读数变化不抖动
inline void DrawPercent(HDC dc, int ring_cx, int y, int h, const RingMetrics& rm,
                        const NumMetrics& nm, int percent, bool dark)
{
    wchar_t buf[8];
    const wchar_t* s = L"--";
    if (percent >= 0)
    {
        swprintf_s(buf, L"%d", percent);
        s = buf;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, percent >= 0 ? LevelColor(percent, dark) : MutedColor(dark));

    HFONT nf = MakeFont(dc, nm.num_h, FW_BOLD);
    HGDIOBJ of = SelectObject(dc, nf);
    RECT rd{ ring_cx - rm.dia / 2, y, ring_cx + (rm.dia + 1) / 2, y + h };
    DrawTextW(dc, s, -1, &rd, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(nf);
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

    int cy = y + h / 2; // 图标 / 圆环 / 数字的垂直中心

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

        // 名称: （与任务栏文字同色）
        SetTextColor(dc, text_col);
        {
            RECT rcName{ cx, y, cx + m.name_w + 2, y + h };
            DrawTextW(dc, d.name, -1, &rcName, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            int colon_x = cx + m.name_w;
            RECT rcColon{ colon_x, y, colon_x + m.colon_w + 2, y + h };
            DrawTextW(dc, L"：", 1, &rcColon, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        int value_x = cx + m.name_w + m.colon_w + m.gap_colon;

        // 电量圆环：充电时环内画 ⚡ 替代数字，否则画百分比
        int ring_cx = value_x + m.ring.dia / 2;
        DrawRing(dc, ring_cx, cy, m.ring, d.percent, dark);
        if (d.charging && d.percent >= 0)
        {
            BoltMetrics b = BoltSize(m.ring);
            DrawBolt(dc, ring_cx - b.w / 2, cy - b.h / 2, b.w, b.h,
                     LevelColor(d.percent, dark));
        }
        else
        {
            DrawPercent(dc, ring_cx, y, h, m.ring, m.num, d.percent, dark);
        }

        cx += m.total;
    }
}

} // namespace batdraw
