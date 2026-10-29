#include "DualSenseDevice.h"
#include "HidApi.h"
#include <setupapi.h>
#include <vector>

// ---- DualSense 特征 ----
static constexpr WORD   SONY_VID       = 0x054C;
static constexpr WORD   DS_PID         = 0x0CE6;  // DualSense
static constexpr WORD   EDGE_PID       = 0x0DF2;  // DualSense Edge
static constexpr BYTE   INPUT_RID      = 0x01;    // Input Report 0x01（USB 状态报告）
// 电池字节在报告 byte[53]（0-based）：
//   byte[0]=ReportID，USBGetStateData[52]（PowerPercent 低4位 + PowerState 高4位）
//   对应报告偏移 = 1 + 52 = 53（DS5Dongle 固件 utils.h USBGetStateData 布局确认）
static constexpr int    POWER_BYTE     = 53;
static constexpr BYTE   POWERSTATE_CHARGING = 0x01;
static constexpr BYTE   POWERSTATE_COMPLETE  = 0x02;

#ifndef HIDP_STATUS_SUCCESS
#define HIDP_STATUS_SUCCESS ((NTSTATUS)0x00110000L)
#endif

// 0.8.0 起 DS5Dongle 在 Sony VID/PID 下额外暴露唤醒键盘与 Consumer Control 两个 HID 集合，
// 且 SetupDi 枚举顺序中 Consumer 可能排在手柄之前。只有 UsagePage 0x01 / Usage 0x05
// 的游戏手柄集合承载 Input Report 0x01（电量所在报告），必须按 Usage 精确匹配。
static bool IsGamepadCollection(HANDLE h)
{
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(h, &preparsed) || !preparsed)
        return false;
    HIDP_CAPS caps{};
    NTSTATUS status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    return status == HIDP_STATUS_SUCCESS &&
           caps.UsagePage == 0x01 && caps.Usage == 0x05 &&
           caps.InputReportByteLength >= POWER_BYTE + 1;
}

DualSenseDevice::DualSenseDevice() = default;

DualSenseDevice::~DualSenseDevice()
{
    Close();
}

bool DualSenseDevice::Open(const std::wstring& device_path)
{
    Close();
    if (!device_path.empty())
        return OpenPath(device_path);

    // 枚举 HID 设备接口，匹配 Sony VID + DualSense/Edge PID
    GUID hid_guid = GUID_DEVINTERFACE_HID;
    HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, index, &if_data); ++index)
    {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, nullptr, 0, &required, nullptr);
        if (required == 0)
            continue;
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail, required, nullptr, nullptr))
            continue;
        if (OpenPath(detail->DevicePath))
        {
            found = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}

bool DualSenseDevice::IsPresent()
{
    // 枚举 HID 设备接口，打开后按 VID/PID + 手柄集合 Usage 匹配。
    // 仅做 CreateFile + HidD_GetAttributes + HidP_GetCaps + CloseHandle，不读写数据，无副作用。
    GUID hid_guid = GUID_DEVINTERFACE_HID;
    HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    SP_DEVICE_INTERFACE_DATA if_data;
    if_data.cbSize = sizeof(if_data);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, index, &if_data); ++index)
    {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, nullptr, 0, &required, nullptr);
        if (required == 0)
            continue;
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail, required, nullptr, nullptr))
            continue;

        HANDLE h = CreateFileW(detail->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) &&
            attr.VendorID == SONY_VID &&
            (attr.ProductID == DS_PID || attr.ProductID == EDGE_PID) &&
            IsGamepadCollection(h))
        {
            found = true;
            CloseHandle(h);
            break;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}

bool DualSenseDevice::OpenPath(const std::wstring& path)
{
    // FILE_FLAG_OVERLAPPED 必须开：QueryBattery 依赖 OVERLAPPED 读做超时/取消，
    // 同步句柄上 ReadFile 会阻塞到有数据，超时与 Stop 取消逻辑全部失效
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HIDD_ATTRIBUTES attr{};
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != SONY_VID ||
        (attr.ProductID != DS_PID && attr.ProductID != EDGE_PID) ||
        !IsGamepadCollection(h))
    {
        CloseHandle(h);
        return false;
    }

    WCHAR name_buf[128]{};
    if (HidD_GetProductString(h, name_buf, sizeof(name_buf)))
        m_product_name = name_buf;

    m_hDevice = h;
    m_device_path = path;
    return true;
}

