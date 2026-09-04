#pragma once

#include <string>
#include "voices.hpp"

namespace AcuVoice {
namespace sapi {

// One published SAPI voice. Index 0..PRESET_COUNT-1 are the presets built on the
// engine's single recorded voice; the last index is the Custom Voice, whose parameters
// are re-read from HKCU for every utterance so a change in the configuration utility
// lands on the next thing a running screen reader says.
[[nodiscard]] inline int total_token_count() noexcept
{
    return PRESET_COUNT + 1;
}

[[nodiscard]] inline int custom_token_index() noexcept
{
    return PRESET_COUNT;
}

class voice_attributes {
public:
    explicit voice_attributes(int token_index = 0) noexcept : index_(token_index)
    {
        if (index_ < 0 || index_ >= total_token_count()) {
            index_ = 0;
        }
    }

    [[nodiscard]] int index() const noexcept { return index_; }
    [[nodiscard]] bool is_custom() const noexcept { return index_ == custom_token_index(); }

    [[nodiscard]] const voice_preset& preset() const noexcept
    {
        return PRESETS[is_custom() ? 0 : index_];
    }

    [[nodiscard]] std::wstring get_name() const
    {
        return is_custom() ? std::wstring(CUSTOM_NAME) : std::wstring(preset().name);
    }

    [[nodiscard]] std::wstring get_summary() const
    {
        return is_custom()
            ? std::wstring(L"Every parameter comes from the AcuVoice configuration utility.")
            : std::wstring(preset().summary);
    }

    // Registry key name. Stable across releases, so an upgrade does not reset the voice
    // a screen reader is set to.
    [[nodiscard]] std::wstring get_token_id() const
    {
        return std::wstring(L"AcuVoice_") + (is_custom() ? CUSTOM_ID : preset().id);
    }

    // Written into the token so SetObjectToken recovers the exact voice without having
    // to parse a localized display name back apart.
    [[nodiscard]] std::wstring get_voice_id() const
    {
        return is_custom() ? std::wstring(CUSTOM_ID) : std::wstring(preset().id);
    }

    [[nodiscard]] std::wstring get_age() const { return VOICE_AGE; }
    [[nodiscard]] std::wstring get_gender() const { return VOICE_GENDER; }
    [[nodiscard]] std::wstring get_language() const { return VOICE_LANGUAGE; }
    [[nodiscard]] std::wstring get_vendor() const { return VOICE_VENDOR; }

private:
    int index_ = 0;
};

}  // namespace sapi
}  // namespace AcuVoice
