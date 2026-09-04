// AcuVoiceServer.exe -- the 32-bit worker a 64-bit SAPI host speaks through.
//
// avcore.dll is 32-bit and there is no 64-bit build of it, so a 64-bit host cannot load
// the engine at all. This process does: it holds one copy of avcore, synthesizes on
// behalf of every 64-bit client, and hands back finished 16-bit PCM.
//
// It stays running between utterances. Starting it costs a process launch and a pipe
// connect, which is around a hundred milliseconds -- far too much to pay on a keystroke
// -- so the first utterance a 64-bit host speaks starts it and every one after that
// finds it already there.

#include <windows.h>
#include <process.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pipe_protocol.h"
#include "av_engine.h"
#include "av_dsp.h"
#include "av_paths.hpp"
#include "debug_log.h"

namespace {

constexpr wchar_t SERVER_MUTEX[] = L"Global\\AcuVoiceTTSServerMutex";

// Long enough that a screen reader's quiet spell never costs a restart, short enough
// that a machine nobody is speaking on does not keep the process forever.
constexpr DWORD IDLE_TIMEOUT_MS = 30 * 60 * 1000;

// Audio is handed back in pieces this size. It is what the cancel latency comes down to
// on a 64-bit host: the client stops consuming between chunks, so a smaller chunk is a
// faster stop and more pipe traffic. 8 KB is a sixteenth of a second at 16 kHz.
constexpr uint32_t AUDIO_CHUNK = 8192;

AcuVoice::engine g_engine;
HANDLE g_shutdown_event = nullptr;
volatile LONG g_active_clients = 0;
volatile LONG g_last_activity = 0;

// avcore's pausation is process-wide, so two clients wanting different pacing have to
// take turns at it. Synthesis itself is fast enough that serializing costs nothing
// worth measuring: a second of speech takes about a millisecond to make.
CRITICAL_SECTION g_engine_lock;

bool write_all(HANDLE pipe, const void* data, uint32_t size)
{
    auto* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, p, size, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        size -= written;
    }
    return true;
}

bool read_all(HANDLE pipe, void* data, uint32_t size)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, p, size, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        size -= got;
    }
    return true;
}

bool send_response(HANDLE pipe, PipeResponse type, const void* data = nullptr, uint32_t size = 0)
{
    PipeMessageHeader header = { static_cast<uint32_t>(type), size };
    if (!write_all(pipe, &header, sizeof(header))) {
        return false;
    }
    return (data && size) ? write_all(pipe, data, size) : true;
}

// Has the client asked us to stop? Checked between audio chunks with a peek rather than
// a read: the client sends CMD_STOP while this thread is still writing, and a blocking
// read on the same handle would deadlock against its own reader.
bool stop_requested(HANDLE pipe)
{
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        return true;   // the client is gone
    }
    if (available < sizeof(PipeMessageHeader)) {
        return false;
    }
    PipeMessageHeader header;
    if (!read_all(pipe, &header, sizeof(header))) {
        return true;
    }
    if (header.size > 0) {
        std::vector<char> discard(header.size);
        read_all(pipe, discard.data(), header.size);
    }
    return header.type == CMD_STOP;
}

void handle_speak(HANDLE pipe, std::vector<char>& payload)
{
    if (payload.size() < sizeof(SpeakCommand)) {
        send_response(pipe, RESP_ERROR);
        return;
    }
    SpeakCommand cmd;
    memcpy(&cmd, payload.data(), sizeof(cmd));

    if (payload.size() < sizeof(SpeakCommand) + cmd.text_length) {
        send_response(pipe, RESP_ERROR);
        return;
    }
    std::string text(payload.data() + sizeof(SpeakCommand), cmd.text_length);

    std::vector<unsigned char> ulaw;
    {
        EnterCriticalSection(&g_engine_lock);
        for (int i = 0; i < AcuVoice::PAUSE_COUNT; ++i) {
            if (cmd.pause[i] >= 0) {
                g_engine.set_pause(i + 1, cmd.pause[i]);
            }
        }
        const bool ok = g_engine.speak_all(text.c_str(), cmd.honour_tags != 0, ulaw);
        LeaveCriticalSection(&g_engine_lock);
        if (!ok && ulaw.empty()) {
            DEBUG_LOG("worker: synthesis produced nothing (status %d)", g_engine.last_error());
            send_response(pipe, RESP_AUDIO_END);
            return;
        }
    }

    std::vector<int16_t> pcm;
    AcuVoice::dsp::ulaw_to_pcm16(ulaw.data(), ulaw.size(), pcm);

    AcuVoice::dsp::params p;
    p.duration = cmd.duration;
    p.pitch = cmd.pitch;
    p.gain = cmd.gain;

    std::vector<int16_t> out;
    AcuVoice::dsp::render(pcm, p, out);

    const char* bytes = reinterpret_cast<const char*>(out.data());
    uint32_t total = static_cast<uint32_t>(out.size() * sizeof(int16_t));
    uint32_t offset = 0;

    while (offset < total) {
        if (stop_requested(pipe)) {
            break;
        }
        const uint32_t n = (total - offset < AUDIO_CHUNK) ? (total - offset) : AUDIO_CHUNK;
        PipeMessageHeader header = { RESP_AUDIO_DATA, n + static_cast<uint32_t>(sizeof(uint32_t)) };
        if (!write_all(pipe, &header, sizeof(header)) ||
            !write_all(pipe, &n, sizeof(n)) ||
            !write_all(pipe, bytes + offset, n)) {
            return;
        }
        offset += n;
    }

    send_response(pipe, RESP_AUDIO_END);
}

