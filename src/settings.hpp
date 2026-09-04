#pragma once

#include <windows.h>
#include <algorithm>
#include <string>

#include "av_engine.h"
#include "voices.hpp"

// Everything the configuration utility can change, kept in HKCU so it needs no
// elevation and follows the user between machines with their profile.
//
//   HKCU\Software\AcuVoice SAPI5
//       Logging          DWORD   1 = write the diagnostic log (default 1)
//   HKCU\Software\AcuVoice SAPI5\CustomVoice
//       Speed            DWORD   85..350   AcuVoice words per minute
//       Pitch            DWORD   45..91    AcuVoice pitch scale
//       Volume           DWORD   0..65535  AcuVoice volume scale
//       Pause1..Pause4   DWORD   milliseconds
//       HonourTags       DWORD   1 = let \spd=..\ and friends in the text reach the engine
//       SpeakPunctuation DWORD   1 = name punctuation instead of pausing on it
//   HKCU\Software\AcuVoice SAPI5\Global
//       RatePercent      DWORD   50..200, multiplies whatever the host asks for
//       VolumePercent    DWORD   0..200,  multiplies whatever the host asks for
//       Pause1..Pause4   DWORD   milliseconds, applied to every voice
//       HonourTags       DWORD   as above, for every voice
//       SpeakPunctuation DWORD   as above, for every voice

namespace AcuVoice {
namespace settings {

inline constexpr const wchar_t* ROOT_KEY = L"Software\\AcuVoice SAPI5";
inline constexpr const wchar_t* CUSTOM_KEY = L"Software\\AcuVoice SAPI5\\CustomVoice";
inline constexpr const wchar_t* GLOBAL_KEY = L"Software\\AcuVoice SAPI5\\Global";

inline DWORD read_dword(const wchar_t* subkey, const wchar_t* name, DWORD fallback)
{
    HKEY key = nullptr;
    DWORD value = fallback;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD size = sizeof(value);
        DWORD type = 0;
        DWORD read = 0;
        if (RegQueryValueExW(key, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&read), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            value = read;
        }
        RegCloseKey(key);
    }
    return value;
}

inline bool write_dword(const wchar_t* subkey, const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG rc = RegSetValueExW(key, name, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// The parameters of one voice, on the engine's own scales.
struct voice_params {
    int speed = SPEED_DEFAULT;
    int pitch = PITCH_DEFAULT;
    int volume = VOLUME_DEFAULT;
    int pause[PAUSE_COUNT] = { PAUSE_DEFAULT[0], PAUSE_DEFAULT[1],
                               PAUSE_DEFAULT[2], PAUSE_DEFAULT[3] };
    bool honour_tags = false;
    bool speak_punctuation = false;

    void clamp()
    {
        speed = std::clamp(speed, SPEED_MIN, SPEED_MAX);
        pitch = std::clamp(pitch, PITCH_MIN, PITCH_MAX);
        volume = std::clamp(volume, VOLUME_MIN, VOLUME_MAX);
        for (int i = 0; i < PAUSE_COUNT; ++i) {
            pause[i] = std::clamp(pause[i], 0, PAUSE_MAX[i]);
        }
    }
};

// Settings that apply on top of every voice, custom or preset.
struct global_settings {
    int rate_percent = 100;      // 50..200
    int volume_percent = 100;    // 0..200
    bool override_pauses = false;
    int pause[PAUSE_COUNT] = { PAUSE_DEFAULT[0], PAUSE_DEFAULT[1],
                               PAUSE_DEFAULT[2], PAUSE_DEFAULT[3] };
    bool honour_tags = false;
    bool speak_punctuation = false;

    [[nodiscard]] bool is_default() const noexcept
    {
        return rate_percent == 100 && volume_percent == 100 && !override_pauses &&
               !honour_tags && !speak_punctuation;
    }
};

inline voice_params load_custom()
{
    voice_params p;
    p.speed = static_cast<int>(read_dword(CUSTOM_KEY, L"Speed", SPEED_DEFAULT));
    p.pitch = static_cast<int>(read_dword(CUSTOM_KEY, L"Pitch", PITCH_DEFAULT));
    p.volume = static_cast<int>(read_dword(CUSTOM_KEY, L"Volume", VOLUME_DEFAULT));
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"Pause%d", i + 1);
        p.pause[i] = static_cast<int>(read_dword(CUSTOM_KEY, name, PAUSE_DEFAULT[i]));
    }
    p.honour_tags = read_dword(CUSTOM_KEY, L"HonourTags", 0) != 0;
    p.speak_punctuation = read_dword(CUSTOM_KEY, L"SpeakPunctuation", 0) != 0;
    p.clamp();
    return p;
}

inline bool save_custom(const voice_params& in)
{
    voice_params p = in;
    p.clamp();
    bool ok = true;
    ok &= write_dword(CUSTOM_KEY, L"Speed", static_cast<DWORD>(p.speed));
    ok &= write_dword(CUSTOM_KEY, L"Pitch", static_cast<DWORD>(p.pitch));
    ok &= write_dword(CUSTOM_KEY, L"Volume", static_cast<DWORD>(p.volume));
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"Pause%d", i + 1);
        ok &= write_dword(CUSTOM_KEY, name, static_cast<DWORD>(p.pause[i]));
    }
    ok &= write_dword(CUSTOM_KEY, L"HonourTags", p.honour_tags ? 1 : 0);
    ok &= write_dword(CUSTOM_KEY, L"SpeakPunctuation", p.speak_punctuation ? 1 : 0);
    return ok;
}

