#include "PluginConfig.h"
#include <cstdlib>
#include <cwchar>

namespace {

constexpr const wchar_t* kSection = L"Advanced";

int Clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        --e;
    return s.substr(b, e - b);
}

std::wstring ReadStrValue(const std::wstring& path, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[128]{};
    DWORD n = GetPrivateProfileStringW(kSection, key, def, buf, 128, path.c_str());
    if (n == 0 && def[0] != L'\0')
        return def;
    return buf;
}

int ReadIntValue(const std::wstring& path, const wchar_t* key, int def, int lo, int hi)
{
    wchar_t buf[32]{};
    GetPrivateProfileStringW(kSection, key, L"", buf, 32, path.c_str());
    if (buf[0] == L'\0')
        return def;
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    if (end == buf)
        return def;
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (*end != L'\0')
        return def;
    return Clamp(static_cast<int>(v), lo, hi);
}

COLORREF ReadColorValue(const std::wstring& path, const wchar_t* key, COLORREF def)
{
    std::wstring s = ReadStrValue(path, key, L"");
    COLORREF c = 0;
    return PluginConfig::ParseColor(s, c) ? c : def;
}

void WriteInt(const std::wstring& path, const wchar_t* key, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(kSection, key, buf, path.c_str());
}

void WriteColor(const std::wstring& path, const wchar_t* key, COLORREF c)
{
    WritePrivateProfileStringW(kSection, key, PluginConfig::FormatColor(c).c_str(), path.c_str());
}

} // namespace

PluginConfig& PluginConfig::Instance()
{
    static PluginConfig instance;
    return instance;
}

void PluginConfig::SetIniPath(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_path = path;
}

void PluginConfig::Load()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty())
        return;

    // 先回到默认值，再逐项覆盖；缺失或非法项自动使用默认值
    m_refresh = RefreshConfig{};
    m_display = DisplayConfig{};
    m_text = TextConfig{};

    const std::wstring& p = m_path;
    m_refresh.kb_wake_ms = ReadIntValue(p, L"kb_wake_ms", m_refresh.kb_wake_ms, 1000, 86400000);
    m_refresh.kb_active_ms = ReadIntValue(p, L"kb_active_ms", m_refresh.kb_active_ms, 1000, 86400000);
    m_refresh.kb_refresh_ms = ReadIntValue(p, L"kb_refresh_ms", m_refresh.kb_refresh_ms, 1000, 86400000);
    m_refresh.kb_cooldown_ms = ReadIntValue(p, L"kb_cooldown_ms", m_refresh.kb_cooldown_ms, 1000, 86400000);
    m_refresh.kb_reconnect_ms = ReadIntValue(p, L"kb_reconnect_ms", m_refresh.kb_reconnect_ms, 1000, 86400000);
    m_refresh.ms_wake_ms = ReadIntValue(p, L"ms_wake_ms", m_refresh.ms_wake_ms, 1000, 86400000);
    m_refresh.ms_active_ms = ReadIntValue(p, L"ms_active_ms", m_refresh.ms_active_ms, 1000, 86400000);
    m_refresh.ms_refresh_ms = ReadIntValue(p, L"ms_refresh_ms", m_refresh.ms_refresh_ms, 1000, 86400000);
    m_refresh.ms_cooldown_ms = ReadIntValue(p, L"ms_cooldown_ms", m_refresh.ms_cooldown_ms, 1000, 86400000);
    m_refresh.g7_offline_ms = ReadIntValue(p, L"g7_offline_ms", m_refresh.g7_offline_ms, 1000, 86400000);
    m_refresh.g7_reset_ms = ReadIntValue(p, L"g7_reset_ms", m_refresh.g7_reset_ms, 1000, 86400000);
    m_refresh.g7_reconnect_ms = ReadIntValue(p, L"g7_reconnect_ms", m_refresh.g7_reconnect_ms, 1000, 86400000);
    m_refresh.g7_read_timeout_ms = ReadIntValue(p, L"g7_read_timeout_ms", m_refresh.g7_read_timeout_ms, 100, 60000);
    m_refresh.ds_pnp_ms = ReadIntValue(p, L"ds_pnp_ms", m_refresh.ds_pnp_ms, 1000, 600000);
    m_refresh.ds_read_ms = ReadIntValue(p, L"ds_read_ms", m_refresh.ds_read_ms, 1000, 86400000);

    m_display.level_high = ReadIntValue(p, L"level_high", m_display.level_high, 1, 100);
    m_display.level_low = ReadIntValue(p, L"level_low", m_display.level_low, 0, 99);
    m_display.ring_scale_x10 = ReadIntValue(p, L"ring_scale_x10", m_display.ring_scale_x10, 8, 40);
    m_display.sep_w = ReadIntValue(p, L"sep_w", m_display.sep_w, 0, 60);
    m_display.pad_lr = ReadIntValue(p, L"pad_lr", m_display.pad_lr, 0, 20);
    m_display.num_gap = ReadIntValue(p, L"num_gap", m_display.num_gap, 0, 20);
    m_display.show_sep = ReadIntValue(p, L"show_sep", m_display.show_sep ? 1 : 0, 0, 1) != 0;
    m_display.show_pct_sign = ReadIntValue(p, L"show_pct_sign", m_display.show_pct_sign ? 1 : 0, 0, 1) != 0;

    m_display.green_dark = ReadColorValue(p, L"color_green_dark", m_display.green_dark);
    m_display.green_light = ReadColorValue(p, L"color_green_light", m_display.green_light);
    m_display.yellow_dark = ReadColorValue(p, L"color_yellow_dark", m_display.yellow_dark);
    m_display.yellow_light = ReadColorValue(p, L"color_yellow_light", m_display.yellow_light);
    m_display.red_dark = ReadColorValue(p, L"color_red_dark", m_display.red_dark);
    m_display.red_light = ReadColorValue(p, L"color_red_light", m_display.red_light);

    for (int i = 0; i < 4; ++i)
    {
        wchar_t key[16];
        swprintf(key, 16, L"name%d", i);
        std::wstring v = Trim(ReadStrValue(p, key, m_text.name[i].c_str()));
        if (!v.empty())
            m_text.name[i] = v;
    }
    {
        std::wstring v = Trim(ReadStrValue(p, L"no_read", m_display.no_read.c_str()));
        if (!v.empty())
            m_display.no_read = v;
    }

    SanitizeLocked();
}

