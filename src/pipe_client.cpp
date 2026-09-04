#include "pipe_client.h"
#include <shlwapi.h>
#include <cstring>
#include "debug_log.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t ACUVOICE_SERVER_MUTEX[] = L"Global\\AcuVoiceTTSServerMutex";
constexpr wchar_t ACUVOICE_LAUNCH_MUTEX[] = L"Global\\AcuVoiceTTSLaunchMutex";

HMODULE GetCurrentModule() {
    HMODULE hModule = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetCurrentModule),
        &hModule
    );
    return hModule;
}
}

PipeClient::PipeClient()
    : pipe_(INVALID_HANDLE_VALUE)
    , serverProcess_(nullptr)
{
    InitializeCriticalSection(&cs_);

    // The worker is 32-bit, because avcore.dll is, so it lives in the x86 subfolder of a
    // 64-bit install and beside the caller in a build tree. Every plausible place is
    // tried rather than one: the install directory the installer recorded, then the
    // directory this module is in and the two ways down and up from it.
    std::vector<std::wstring> roots;

    for (const REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AcuVoice SAPI5",
                          0, KEY_READ | view, &key) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t path[MAX_PATH] = {};
        DWORD size = sizeof(path);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                             reinterpret_cast<LPBYTE>(path), &size) == ERROR_SUCCESS &&
            type == REG_SZ && path[0]) {
            roots.emplace_back(path);
        }
        RegCloseKey(key);
    }

    wchar_t dllPath[MAX_PATH] = {};
    if (HMODULE hModule = GetCurrentModule()) {
        GetModuleFileNameW(hModule, dllPath, MAX_PATH);
        PathRemoveFileSpecW(dllPath);
        roots.emplace_back(dllPath);
        PathRemoveFileSpecW(dllPath);
        roots.emplace_back(dllPath);
    }

    for (const std::wstring& root : roots) {
        std::wstring base = root;
        if (!base.empty() && base.back() != L'\\') {
            base += L'\\';
        }
        for (const wchar_t* leaf : { L"x86\\AcuVoiceServer.exe", L"AcuVoiceServer.exe" }) {
            const std::wstring candidate = base + leaf;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                serverPath_ = candidate;
                return;
            }
        }
    }

    // Nothing found. Keep the most likely path so the log names something useful.
    if (!roots.empty()) {
        serverPath_ = roots.front();
        if (!serverPath_.empty() && serverPath_.back() != L'\\') {
            serverPath_ += L'\\';
        }
        serverPath_ += L"x86\\AcuVoiceServer.exe";
    }
}

PipeClient::~PipeClient() {
    disconnect();
    DeleteCriticalSection(&cs_);
}

bool PipeClient::isServerRunning() {
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, ACUVOICE_SERVER_MUTEX);
    if (hMutex) {
        CloseHandle(hMutex);
        return true;
    }
    return false;
}

bool PipeClient::launchServer() {
    if (isServerRunning()) {
        return true;
    }

    if (GetFileAttributesW(serverPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DEBUG_LOG("pipe: worker not found at %ls", serverPath_.c_str());
        return false;
    }
    DEBUG_LOG("pipe: launching worker %ls", serverPath_.c_str());

    HANDLE launchMutex = CreateMutexW(nullptr, FALSE, ACUVOICE_LAUNCH_MUTEX);
    if (!launchMutex) {
        return false;
    }

    DWORD waitResult = WaitForSingleObject(launchMutex, 5000);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        CloseHandle(launchMutex);
        return false;
    }

    if (isServerRunning()) {
        ReleaseMutex(launchMutex);
        CloseHandle(launchMutex);
        return true;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    bool launched = CreateProcessW(serverPath_.c_str(), nullptr, nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (launched) {
        serverProcess_ = pi.hProcess;
        CloseHandle(pi.hThread);

        for (int i = 0; i < 20 && !isServerRunning(); ++i) {
            Sleep(100);
        }
    }

    ReleaseMutex(launchMutex);
    CloseHandle(launchMutex);
    return launched;
}

void PipeClient::disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool PipeClient::connect() {
    CriticalLock lock(&cs_);
    return ensureConnected();
}

// A worker built against a different wire format must not be talked to. Old builds
// answer PING with an empty payload, so a missing version is treated as a mismatch.
bool PipeClient::handshakeOk() {
    if (!sendCommand(CMD_PING)) {
        return false;
    }
    PipeResponse resp;
    std::vector<char> data;
    if (!readResponse(resp, data) || resp != RESP_PONG || data.size() < sizeof(uint32_t)) {
        return false;
    }
    return *reinterpret_cast<const uint32_t*>(data.data()) == ACUVOICE_PROTOCOL_VERSION;
}

// Ask the mismatched worker to exit, wait for it to release its mutex, then start ours.
bool PipeClient::replaceStaleServer() {
    sendCommand(CMD_SHUTDOWN);
    PipeResponse resp;
    std::vector<char> data;
    readResponse(resp, data);
    disconnect();

    for (int i = 0; i < 30 && isServerRunning(); ++i) {
        Sleep(100);
    }
    return launchServer();
}

bool PipeClient::ensureConnected() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        return true;
    }

    for (int attempt = 0; attempt < 5; ++attempt) {
        pipe_ = CreateFileW(
            ACUVOICE_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr
        );

        if (pipe_ != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);

            if (handshakeOk()) {
                return true;
            }
            // Left over from a previous version; replace it and connect to ours.
            DEBUG_LOG("pipe: worker speaks a different protocol version, replacing it");
            if (!replaceStaleServer()) {
                DEBUG_LOG("pipe: could not replace the stale worker");
                disconnect();
                return false;
            }
            continue;
        }

        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            if (attempt == 0) {
                launchServer();
            }
            Sleep(300);
        } else if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(ACUVOICE_PIPE_NAME, 1000);
        } else if (error == ERROR_ACCESS_DENIED) {
            // A worker exists but will not let us in. That means it is running at a
            // higher integrity level than this process -- something started it elevated
            // -- and Windows' no-write-up rule denies its pipe to us. We cannot kill it
            // from here and cannot start a second one, because it holds the mutex, so
            // all that is left is to say so plainly: this used to fail with no message
            // at all and every 64-bit voice simply went silent.
            DEBUG_LOG("pipe: the worker refused the connection (access denied). It is "
                      "running at a higher integrity level than this process, which "
                      "Windows will not let us talk to. End AcuVoiceServer.exe from an "
                      "elevated Task Manager, or reboot, and it will restart correctly.");
            break;
        } else {
            DEBUG_LOG("pipe: could not open %ls (error %lu)", ACUVOICE_PIPE_NAME, error);
            break;
        }
    }
    DEBUG_LOG("pipe: gave up connecting to the worker");
    return false;
}

