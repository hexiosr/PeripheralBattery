#include "KeyboardReader.h"
#include "PluginConfig.h"
#include <windows.h>

KeyboardReader::~KeyboardReader()
{
    Stop();
}

void KeyboardReader::Start()
{
    if (m_thread.joinable())
        return;
    m_stop = false;
    m_thread = std::thread(&KeyboardReader::WorkerLoop, this);
    // 不 detach：Stop() join 确认线程退出后才允许再次 Start，
    // 避免旧线程因 m_stop 被复位而永不退出、与新线程并发跑两个查询循环
}

void KeyboardReader::Stop()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join(); // 线程内的长等待均可被 m_stop 打断，join 通常几百毫秒内返回
}

BatterySnapshot KeyboardReader::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

void KeyboardReader::UpdateSnapshotLocked(const BatteryReading& reading, bool online, ULONGLONG now_ms)
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

// 检测是否有任意键盘键被按下（Windows API，不对 HID 设备做任何写入，
// 不影响键盘/接收器休眠；用于感知"键盘被唤醒/正在使用"）
static bool AnyKeyPressed()
{
    // 跳过鼠标键（0x01..0x07），只扫键盘键
    for (int vk = 0x08; vk <= 0xFE; ++vk)
    {
        if (GetAsyncKeyState(vk) & 0x8000)
            return true;
    }
    return false;
}

void KeyboardReader::WorkerLoop()
{
    KeyboardDevice device;

    ULONGLONG last_query_ms = 0;
    ULONGLONG last_key_activity_ms = 0; // 最近一次检测到按键的时刻

    while (!m_stop)
    {
        // 每轮取一次配置快照：玩家在选项里改完间隔后，最迟一个等待周期生效
        const RefreshConfig cfg = PluginConfig::Instance().Refresh();
        ULONGLONG now = GetTickCount64();

        // ---- 按键状态采样（GetAsyncKeyState，纯 Windows API，零 HID 写入）----
        // 打字时每次按键间隔很短（< 60s），不会被误判为唤醒；
        // 只有闲置满 60 秒后的【首次】按键（键盘休眠后唤醒）才触发查询
        bool key_down = AnyKeyPressed();
        bool wake = false;
        if (key_down)
        {
            if (last_key_activity_ms != 0 && now - last_key_activity_ms >= (ULONGLONG)cfg.kb_wake_ms)
                wake = true;
            last_key_activity_ms = now;
        }

        // ---- 查询触发条件（无任何周期轮询，键盘可自然休眠）----
        bool need_query = false;
        if (last_query_ms == 0)
        {
            need_query = true;                    // ① 初次启动：连接后查一次
        }
        else if (wake)
        {
            need_query = true;                    // ② 键盘唤醒瞬间：查一次
        }
        else if (now - last_key_activity_ms < (ULONGLONG)cfg.kb_active_ms &&
                 now - last_query_ms >= (ULONGLONG)cfg.kb_refresh_ms)
        {
            need_query = true;                    // ③ 使用中每 5 秒刷新一次
        }

        // 防抖：距上次查询不足 2 秒则跳过
        if (need_query && now - last_query_ms < (ULONGLONG)cfg.kb_cooldown_ms)
            need_query = false;

        if (need_query)
        {
            // ---- 查询前确保句柄打开（句柄只在查询瞬间持有）----
            if (!device.IsOpen())
            {
                bool ok = device.Open(device.DevicePath());
                // 旧路径失效（接收器被拔插/重置导致路径变化）→ 清空路径全量搜索
                if (!ok && !device.DevicePath().empty())
                    ok = device.Open(L"");
                if (!ok)
                {
                    // 打开失败（接收器未连接）：不做离线判定，保持最后读数，
                    // 延迟后重试（可被 Stop 打断）
                    SleepInterruptible(m_stop, (DWORD)cfg.kb_reconnect_ms);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_snapshot.has_device = true;
                    m_snapshot.product = device.ProductName();
                }
            }

            // ---- 临时开会话 + 查询（不开会话 dongle 对查询静音）----
            device.SendSessionOpen();
            if (!m_stop)
                Sleep(100);
            device.SendBatteryQuery();

            ULONGLONG deadline = GetTickCount64() + 600;
            while (!m_stop && GetTickCount64() < deadline)
            {
                BYTE frame[256]{}; // 与 KeyboardDevice::OpenPath 的输入报告长度上限（256B）匹配
                if (!device.ReadInput(frame, 100))
                    continue;
                BatteryReading r = KeyboardDevice::ParseQueryResponse(frame, sizeof(frame));
                if (r.valid)
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        UpdateSnapshotLocked(r, true, GetTickCount64());
                    }
                    break;
                }
                if (KeyboardDevice::ParsePushFrame(frame, sizeof(frame), r))
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        UpdateSnapshotLocked(r, true, GetTickCount64());
                    }
                    break;
                }
            }

            last_query_ms = GetTickCount64();
            // 查询成功与否都不做离线判定：
            // 查询成功 → 更新读数；
            // 无响应（键盘休眠 dongle 静音 / 接收器拔出）→ 保持最后读数

            // ---- 释放会话：关闭句柄，下次查询再重开 ----
            // dongle 的配置会话一次只服务一个客户端；句柄关闭后会话释放，
            // 官方驱动/网页驱动可随时正常连接设置，无需关掉本插件。
            device.Close();

            if (!m_stop)
                Sleep(200);
        }
        else
        {
            // 空闲 tick（~80ms）：不持有句柄、不发送任何数据，
            // 只做按键检测——键盘/接收器完全自然休眠
            if (!m_stop)
                Sleep(80);
        }
    }

    // 线程退出：释放设备句柄
    device.Close();
}