void PluginConfig::Save()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_path.empty())
        return;

    SanitizeLocked();

    const std::wstring& p = m_path;
    WriteInt(p, L"kb_wake_ms", m_refresh.kb_wake_ms);
    WriteInt(p, L"kb_active_ms", m_refresh.kb_active_ms);
    WriteInt(p, L"kb_refresh_ms", m_refresh.kb_refresh_ms);
    WriteInt(p, L"kb_cooldown_ms", m_refresh.kb_cooldown_ms);
    WriteInt(p, L"kb_reconnect_ms", m_refresh.kb_reconnect_ms);
    WriteInt(p, L"ms_wake_ms", m_refresh.ms_wake_ms);
    WriteInt(p, L"ms_active_ms", m_refresh.ms_active_ms);
    WriteInt(p, L"ms_refresh_ms", m_refresh.ms_refresh_ms);
    WriteInt(p, L"ms_cooldown_ms", m_refresh.ms_cooldown_ms);
    WriteInt(p, L"g7_offline_ms", m_refresh.g7_offline_ms);
    WriteInt(p, L"g7_reset_ms", m_refresh.g7_reset_ms);
    WriteInt(p, L"g7_reconnect_ms", m_refresh.g7_reconnect_ms);
    WriteInt(p, L"g7_read_timeout_ms", m_refresh.g7_read_timeout_ms);
    WriteInt(p, L"ds_pnp_ms", m_refresh.ds_pnp_ms);
    WriteInt(p, L"ds_read_ms", m_refresh.ds_read_ms);

    WriteInt(p, L"level_high", m_display.level_high);
    WriteInt(p, L"level_low", m_display.level_low);
    WriteInt(p, L"ring_scale_x10", m_display.ring_scale_x10);
    WriteInt(p, L"sep_w", m_display.sep_w);
    WriteInt(p, L"pad_lr", m_display.pad_lr);
    WriteInt(p, L"num_gap", m_display.num_gap);
    WriteInt(p, L"show_sep", m_display.show_sep ? 1 : 0);
    WriteInt(p, L"show_pct_sign", m_display.show_pct_sign ? 1 : 0);

    WriteColor(p, L"color_green_dark", m_display.green_dark);
    WriteColor(p, L"color_green_light", m_display.green_light);
    WriteColor(p, L"color_yellow_dark", m_display.yellow_dark);
    WriteColor(p, L"color_yellow_light", m_display.yellow_light);
    WriteColor(p, L"color_red_dark", m_display.red_dark);
    WriteColor(p, L"color_red_light", m_display.red_light);

    for (int i = 0; i < 4; ++i)
    {
        wchar_t key[16];
        swprintf(key, 16, L"name%d", i);
        WritePrivateProfileStringW(kSection, key, m_text.name[i].c_str(), p.c_str());
    }
    WritePrivateProfileStringW(kSection, L"no_read", m_display.no_read.c_str(), p.c_str());
}