bool PipeClient::writeAll(const void* data, uint32_t size) {
    auto* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe_, p, size, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        size -= written;
    }
    return true;
}

bool PipeClient::readAll(void* data, uint32_t size) {
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe_, p, size, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        size -= got;
    }
    return true;
}

bool PipeClient::sendCommand(PipeCommand cmd, const void* data, uint32_t size) {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    PipeMessageHeader header = { cmd, size };
    if (!writeAll(&header, sizeof(header))) {
        return false;
    }
    if (data && size > 0) {
        return writeAll(data, size);
    }
    return true;
}

bool PipeClient::readResponse(PipeResponse& resp, std::vector<char>& data) {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    PipeMessageHeader header;
    if (!readAll(&header, sizeof(header))) {
        return false;
    }

    resp = static_cast<PipeResponse>(header.type);
    data.clear();

    if (header.size > 0) {
        if (header.size > (16u << 20)) {
            return false;
        }
        data.resize(header.size);
        if (!readAll(data.data(), header.size)) {
            return false;
        }
    }
    return true;
}

bool PipeClient::engineInfo(EngineInfo& out) {
    CriticalLock lock(&cs_);
    memset(&out, 0, sizeof(out));

    if (!ensureConnected() || !sendCommand(CMD_ENGINE_INFO)) {
        disconnect();
        return false;
    }

    PipeResponse resp;
    std::vector<char> data;
    if (!readResponse(resp, data) || resp != RESP_ENGINE_INFO || data.size() < sizeof(EngineInfo)) {
        return false;
    }
    memcpy(&out, data.data(), sizeof(EngineInfo));
    return true;
}

bool PipeClient::speak(const char* text, uint32_t textLength,
                       double duration, double pitch, double gain, bool honourTags,
                       const int32_t pause[4],
                       PipeAudioCallback callback, void* user) {
    CriticalLock lock(&cs_);

    std::vector<char> payload(sizeof(SpeakCommand) + textLength);
    auto* cmd = reinterpret_cast<SpeakCommand*>(payload.data());
    cmd->duration = duration;
    cmd->pitch = pitch;
    cmd->gain = gain;
    cmd->honour_tags = honourTags ? 1u : 0u;
    for (int i = 0; i < 4; ++i) {
        cmd->pause[i] = pause ? pause[i] : -1;
    }
    cmd->text_length = textLength;
    if (textLength > 0) {
        memcpy(payload.data() + sizeof(SpeakCommand), text, textLength);
    }

    if (!ensureConnected() ||
        !sendCommand(CMD_SPEAK, payload.data(), static_cast<uint32_t>(payload.size()))) {
        disconnect();
        return false;
    }

    bool stopped = false;

    while (true) {
        PipeResponse resp;
        std::vector<char> data;

        if (!readResponse(resp, data)) {
            disconnect();
            return false;
        }

        if (resp == RESP_AUDIO_END) {
            break;
        }

        if (resp == RESP_ERROR) {
            return false;
        }

        if (resp == RESP_OK) {
            continue;
        }

        if (resp == RESP_AUDIO_DATA && data.size() >= sizeof(uint32_t) && !stopped) {
            uint32_t chunkSize = *reinterpret_cast<uint32_t*>(data.data());
            const char* audioData = data.data() + sizeof(uint32_t);

            if (callback && !callback(audioData, chunkSize, user)) {
                sendCommand(CMD_STOP);
                stopped = true;
            }
        }
    }

    return true;
}

void PipeClient::shutdownServer() {
    CriticalLock lock(&cs_);

    if (!isServerRunning()) {
        return;
    }

    if (ensureConnected()) {
        sendCommand(CMD_SHUTDOWN);
        PipeResponse resp;
        std::vector<char> data;
        readResponse(resp, data);
        disconnect();
    }

    for (int i = 0; i < 20 && isServerRunning(); ++i) {
        Sleep(100);
    }
}
