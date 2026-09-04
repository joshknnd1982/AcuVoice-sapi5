// reg_test.exe -- register the SAPI engine for the current user only.
//
// DllRegisterServer writes to HKLM, which needs elevation, so nothing about the SAPI
// side could be tested without an install. This does the same registration under
// HKEY_CURRENT_USER, where SAPI also looks: the CLSID goes to HKCU\Software\Classes
// (which the WOW64 redirector splits by bitness on its own, so the 32-bit build lands
// where 32-bit SAPI looks and the 64-bit build where 64-bit SAPI looks), and the voice
// tokens go to HKCU\Software\Microsoft\Speech\Voices\Tokens, which is not redirected and
// so is shared by both.
//
//   reg_test register [path\to\AcuVoiceSAPI.dll]
//   reg_test unregister

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <string>

#include "registry.hpp"
#include "voice_registry.hpp"
#include "voice_attributes.hpp"
#include "av_paths.hpp"

namespace {

// Must match the uuids on the two classes in the engine dll.
const wchar_t* ENGINE_CLSID = L"{6D1A8C47-2F30-4B6E-9A1C-7E5B0D3A4F21}";
const wchar_t* ENUM_CLSID   = L"{D25E46E7-3C9A-403B-BCA1-DBF910D0F99A}";

bool set_value(HKEY root, const std::wstring& key, const wchar_t* name,
               const std::wstring& value)
{
    HKEY handle = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &handle, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG rc = RegSetValueExW(handle, name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

void delete_tree(HKEY root, const std::wstring& key)
{
    RegDeleteTreeW(root, key.c_str());
}

bool register_class(const wchar_t* clsid, const std::wstring& dll, const wchar_t* name)
{
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
    bool ok = set_value(HKEY_CURRENT_USER, base, nullptr, name);
    ok &= set_value(HKEY_CURRENT_USER, base + L"\\InprocServer32", nullptr, dll);
    ok &= set_value(HKEY_CURRENT_USER, base + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring mode = (argc > 1) ? argv[1] : L"register";
    wprintf(L"reg_test, %d-bit\n", static_cast<int>(sizeof(void*) * 8));

    if (mode == L"unregister") {
        AcuVoice::sapi::remove_voice_tokens(HKEY_CURRENT_USER);
        delete_tree(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\CLSID\\") + ENGINE_CLSID);
        delete_tree(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\CLSID\\") + ENUM_CLSID);
        wprintf(L"unregistered from HKEY_CURRENT_USER\n");
        return 0;
    }

    std::wstring dll;
    if (argc > 2) {
        dll = argv[2];
    } else {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir = self;
        dir.resize(dir.find_last_of(L'\\') + 1);
        dll = dir + L"AcuVoiceSAPI.dll";
    }
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"  !! %s does not exist\n", dll.c_str());
        return 1;
    }

    if (!register_class(ENGINE_CLSID, dll, L"AcuVoice SAPI5 TTS Engine") ||
        !register_class(ENUM_CLSID, dll, L"AcuVoice SAPI5 Voice Enumerator")) {
        wprintf(L"  !! could not write the CLSID entries\n");
        return 1;
    }

    try {
        AcuVoice::sapi::write_voice_tokens(HKEY_CURRENT_USER, ENGINE_CLSID);
    }
    catch (...) {
        wprintf(L"  !! could not write the voice tokens\n");
        return 1;
    }

    wprintf(L"  engine dll   %s\n", dll.c_str());
    wprintf(L"  %d voices registered under HKEY_CURRENT_USER\n",
            AcuVoice::sapi::total_token_count());
    return 0;
}
