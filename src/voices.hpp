#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "av_engine.h"
#include "av_dsp.h"

namespace AcuVoice {

// AcuVoice ships one recorded voice and one language. The sound bank in Ulaw08Sb is a
// single male American English speaker -- "Roger" -- stored as real recorded speech in
// four hashed partitions (Hashfon1..4.cmp are buckets of one bank, not four voices), and
// AcuEng.dll, the original SAPI4 engine, published exactly one mode for it:
// "AcuVoice, Roger", American English, Adult.
//
// So the voice list here is Roger plus a set of presets built from him: the engine's
// three prosody controls are continuous, and a screen reader user picking "AcuVoice
// Roger Deep" from a voice list is doing the same thing as moving two sliders, with less
// work. Every preset says in its description which of Roger's parameters it moves, and
// the README says plainly that they are all the same recorded voice.

struct voice_preset {
    const wchar_t* id;        // registry key suffix, stable across releases
    const wchar_t* name;      // what the voice list shows
    const wchar_t* summary;   // one line for the configuration utility
    int speed;                // AcuVoice \spd= scale, 85..350
    int pitch;                // AcuVoice \pit= scale, 45..91
    int volume;               // AcuVoice \vol= scale, 0..65535
    int pause_scale;          // percent applied to the engine's four pause lengths
};

inline constexpr voice_preset PRESETS[] = {
    { L"roger",     L"AcuVoice Roger",
      L"The engine's own voice, exactly as AcuVoice shipped it.",
      SPEED_DEFAULT, PITCH_DEFAULT, VOLUME_DEFAULT, 100 },

    { L"deep",      L"AcuVoice Roger Deep",
      L"Roger a fourth lower and a little slower.",
      160, 50, VOLUME_DEFAULT, 110 },

    { L"bright",    L"AcuVoice Roger Bright",
      L"Roger a fourth higher, at his usual pace.",
      180, 78, VOLUME_DEFAULT, 100 },

    { L"brisk",     L"AcuVoice Roger Brisk",
      L"Roger at 260 words a minute, for reading rather than listening.",
      260, 65, VOLUME_DEFAULT, 70 },

    { L"measured",  L"AcuVoice Roger Measured",
      L"Roger at 120 words a minute with longer pauses.",
      120, 60, VOLUME_DEFAULT, 140 },

    { L"announcer", L"AcuVoice Roger Announcer",
      L"Roger slightly lower and louder, with broadcast pacing.",
      165, 57, 46000, 120 },

    { L"clipped",   L"AcuVoice Roger Clipped",
      L"Roger fast and with the pauses taken out, for skimming.",
      300, 66, VOLUME_DEFAULT, 25 },
};

inline constexpr int PRESET_COUNT = static_cast<int>(sizeof(PRESETS) / sizeof(PRESETS[0]));

// The token whose parameters come from the configuration utility rather than the table.
inline constexpr const wchar_t* CUSTOM_ID = L"custom";
inline constexpr const wchar_t* CUSTOM_NAME = L"AcuVoice Custom Voice";

inline constexpr const wchar_t* VOICE_VENDOR = L"AcuVoice / Fonix";
inline constexpr const wchar_t* VOICE_LANGUAGE = L"409";     // en-US
inline constexpr const wchar_t* VOICE_GENDER = L"Male";
inline constexpr const wchar_t* VOICE_AGE = L"Adult";

[[nodiscard]] inline int preset_by_id(const std::wstring& id) noexcept
{
    for (int i = 0; i < PRESET_COUNT; ++i) {
        if (id == PRESETS[i].id) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------------
// The engine's own scales, and what they mean once something has to act on them.
// ---------------------------------------------------------------------------------

// avcore's own words-per-minute curve, lifted from its tag handler: 175 is the pace the
// recordings were made at and needs no work; 85 doubles the duration, 350 halves it.
[[nodiscard]] inline double speed_to_duration(int wpm) noexcept
{
    const double w = std::clamp(wpm, SPEED_MIN, SPEED_MAX);
    if (w > SPEED_DEFAULT) {
        return (525.0 - w) / 350.0;
    }
    if (w < SPEED_DEFAULT) {
        return 2.0 - (w - 85.0) / 90.0;
    }
    return 1.0;
}

// avcore's own pitch curve: 45..91 is a semitone offset of -6..+6 around 63.
[[nodiscard]] inline double pitch_to_semitones(int pit) noexcept
{
    const double p = std::clamp(pit, PITCH_MIN, PITCH_MAX);
    if (p > PITCH_DEFAULT) {
        return (p - 7.0) * 3.0 / 14.0 - 12.0;
    }
    if (p < PITCH_DEFAULT) {
        return (p - 27.0) / 3.0 - 12.0;
    }
    return 0.0;
}

[[nodiscard]] inline double pitch_to_factor(int pit) noexcept
{
    return std::pow(2.0, pitch_to_semitones(pit) / 12.0);
}

// avcore reduces 0..65535 to a percentage change of -50..+50 around 32767. Taken as a
// straight amplitude ratio that is the same shape and needs no table.
[[nodiscard]] inline double volume_to_gain(int vol) noexcept
{
    return std::clamp(vol, VOLUME_MIN, VOLUME_MAX) / static_cast<double>(VOLUME_DEFAULT);
}

// Percent <-> engine scale, for the configuration utility. 0 % is always the parameter's
// minimum and 100 % its maximum, so a slider reads the same way for every control.
[[nodiscard]] inline int percent_to_range(int percent, int lo, int hi) noexcept
{
    const int p = std::clamp(percent, 0, 100);
    return lo + static_cast<int>(std::lround((hi - lo) * (p / 100.0)));
}

[[nodiscard]] inline int range_to_percent(int value, int lo, int hi) noexcept
{
    if (hi <= lo) {
        return 0;
    }
    const int v = std::clamp(value, lo, hi);
    return static_cast<int>(std::lround((v - lo) * 100.0 / (hi - lo)));
}

}  // namespace AcuVoice
