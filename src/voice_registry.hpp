#pragma once

#include <windows.h>
#include <sapi.h>
#include <string>

#include "registry.hpp"
#include "voice_attributes.hpp"

namespace AcuVoice {
namespace sapi {

// Every voice is written as its own static token rather than being left to the dynamic
// enumerator. Static tokens are what every SAPI5 client reads, Windows Narrator
// included, so this is the form with the widest reach -- and the enumerator is
// registered as well, for the hosts that use it.
inline constexpr const wchar_t* voices_path =
    L"Software\\Microsoft\\Speech\\Voices\\Tokens";

// Which registry view these land in is decided by the architecture of the dll doing the
// registering: the 32-bit build writes under WOW6432Node, where 32-bit SAPI looks, and
// the 64-bit build writes to the native view, where 64-bit hosts look. Both are
// installed and both register, so every host sees the full list.
inline void write_voice_tokens(HKEY root, const std::wstring& clsid_str)
{
    using namespace AcuVoice::registry;

    key tokens(root, voices_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);

    for (int i = 0; i < total_token_count(); ++i) {
        const voice_attributes v(i);
        const std::wstring name = v.get_name();

        key token(tokens, v.get_token_id(), KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
        token.set(name);
        token.set(L"CLSID", clsid_str);
        // SAPI looks a display name up under a value named for the LCID it is asking
        // about, falling back to the key's default value, which is set just above.
        token.set(L"409", name);

        key attrs(token, L"Attributes", KEY_SET_VALUE, true);
        attrs.set(L"Name", name);
        attrs.set(L"Gender", v.get_gender());
        attrs.set(L"Age", v.get_age());
        attrs.set(L"Language", v.get_language());
        attrs.set(L"Vendor", v.get_vendor());
        attrs.set(L"Description", v.get_summary());
        // Read back by SetObjectToken, so the exact voice is recovered without parsing
        // a display name apart.
        attrs.set(L"AcuVoice", v.get_voice_id());
    }
}

inline void remove_voice_tokens(HKEY root) noexcept
{
    using namespace AcuVoice::registry;

    try {
        key tokens(root, voices_path, KEY_ALL_ACCESS);
        for (int i = 0; i < total_token_count(); ++i) {
            const std::wstring id = voice_attributes(i).get_token_id();
            try {
                key token(tokens, id, KEY_ALL_ACCESS);
                token.delete_subkey(L"Attributes");
            }
            catch (...) {
            }
            try {
                tokens.delete_subkey(id);
            }
            catch (...) {
            }
        }
    }
    catch (...) {
    }
}

}  // namespace sapi
}  // namespace AcuVoice
