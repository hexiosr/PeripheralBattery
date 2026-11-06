#pragma once
// ============================================================
// PeripheralItem.h —— 外设电量显示项（IPluginItem 实现）
// 单个组合显示项：键盘 / 鼠标 / 手柄（G7 Pro 与 DualSense 自动识别），
// 在插件选项对话框中勾选需要显示的设备（自绘：电量圆环 + 环内数字/⚡）
// ============================================================
#include "PluginInterface.h"

class PeripheralItem : public IPluginItem
{
public:
    PeripheralItem() = default;

    virtual const wchar_t* GetItemName() const override;
    virtual const wchar_t* GetItemId() const override;
    virtual const wchar_t* GetItemLableText() const override;
    virtual const wchar_t* GetItemValueText() const override;
    virtual const wchar_t* GetItemValueSampleText() const override;
    virtual bool IsCustomDraw() const override;
    virtual int GetItemWidth() const override;
    virtual int GetItemWidthEx(void* hDC) const override;
    virtual void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;
};
