#pragma once
// ============================================================
// Plugin.h —— 外设电量监控插件（ITMPlugin 实现）
// 单个组合显示项：外设电量显示（G87 / A5 Ultra / G7 Pro / DS5），
// 任务栏直接用型号名，不显示"键盘/鼠标"类别名；
// 插件选项对话框中可勾选显示哪些设备并调整顺序（勾选即显示），结果持久化到 ini；
// 未勾选的设备不会启动后台轮询。
// ============================================================
#include "PluginInterface.h"
#include "BatteryDraw.h"
#include "PeripheralItem.h"
#include "KeyboardReader.h"
#include "MouseReader.h"
#include "DualSenseReader.h"
#include "G7Reader.h"
#include <string>

// 设备条目数量（新增设备时同步 +1；显示名可在“显示设置”里改）
constexpr int kDeviceCount = 4;

// 默认显示顺序（设备索引）：按型号名首字母 A → Z：A5 Ultra、DS5、G7 Pro、G87
constexpr int kDefaultOrder[kDeviceCount] = { 1, 3, 2, 0 };

class Plugin : public ITMPlugin
{
public:
    static Plugin& Instance();

    // ITMPlugin
    virtual IPluginItem* GetItem(int index) override;
    virtual void DataRequired() override;
    virtual const wchar_t* GetInfo(PluginInfoIndex index) override;
    virtual const wchar_t* GetTooltipInfo() override;
    virtual void OnInitialize(ITrafficMonitor* pApp) override;
    virtual OptionReturn ShowOptionsDialog(void* hParent) override;
    virtual void* GetPluginIcon() override;

    // 供显示项使用（UI 线程缓存，由 DataRequired 更新）
    const std::wstring& CombinedValueText() const { return m_value_text; }
    const std::wstring& CombinedSampleText() const { return m_sample_text; }

    // 供显示项自绘（UI 线程缓存，由 RefreshDisplay 更新；构造时预填全部设备）
    const DeviceDisplay* Display() const { return m_display; }
    int DisplayCount() const { return m_display_count; }
    // 全部设备取消勾选：任务栏保留占位项，否则条目消失就无法右键打开设置
    bool HasPlaceholder() const { return m_placeholder; }

    void SetOptionChanged(bool v) { m_option_changed = v; }
    void ApplyOptionsNow() { RefreshDisplay(); } // 选项确定后立即刷新显示
    void LoadConfig();
    void SyncReaders();              // 勾选才查询：按勾选状态启停各设备 Reader

    // 选项对话框工作副本：勾选/排序只改副本，点"确定"才写回保存生效
    void DlgBegin();                 // 打开对话框时拷贝当前配置
    void DlgApply();                 // 确定：写回配置、保存并刷新显示
    bool DlgShowFor(int dev) const { return dev >= 0 && dev < kDeviceCount ? m_dlg_show[dev] : false; }
    void DlgToggle(int dev) { if (dev >= 0 && dev < kDeviceCount) m_dlg_show[dev] = !m_dlg_show[dev]; }
    int DlgOrderAt(int i) const { return m_dlg_order[i]; }
    void DlgMoveUp(int dev);
    void DlgMoveDown(int dev);
    void DlgRestoreDefault();
    void SaveConfig();

private:
    Plugin(); // 预填自绘显示数组（名称来自 PluginConfig）
    Plugin(const Plugin&) = delete;

    void RefreshDisplay();
    void BuildValueText(const BatterySnapshot& snap, std::wstring& out, const std::wstring& no_read);
    void BuildG7ValueText(const BatterySnapshot& snap, std::wstring& out, const std::wstring& no_read);
    void BuildKeyboardTooltip(const BatterySnapshot& snap, std::wstring& out, const std::wstring& label);
    void BuildMouseTooltip(const BatterySnapshot& snap, std::wstring& out, const std::wstring& label);
    void BuildPadTooltip(const std::wstring& label, const BatterySnapshot& snap, std::wstring& out);

private:
    static Plugin m_instance;

    PeripheralItem m_item;
    KeyboardReader m_key_reader;
    MouseReader m_mouse_reader;
    DualSenseReader m_ds_reader;
    G7Reader m_g7_reader;
    bool m_reader_started[kDeviceCount] = {};   // 各设备 Reader 是否已启动（勾选才查询）

    ITrafficMonitor* m_app = nullptr;
    bool m_show[kDeviceCount] = { true, true, true, true }; // 设备勾选状态
    int m_order[kDeviceCount] = {};                         // 显示顺序（设备索引，默认按字母序）
    bool m_dlg_show[kDeviceCount];                          // 选项对话框工作副本
    int m_dlg_order[kDeviceCount];
    bool m_option_changed = false;

    DeviceDisplay m_display[kDeviceCount]; // 自绘数据（仅勾选设备，按显示顺序）
    int m_display_count = 0;
    bool m_placeholder = false;

    // UI 线程缓存（由 DataRequired 更新，PeripheralItem::GetItemValueText 读取）
    std::wstring m_value_text = L"G87: --% | A5 Ultra: --% | G7 Pro: --% | DS5: --%";
    std::wstring m_sample_text = L"G87: 100%\u26A1 | A5 Ultra: 100%\u26A1 | G7 Pro: \u25AE\u25AE\u25AE\u25AE\u26A1 | DS5: 100%\u26A1";
    std::wstring m_tooltip;
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance();
#ifdef __cplusplus
}
#endif
