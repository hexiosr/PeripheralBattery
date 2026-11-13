#include "DualSenseReader.h"
#include "PluginConfig.h"
#include <windows.h>

DualSenseReader::~DualSenseReader()
{
    Stop();
}

void DualSenseReader::Start()
{
    if (m_thread.joinable())
        return;
    m_stop = false;
    m_thread = std::thread(&DualSenseReader::WorkerLoop, this);
    // 不 detach：Stop() join 确认线程退出后才允许再次 Start，
    // 避免旧线程因 m_stop 被复位而永不退出、与新线程并发跑两个查询循环
}

void DualSenseReader::Stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join(); // 线程内的长等待均可被 m_stop 打断，join 通常几百毫秒内返回
}

BatterySnapshot DualSenseReader::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

void DualSenseReader::UpdateSnapshotLocked(const BatteryReading& reading, bool online, ULONGLONG now_ms)
{
    m_snapshot.online = online;
    if (reading.valid)
    {
        m_snapshot.valid = true;
        m_snapshot.percent = reading.percent;
        m_snapshot.charging = reading.charging;
        m_snapshot.last_update_ms = now_ms;
    }
}

void DualSenseReader::WorkerLoop()
{
    DualSenseDevice device;

    ULONGLONG last_pnp_ms = 0;
    ULONGLONG last_read_ms = 0;
    bool prev_present = false; // 上次 PnP 检查的设备状态（检测"刚刚出现"）

    while (!m_stop)
    {
        // 每轮取一次配置快照：玩家在选项里改完间隔后，最迟一个等待周期生效
        const RefreshConfig cfg = PluginConfig::Instance().Refresh();
        ULONGLONG now = GetTickCount64();

        if (last_pnp_ms == 0 || now - last_pnp_ms >= (ULONGLONG)cfg.ds_pnp_ms)
        {
            last_pnp_ms = now;

            if (!DualSenseDevice::IsPresent())
            {
                // 设备不在系统（手柄蓝牙断开 → DS5Dongle 隐藏 USB 设备）→ 显示 --
                std::lock_guard<std::mutex> lock(m_mutex);
                m_snapshot.valid = false;
                m_snapshot.online = false;
                prev_present = false;
                device.Close();
                if (!m_stop)
                    Sleep(200);
                continue;
            }

            // 设备在系统。设备刚刚出现（PnP 到达）→ 立即读一次电量，不等 60s 间隔
            bool just_appeared = !prev_present;
            if (just_appeared)
                last_read_ms = 0;

            if (last_read_ms == 0 || now - last_read_ms >= (ULONGLONG)cfg.ds_read_ms)
            {
                if (!device.IsOpen())
                {
                    // 缓存路径直开；失败或尚无缓存 → 全量搜索（接收器重插路径变化时兜底）
                    bool opened = !device.DevicePath().empty() && device.Open(device.DevicePath());
                    if (!opened)
                        opened = device.Open(L"");
                    if (!opened)
                    {
                        // 打开失败（设备重枚举瞬间/驱动未就绪）→ 视为尚未真正可用，下次 2s 再试
                        prev_present = false;
                        if (!m_stop)
                            Sleep(200);
                        continue;
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_snapshot.has_device)
                    {
                        m_snapshot.has_device = true;
                        m_snapshot.product = device.ProductName();
                    }
                }

                BatteryReading r;
                bool got = device.QueryBattery(r, m_stop);
                if (got)
                {
                    last_read_ms = now; // 仅读取成功才进入 60s 节流；失败保持快速重试
                    std::lock_guard<std::mutex> lock(m_mutex);
                    UpdateSnapshotLocked(r, true, GetTickCount64());
                }
                // 查询失败/超时（待机无上报）→ 保持最后读数，连接状态已由 IsPresent 保证
                device.Close(); // 即查即关
                prev_present = true;
            }
        }

        if (!m_stop)
            Sleep(200);
    }

    device.Close();
}