inline global_settings load_global()
{
    global_settings g;
    g.rate_percent = std::clamp(static_cast<int>(read_dword(GLOBAL_KEY, L"RatePercent", 100)), 25, 400);
    g.volume_percent = std::clamp(static_cast<int>(read_dword(GLOBAL_KEY, L"VolumePercent", 100)), 0, 200);
    g.override_pauses = read_dword(GLOBAL_KEY, L"OverridePauses", 0) != 0;
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"Pause%d", i + 1);
        g.pause[i] = std::clamp(
            static_cast<int>(read_dword(GLOBAL_KEY, name, PAUSE_DEFAULT[i])), 0, PAUSE_MAX[i]);
    }
    g.honour_tags = read_dword(GLOBAL_KEY, L"HonourTags", 0) != 0;
    g.speak_punctuation = read_dword(GLOBAL_KEY, L"SpeakPunctuation", 0) != 0;
    return g;
}

inline bool save_global(const global_settings& g)
{
    bool ok = true;
    ok &= write_dword(GLOBAL_KEY, L"RatePercent", static_cast<DWORD>(g.rate_percent));
    ok &= write_dword(GLOBAL_KEY, L"VolumePercent", static_cast<DWORD>(g.volume_percent));
    ok &= write_dword(GLOBAL_KEY, L"OverridePauses", g.override_pauses ? 1 : 0);
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"Pause%d", i + 1);
        ok &= write_dword(GLOBAL_KEY, name, static_cast<DWORD>(g.pause[i]));
    }
    ok &= write_dword(GLOBAL_KEY, L"HonourTags", g.honour_tags ? 1 : 0);
    ok &= write_dword(GLOBAL_KEY, L"SpeakPunctuation", g.speak_punctuation ? 1 : 0);
    return ok;
}

// The parameters a preset speaks with. Its pause_scale is a percentage of the engine's
// own defaults, which is what makes "Clipped" clipped.
inline voice_params preset_params(int index)
{
    voice_params p;
    if (index < 0 || index >= PRESET_COUNT) {
        index = 0;
    }
    const voice_preset& v = PRESETS[index];
    p.speed = v.speed;
    p.pitch = v.pitch;
    p.volume = v.volume;
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        p.pause[i] = std::clamp(PAUSE_DEFAULT[i] * v.pause_scale / 100, 0, PAUSE_MAX[i]);
    }
    return p;
}

}  // namespace settings
}  // namespace AcuVoice
