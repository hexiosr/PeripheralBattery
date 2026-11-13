#include "PeripheralItem.h"
#include "Plugin.h"
#include "BatteryDraw.h"

const wchar_t* PeripheralItem::GetItemName() const
{
    return L"外设电量显示";
}

const wchar_t* PeripheralItem::GetItemId() const
{
    return L"peripheralBattery";
}

const wchar_t* PeripheralItem::GetItemLableText() const
{
    return L"";
}

const wchar_t* PeripheralItem::GetItemValueText() const
{
    return Plugin::Instance().CombinedValueText().c_str();
}

const wchar_t* PeripheralItem::GetItemValueSampleText() const
{
    return Plugin::Instance().CombinedSampleText().c_str();
}

bool PeripheralItem::IsCustomDraw() const
{
    return true; // 自绘：电量圆环 + 电量配色 + 短分隔线，纯字符文本无法实现
}

int PeripheralItem::GetItemWidth() const
{
    // 96 DPI 基准宽度（主程序会按系统 DPI 放大）
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc)
        return 0;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(hdc, font);
    Plugin& p = Plugin::Instance();
    const DisplayConfig cfg = PluginConfig::Instance().Display();
    int w = p.HasPlaceholder()
                ? batdraw::PlaceholderWidth(hdc, p.CombinedValueText().c_str(), cfg)
                : batdraw::LayoutWidth(hdc, p.Display(), p.DisplayCount(), cfg);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return w;
}

int PeripheralItem::GetItemWidthEx(void* hDC) const
{
    // 实际 hDC 下的精确宽度（主程序不缩放）
    Plugin& p = Plugin::Instance();
    const DisplayConfig cfg = PluginConfig::Instance().Display();
    if (p.HasPlaceholder())
        return batdraw::PlaceholderWidth((HDC)hDC, p.CombinedValueText().c_str(), cfg);
    return batdraw::LayoutWidth((HDC)hDC, p.Display(), p.DisplayCount(), cfg);
}

void PeripheralItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    Plugin& p = Plugin::Instance();
    const DisplayConfig cfg = PluginConfig::Instance().Display();
    if (p.HasPlaceholder())
        batdraw::DrawPlaceholder((HDC)hDC, x, y, w, h, dark_mode, p.CombinedValueText().c_str(), cfg);
    else
        batdraw::DrawSegments((HDC)hDC, x, y, h, dark_mode, p.Display(), p.DisplayCount(), cfg);
}