void DualSenseDevice::Close()
{
    if (m_hDevice)
    {
        CloseHandle(m_hDevice);
        m_hDevice = nullptr;
    }
    // 保留 m_device_path / m_product_name：路径作为下次 Open() 的缓存
    // （设备未重插时直接按路径重开，免全量枚举），产品名供快照展示
}

bool DualSenseDevice::ParseInputReport(const BYTE* buf, int len, BatteryReading& out)
{
    // 报告 0x01：64 字节（含 Report ID）
    if (!buf || len < POWER_BYTE + 1 || buf[0] != INPUT_RID)
        return false;

    // 空壳报告防护（对照 Linux hid-playstation.c 研究）：
    // 连接/断开瞬间会收到报告体为空的 0x01（下标 10 起全 0），
    // 电量下标 53 上是填充 0x00，照读会误报"0 档"。
    bool body_empty = true;
    for (int i = 10; i < len; ++i)
    {
        if (buf[i] != 0) { body_empty = false; break; }
    }
    if (body_empty)
        return false;

    BYTE power_byte = buf[POWER_BYTE];
    int  level      = power_byte & 0x0F;       // 0x00-0x0A → 0~10 档
    BYTE state      = (power_byte >> 4) & 0x0F;

    if (level > 10)
        return false; // 无效读数
    // 异常状态 (0xA 电压异常 / 0xB 温度异常 / 0xF 充电错误) 下 PowerPercent 无效
    // (DS5Dongle 固件注释与 Linux hid-playstation.c 一致):保持上次读数
    if (state == 0xA || state == 0xB || state == 0xF)
        return false;

    out.valid = true;
    // 与 Linux 驱动 dualsense_parse_report 一致：
    // battery = min(level * 10 + 5, 100)；0→5%, 5→55%, 9→95%, 10→100%
    if (level >= 10)
        out.percent = 100;
    else
        out.percent = level * 10 + 5;

    if (state == POWERSTATE_CHARGING)
        out.charging = true;                       // 0x01 充电中
    else if (state == POWERSTATE_COMPLETE)
    {
        out.charging = true;                       // 0x02 已充满 → 100%
        out.percent = 100;
    }
    else
        out.charging = false;                      // 0x00 放电等
    return true;
}

bool DualSenseDevice::QueryBattery(BatteryReading& out, const std::atomic<bool>& cancel, DWORD timeout_ms)
{
    if (!m_hDevice)
        return false;

    // 被动读取 Input Report 0x01：DS5Dongle 只在收到手柄蓝牙帧时才转发 USB 报告，
    // 手柄待机/休眠/断开后不再上报。同步 ReadFile 会永久阻塞，
    // 导致 WorkerLoop 卡死、PnP 断开检测停摆。故用 OVERLAPPED + 分片等待：
    // 总耐心 timeout_ms 不变，cancel 置位（插件 Stop）时 100ms 内放弃，
    // 超时/取消视为"暂无新数据"返回 false，线程继续跑 PnP 检测。
    BYTE buf[65] = { 0 };
    DWORD read = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;

    BOOL ok = ReadFile(m_hDevice, buf, sizeof(buf), &read, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        ULONGLONG deadline = GetTickCount64() + timeout_ms;
        bool completed = false;
        while (true)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                break;
            DWORD slice = 100;
            if (deadline - now < slice)
                slice = static_cast<DWORD>(deadline - now);
            if (WaitForSingleObject(ov.hEvent, slice) == WAIT_OBJECT_0)
            {
                completed = true;
                break;
            }
            if (cancel.load())
                break;
        }
        if (completed)
        {
            ok = GetOverlappedResult(m_hDevice, &ov, &read, FALSE);
        }
        else
        {
            // CancelIo 后必须等取消完成再返回，否则内核完成取消时
            // 仍会写已销毁栈帧上的 OVERLAPPED
            CancelIo(m_hDevice);
            DWORD canceled = 0;
            GetOverlappedResult(m_hDevice, &ov, &canceled, TRUE);
            CloseHandle(ov.hEvent);
            return false; // 超时/取消：手柄待机/休眠/断开，无新上报
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok || read < POWER_BYTE + 1)
        return false;
    return ParseInputReport(buf, (int)read, out);
}