RefreshConfig PluginConfig::Refresh() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_refresh;
}

DisplayConfig PluginConfig::Display() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_display;
}

TextConfig PluginConfig::Text() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_text;
}

std::wstring PluginConfig::Name(int dev) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (dev < 0 || dev >= 4)
        return std::wstring();
    return m_text.name[dev];
}

void PluginConfig::SetAll(const RefreshConfig& refresh, const DisplayConfig& display, const TextConfig& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refresh = refresh;
    m_display = display;
    m_text = text;
    SanitizeLocked();
}

void PluginConfig::SanitizeLocked()
{
    m_refresh.kb_wake_ms = Clamp(m_refresh.kb_wake_ms, 1000, 86400000);
    m_refresh.kb_active_ms = Clamp(m_refresh.kb_active_ms, 1000, 86400000);
    m_refresh.kb_refresh_ms = Clamp(m_refresh.kb_refresh_ms, 1000, 86400000);
    m_refresh.kb_cooldown_ms = Clamp(m_refresh.kb_cooldown_ms, 1000, 86400000);
    m_refresh.kb_reconnect_ms = Clamp(m_refresh.kb_reconnect_ms, 1000, 86400000);
    m_refresh.ms_wake_ms = Clamp(m_refresh.ms_wake_ms, 1000, 86400000);
    m_refresh.ms_active_ms = Clamp(m_refresh.ms_active_ms, 1000, 86400000);
    m_refresh.ms_refresh_ms = Clamp(m_refresh.ms_refresh_ms, 1000, 86400000);
    m_refresh.ms_cooldown_ms = Clamp(m_refresh.ms_cooldown_ms, 1000, 86400000);
    m_refresh.g7_offline_ms = Clamp(m_refresh.g7_offline_ms, 1000, 86400000);
    m_refresh.g7_reset_ms = Clamp(m_refresh.g7_reset_ms, 1000, 86400000);
    m_refresh.g7_reconnect_ms = Clamp(m_refresh.g7_reconnect_ms, 1000, 86400000);
    m_refresh.g7_read_timeout_ms = Clamp(m_refresh.g7_read_timeout_ms, 100, 60000);
    m_refresh.ds_pnp_ms = Clamp(m_refresh.ds_pnp_ms, 1000, 600000);
    m_refresh.ds_read_ms = Clamp(m_refresh.ds_read_ms, 1000, 86400000);

    m_display.level_high = Clamp(m_display.level_high, 1, 100);
    m_display.level_low = Clamp(m_display.level_low, 0, 99);
    if (m_display.level_low >= m_display.level_high)
    {
        m_display.level_high = 60;
        m_display.level_low = 20;
    }
    m_display.ring_scale_x10 = Clamp(m_display.ring_scale_x10, 8, 40);
    m_display.sep_w = Clamp(m_display.sep_w, 0, 60);
    m_display.pad_lr = Clamp(m_display.pad_lr, 0, 20);
    m_display.num_gap = Clamp(m_display.num_gap, 0, 20);

    m_display.no_read = Trim(m_display.no_read);
    if (m_display.no_read.empty())
        m_display.no_read = L"--";
    else if (m_display.no_read.size() > 15)
        m_display.no_read.resize(15);

    for (int i = 0; i < 4; ++i)
    {
        m_text.name[i] = Trim(m_text.name[i]);
        if (m_text.name[i].empty())
            m_text.name[i] = TextConfig{}.name[i];
        else if (m_text.name[i].size() > 31)
            m_text.name[i].resize(31);
    }
}

bool PluginConfig::ParseColor(const std::wstring& text, COLORREF& out)
{
    std::wstring s = Trim(text);
    if (!s.empty() && s[0] == L'#')
        s.erase(0, 1);
    if (s.size() != 6)
        return false;

    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (end != s.c_str() + 6)
        return false;
    if (v > 0xFFFFFFul)
        return false;

    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

std::wstring PluginConfig::FormatColor(COLORREF color)
{
    wchar_t buf[8];
    const unsigned r = color & 0xFF;
    const unsigned g = (color >> 8) & 0xFF;
    const unsigned b = (color >> 16) & 0xFF;
    swprintf(buf, 8, L"%02X%02X%02X", r, g, b);
    return buf;
}