unsigned __stdcall client_thread(void* arg)
{
    HANDLE pipe = static_cast<HANDLE>(arg);
    InterlockedIncrement(&g_active_clients);
    DEBUG_LOG("worker: client connected (%ld active)", g_active_clients);

    while (true) {
        PipeMessageHeader header;
        if (!read_all(pipe, &header, sizeof(header))) {
            break;
        }
        InterlockedExchange(&g_last_activity, static_cast<LONG>(GetTickCount()));

        std::vector<char> payload;
        if (header.size > 0) {
            if (header.size > (64u << 20)) {
                break;
            }
            payload.resize(header.size);
            if (!read_all(pipe, payload.data(), header.size)) {
                break;
            }
        }

        switch (header.type) {
            case CMD_PING: {
                const uint32_t version = ACUVOICE_PROTOCOL_VERSION;
                send_response(pipe, RESP_PONG, &version, sizeof(version));
                break;
            }
            case CMD_ENGINE_INFO: {
                EngineInfo info = {};
                info.sample_rate = AcuVoice::OUTPUT_SAMPLE_RATE;
                info.engine_rate = static_cast<uint32_t>(g_engine.sample_rate());
                info.bits = static_cast<uint32_t>(g_engine.bits());
                info.format = static_cast<uint32_t>(g_engine.format());
                info.channels_allowed = static_cast<uint32_t>(g_engine.channels_allowed());
                const std::string v = g_engine.version();
                strncpy_s(info.version, v.c_str(), _TRUNCATE);
                send_response(pipe, RESP_ENGINE_INFO, &info, sizeof(info));
                break;
            }
            case CMD_SPEAK:
                handle_speak(pipe, payload);
                break;
            case CMD_STOP:
                // Only meaningful while an utterance is in flight, where handle_speak
                // reads it; arriving here it just means there was nothing to stop.
                send_response(pipe, RESP_OK);
                break;
            case CMD_SHUTDOWN:
                DEBUG_LOG("worker: shutdown requested by a client");
                send_response(pipe, RESP_OK);
                FlushFileBuffers(pipe);
                SetEvent(g_shutdown_event);
                goto done;
            default:
                send_response(pipe, RESP_ERROR);
                break;
        }
    }

done:
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    InterlockedExchange(&g_last_activity, static_cast<LONG>(GetTickCount()));
    InterlockedDecrement(&g_active_clients);
    return 0;
}

// The pipe has to be reachable from a lower integrity level than this process is running
// at, and by default it is not.
//
// A named pipe inherits the creator's integrity label. If anything ever starts this
// worker elevated -- the installer running its self-test, a user launching the
// configuration utility as administrator, a host started from an elevated shell -- the
// pipe is labelled High, Windows' no-write-up rule denies every ordinary medium-integrity
// SAPI host access to it, and the mutex below then tells those hosts that a worker is
// already running so they never start one they could actually talk to. The result is
// every 64-bit voice going silent until the elevated worker times out, with an
// ERROR_ACCESS_DENIED that nothing reports.
//
// So the label is set explicitly to Low, which any caller can write to, and the DACL is
// narrowed to compensate: SYSTEM and Administrators in full, and read/write for the user
// this worker belongs to. Without the explicit DACL the descriptor would come out with a
// null one, which is everyone.
[[nodiscard]] std::wstring current_user_sid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::wstring result;
    if (needed) {
        std::vector<BYTE> buffer(needed);
        if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(
                    reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
                result = text;
                LocalFree(text);
            }
        }
    }
    CloseHandle(token);
    return result;
}

