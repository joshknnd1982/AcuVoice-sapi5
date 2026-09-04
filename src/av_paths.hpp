#pragma once

#include <windows.h>
#include <string>

namespace AcuVoice {

// Where this module was loaded from, with a trailing separator. The 64-bit build sits
// one level down in x64\, so the runtime files -- engine\, the worker, the utility --
// are found by walking up when they are not beside the caller.
[[nodiscard]] inline std::wstring module_directory(HMODULE self)
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) {
        return {};
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return {};
    }
    *(slash + 1) = L'\0';
    return path;
}

[[nodiscard]] inline std::wstring this_module_directory()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&this_module_directory), &self)) {
        return {};
    }
    return module_directory(self);
}

[[nodiscard]] inline bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The install root: the directory holding engine\Lib\avcore.dll. Starts beside the
// caller and climbs, so it works from the install root, from x64\, and from a build
// tree where the binaries sit in build_x86\bin\Release.
[[nodiscard]] inline std::wstring install_root()
{
    std::wstring dir = this_module_directory();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        if (exists(dir + L"engine\\Lib\\avcore.dll")) {
            return dir;
        }
        // Drop the trailing separator, then the last component.
        std::wstring parent = dir.substr(0, dir.size() - 1);
        const size_t slash = parent.find_last_of(L'\\');
        if (slash == std::wstring::npos) {
            break;
        }
        dir = parent.substr(0, slash + 1);
    }
    return this_module_directory();
}

[[nodiscard]] inline std::wstring avcore_path()
{
    return install_root() + L"engine\\Lib\\avcore.dll";
}

// The writable side of the install: the dictionary the user's own words go into, the
// engine's scratch directory and the log. Program Files is read-only for the account a
// screen reader runs as, and the engine has to be able to write both.
[[nodiscard]] inline std::wstring program_data_root()
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH) == 0) {
        return install_root();
    }
    std::wstring dir = base;
    if (!dir.empty() && dir.back() != L'\\') {
        dir += L'\\';
    }
    dir += L"AcuVoice SAPI5\\";
    return dir;
}

}  // namespace AcuVoice
