#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include "pipe_protocol.h"

class CriticalLock {
public:
    explicit CriticalLock(CRITICAL_SECTION* cs) noexcept : cs_(cs) {
        EnterCriticalSection(cs_);
    }

    ~CriticalLock() {
        LeaveCriticalSection(cs_);
    }

    CriticalLock(const CriticalLock&) = delete;
    CriticalLock& operator=(const CriticalLock&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

using PipeAudioCallback = bool(*)(const char* data, uint32_t size, void* user);

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool connect();

    // What the worker will send back, and what avcore reports about itself. Zeroed if
    // the worker could not be reached.
    bool engineInfo(EngineInfo& out);

    // `text` is already Windows-1252 and already prepared; the worker only synthesizes.
    // Audio arrives as 16-bit PCM at OUTPUT_SAMPLE_RATE with the prosody already applied.
    bool speak(const char* text, uint32_t textLength,
               double duration, double pitch, double gain, bool honourTags,
               const int32_t pause[4],
               PipeAudioCallback callback, void* user);

    void shutdownServer();

private:
    bool ensureConnected();
    bool handshakeOk();
    bool replaceStaleServer();
    void disconnect();
    bool isServerRunning();
    bool launchServer();
    bool sendCommand(PipeCommand cmd, const void* data = nullptr, uint32_t size = 0);
    bool readResponse(PipeResponse& resp, std::vector<char>& data);
    bool writeAll(const void* data, uint32_t size);
    bool readAll(void* data, uint32_t size);

    HANDLE pipe_;
    HANDLE serverProcess_;
    std::wstring serverPath_;
    CRITICAL_SECTION cs_;
};