PSECURITY_DESCRIPTOR g_pipe_sd = nullptr;

SECURITY_ATTRIBUTES* pipe_security()
{
    static SECURITY_ATTRIBUTES sa = {};
    static bool built = false;
    if (built) {
        return g_pipe_sd ? &sa : nullptr;
    }
    built = true;

    const std::wstring sid = current_user_sid();
    std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
    // IU -- anyone logged on interactively -- only as a fallback for the case where this
    // process cannot read its own token, which should not happen.
    sddl += L"(A;;GRGW;;;" + (sid.empty() ? std::wstring(L"IU") : sid) + L")";
    sddl += L"S:(ML;;NW;;;LW)";

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &g_pipe_sd, nullptr)) {
        DEBUG_LOG("worker: could not build the pipe security descriptor (%lu); falling "
                  "back to the default, which a lower integrity level cannot open",
                  GetLastError());
        g_pipe_sd = nullptr;
        return nullptr;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = g_pipe_sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

HANDLE create_pipe_instance()
{
    return CreateNamedPipeW(
        ACUVOICE_PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        64 * 1024, 64 * 1024, 0, pipe_security());
}

}  // namespace

int wmain()
{
    // One worker per machine. A second copy would be a second 154 MB sound bank and a
    // second engine holding the same channel counter.
    HANDLE instance = CreateMutexW(nullptr, TRUE, SERVER_MUTEX);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    InitializeCriticalSection(&g_engine_lock);
    g_shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    InterlockedExchange(&g_last_activity, static_cast<LONG>(GetTickCount()));

    const std::wstring core = AcuVoice::avcore_path();
    if (!g_engine.load(core)) {
        DEBUG_LOG("worker: could not load %ls -- exiting", core.c_str());
        return 1;
    }
    DEBUG_LOG("worker: ready, avcore %s, engine %d Hz %d-bit format %d",
              g_engine.version().c_str(), g_engine.sample_rate(),
              g_engine.bits(), g_engine.format());

    // The listening instance is created before the previous one is handed to a client,
    // so the pipe name never stops existing. A gap there is not an error the client can
    // distinguish from "no worker running", and it would answer by launching a second
    // one and waiting a hundred milliseconds for it -- on every keystroke.
    HANDLE listening = create_pipe_instance();
    if (listening == INVALID_HANDLE_VALUE) {
        DEBUG_LOG("worker: CreateNamedPipe failed (%lu)", GetLastError());
        return 1;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (WaitForSingleObject(g_shutdown_event, 0) != WAIT_OBJECT_0) {
        ResetEvent(ov.hEvent);
        BOOL connected = ConnectNamedPipe(listening, &ov);
        DWORD err = GetLastError();

        if (!connected && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, g_shutdown_event };
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
            if (w == WAIT_OBJECT_0 + 1) {
                break;
            }
            if (w == WAIT_TIMEOUT) {
                const LONG idle = static_cast<LONG>(GetTickCount()) -
                                  InterlockedCompareExchange(&g_last_activity, 0, 0);
                if (g_active_clients == 0 && idle > static_cast<LONG>(IDLE_TIMEOUT_MS)) {
                    DEBUG_LOG("worker: idle for %ld ms with no clients, exiting", idle);
                    CancelIo(listening);
                    break;
                }
                CancelIo(listening);
                continue;
            }
            DWORD transferred = 0;
            if (!GetOverlappedResult(listening, &ov, &transferred, FALSE)) {
                continue;
            }
        } else if (!connected && err != ERROR_PIPE_CONNECTED) {
            Sleep(50);
            continue;
        }

        HANDLE served = listening;
        listening = create_pipe_instance();
        if (listening == INVALID_HANDLE_VALUE) {
            DEBUG_LOG("worker: could not create the next pipe instance (%lu)", GetLastError());
            listening = served;
            continue;
        }

        // Each client gets its own thread so a long utterance never blocks the accept
        // loop, and so a second SAPI voice in the same host does not queue behind the
        // first any longer than the engine lock makes it.
        unsigned id = 0;
        HANDLE thread = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, client_thread, served, 0, &id));
        if (thread) {
            CloseHandle(thread);
        } else {
            CloseHandle(served);
        }
    }

    CloseHandle(ov.hEvent);
    CloseHandle(listening);
    DEBUG_LOG("worker: exiting");
    DeleteCriticalSection(&g_engine_lock);
    ReleaseMutex(instance);
    CloseHandle(instance);
    return 0;
}
