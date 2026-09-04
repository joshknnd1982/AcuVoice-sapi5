#include "av_engine.h"
#include "debug_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace AcuVoice {

namespace {

// avcore builds its synthesis status block on the stack in under 0x504 bytes, with a
// 0x29A magic at +0x3F4 that every entry point checks. This is comfortably larger and
// always zeroed, so nothing here depends on having got its exact size right.
constexpr size_t SSB_BYTES = 8192;

// One segment is up to about twelve seconds of 8 kHz mu-law. A megabyte is two minutes
// of it, which no single segment has ever come close to.
constexpr size_t SEGMENT_BUFFER = 1024 * 1024;

// Guard against a malformed utterance looping forever inside isSeg_Available.
constexpr int MAX_SEGMENTS = 100000;

template<typename T>
void bind(HMODULE dll, const char* name, T& fn)
{
    fn = reinterpret_cast<T>(GetProcAddress(dll, name));
}

}  // namespace

engine::~engine()
{
    // The dll is deliberately left loaded. avcore keeps a machine-wide channel counter
    // in a shared section and allocates its sound-bank indexes at first use; unloading
    // and reloading it inside a long-lived host buys nothing and costs a reload of a
    // 154 MB bank.
}

bool engine::load(const std::wstring& avcore_path)
{
    if (loaded_) {
        return true;
    }

    dll_ = LoadLibraryW(avcore_path.c_str());
    if (!dll_) {
        DEBUG_LOG("avcore.dll failed to load from %ls (error %lu)",
                  avcore_path.c_str(), GetLastError());
        return false;
    }

    bind(dll_, "_get_av_version@4", get_av_version_);
    bind(dll_, "_initialize_SSB@12", initialize_SSB_);
    bind(dll_, "_close_SSB@4", close_SSB_);
    bind(dll_, "_set_tag_flag@8", set_tag_flag_);
    bind(dll_, "_isSeg_Available@4", isSeg_Available_);
    bind(dll_, "_synth_to_buffer@12", synth_to_buffer_);
    bind(dll_, "_get_snd_fmt@0", get_snd_fmt_);
    bind(dll_, "_get_snd_bps@0", get_snd_bps_);
    bind(dll_, "_get_snd_sps@0", get_snd_sps_);
    bind(dll_, "_get_channels_allowed@0", get_channels_allowed_);
    bind(dll_, "_get_pausation@4", get_pausation_);
    bind(dll_, "_set_pausation@8", set_pausation_);
    bind(dll_, "_pitch_dequeue@8", pitch_dequeue_);
    bind(dll_, "_speed_dequeue@8", speed_dequeue_);
    bind(dll_, "_volume_dequeue@8", volume_dequeue_);
    bind(dll_, "_Rpit_dequeue@8", Rpit_dequeue_);
    bind(dll_, "_Rspd_dequeue@8", Rspd_dequeue_);
    bind(dll_, "_get_bookmark@8", get_bookmark_);

    if (!initialize_SSB_ || !close_SSB_ || !isSeg_Available_ || !synth_to_buffer_ ||
        !set_tag_flag_) {
        DEBUG_LOG("avcore.dll loaded but is missing the synthesis entry points -- "
                  "this is not an AcuVoice 3.x core");
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    loaded_ = true;
    DEBUG_LOG("avcore.dll %s loaded from %ls: format %d, %d-bit, %d Hz, %d channels allowed",
              version().c_str(), avcore_path.c_str(), format(), bits(), sample_rate(),
              channels_allowed());
    return true;
}

std::string engine::version() const
{
    if (!get_av_version_) {
        return "unknown";
    }
    char buf[256] = {};
    get_av_version_(buf);
    buf[sizeof(buf) - 1] = '\0';
    return buf[0] ? buf : "unknown";
}

int engine::sample_rate() const
{
    return get_snd_sps_ ? get_snd_sps_() : ENGINE_SAMPLE_RATE;
}

int engine::bits() const
{
    return get_snd_bps_ ? get_snd_bps_() : 8;
}

int engine::format() const
{
    return get_snd_fmt_ ? get_snd_fmt_() : 7;
}

int engine::channels_allowed() const
{
    return get_channels_allowed_ ? get_channels_allowed_() : 0;
}

int engine::get_pause(int which) const
{
    if (!get_pausation_ || which < 1 || which > PAUSE_COUNT) {
        return 0;
    }
    return get_pausation_(which);
}

void engine::set_pause(int which, int milliseconds) const
{
    if (!set_pausation_ || which < 1 || which > PAUSE_COUNT) {
        return;
    }
    // The value comes first and the slot second, the opposite way round from
    // get_pausation. Called the other way about it silently does nothing: the slot
    // number lands in the value and 1234 is not in 1..4, so the switch falls through.
    //
    // set_pausation also tries to write the value back to %WINDIR%\acuvoice.ini. That
    // write fails for an unelevated process and the failure is ignored on both sides:
    // the in-memory value -- the one synthesis actually reads -- is set either way, and
    // the settings this wrapper wants remembered live in HKCU, not in the ini.
    set_pausation_(milliseconds, which);
}

bool engine::speak(const char* text, bool honour_tags,
                   segment_sink sink, void* user, queued_prosody* queued) const
{
    last_error_ = 0;
    if (!loaded_ || !text || !*text) {
        return false;
    }

    std::vector<unsigned char> ssb(SSB_BYTES, 0);
    std::vector<unsigned char> segment(SEGMENT_BUFFER);

    const int rc = initialize_SSB_(ssb.data(), text, 0);
    if (rc != 0) {
        last_error_ = rc;
        // 2870 is an utterance that came down to nothing but whitespace, which happens
        // constantly and is not worth a log line.
        if (rc != 2870) {
            DEBUG_LOG("initialize_SSB failed: %d (%s)", rc, error_text(rc));
        }
        return false;
    }

    set_tag_flag_(ssb.data(), honour_tags ? 1 : 0);

    bool ok = true;
    int segments = 0;
    while (isSeg_Available_(ssb.data())) {
        unsigned long len = 0;
        const int r = synth_to_buffer_(ssb.data(), segment.data(), &len);
        if (r != 0) {
            // Status 1 at the first segment is how the engine reports "nothing to say",
            // for input that survived initialize_SSB but held no speakable token.
            if (!(r == 1 && segments == 0)) {
                last_error_ = r;
                DEBUG_LOG("synth_to_buffer failed at segment %d: %d (%s)",
                          segments, r, error_text(r));
                ok = false;
            }
            break;
        }
        if (len > segment.size()) {
            DEBUG_LOG("synth_to_buffer reported %lu bytes into a %zu byte buffer; "
                      "stopping", len, segment.size());
            ok = false;
            break;
        }
        if (len && sink && !sink(segment.data(), len, user)) {
            break;   // the caller asked to stop; not an error
        }
        if (++segments > MAX_SEGMENTS) {
            DEBUG_LOG("synthesis produced more than %d segments; stopping", MAX_SEGMENTS);
            ok = false;
            break;
        }
    }

    if (queued) {
        // The tag parser queues these; nothing inside avcore ever reads them back, so
        // they have to be drained here or they would surface during a later utterance.
        int slot[4];
        const auto drain = [&slot](int(__stdcall *fn)(void*, void*), void* s,
                                   bool& has, int& value) {
            if (!fn) return;
            while (true) {
                slot[0] = slot[1] = slot[2] = slot[3] = 0;
                if (fn(s, slot) != 0) break;
                has = true;
                value = slot[0];
            }
        };
        drain(speed_dequeue_, ssb.data(), queued->has_speed, queued->speed);
        drain(pitch_dequeue_, ssb.data(), queued->has_pitch, queued->pitch);
        drain(volume_dequeue_, ssb.data(), queued->has_volume, queued->volume);
        drain(Rspd_dequeue_, ssb.data(), queued->has_rspeed, queued->rspeed);
        drain(Rpit_dequeue_, ssb.data(), queued->has_rpitch, queued->rpitch);
        drain(get_bookmark_, ssb.data(), queued->has_bookmark, queued->bookmark);
    }

    close_SSB_(ssb.data());
    return ok;
}

namespace {
struct collect_ctx {
    std::vector<unsigned char>* out;
};

bool collect(const unsigned char* data, size_t n, void* user)
{
    auto* ctx = static_cast<collect_ctx*>(user);
    ctx->out->insert(ctx->out->end(), data, data + n);
    return true;
}
}

bool engine::speak_all(const char* text, bool honour_tags,
                       std::vector<unsigned char>& ulaw, queued_prosody* queued) const
{
    ulaw.clear();
    collect_ctx ctx{ &ulaw };
    return speak(text, honour_tags, collect, &ctx, queued);
}

const char* engine::error_text(int code) noexcept
{
    switch (code) {
        case 0:    return "ok";
        case 1:    return "no more speech in this utterance";
        case 2700: return "null argument";
        case 2800: return "pitch out of range (45..91)";
        case 2810: return "speed out of range (85..350)";
        case 2820: return "volume out of range (0..65535)";
        case 2830: return "null input";
        case 2840: return "invalid input file";
        case 2850: return "null output";
        case 2870: return "nothing speakable in the text";
        case 2900: return "out of memory";
        case 3000: return "null synthesis status block";
        case 3010: return "the engine's channel limit is exhausted";
        default:   return "unknown engine status";
    }
}

// --------------------------------------------------------------------------------
// %WINDIR%\acuvoice.ini
// --------------------------------------------------------------------------------

std::wstring ini_path()
{
    wchar_t dir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(dir, MAX_PATH) == 0) {
        return L"C:\\Windows\\acuvoice.ini";
    }
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') {
        path += L'\\';
    }
    path += L"acuvoice.ini";
    return path;
}

namespace {
std::wstring ini_string(const wchar_t* section, const wchar_t* key, const std::wstring& path)
{
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 1024, path.c_str());
    return buf;
}
}

ini_config read_ini()
{
    const std::wstring path = ini_path();
    ini_config cfg;
    cfg.sound_bank = ini_string(L"AcuVoiceAppDir", L"SNDBANK", path);
    cfg.temp_dir = ini_string(L"AcuVoiceAppDir", L"TEMPDIR", path);
    cfg.dict_dir = ini_string(L"AcuVoiceAppDir", L"DICTFLSDIR", path);
    cfg.user_dict_dir = ini_string(L"AcuVoiceAppDir", L"USERDICTDIR", path);
    cfg.custom_dictionary = _wcsicmp(ini_string(L"AcuVoiceDictionary", L"CUSTOM", path).c_str(),
                                     L"YES") == 0;
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t key[16];
        swprintf_s(key, L"PAUSE%d", i + 1);
        cfg.pause[i] = static_cast<int>(
            GetPrivateProfileIntW(L"AcuVoiceSettings", key, PAUSE_DEFAULT[i], path.c_str()));
    }
    return cfg;
}

bool write_ini(const ini_config& cfg)
{
    const std::wstring path = ini_path();
    bool ok = true;
    const auto put = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(section, key, value.c_str(), path.c_str())) {
            ok = false;
        }
    };
    put(L"AcuVoiceAppDir", L"SNDBANK", cfg.sound_bank);
    put(L"AcuVoiceAppDir", L"TEMPDIR", cfg.temp_dir);
    put(L"AcuVoiceAppDir", L"DICTFLSDIR", cfg.dict_dir);
    put(L"AcuVoiceAppDir", L"USERDICTDIR", cfg.user_dict_dir);
    put(L"AcuVoiceDictionary", L"CUSTOM", cfg.custom_dictionary ? L"YES" : L"NO");
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        wchar_t key[16];
        swprintf_s(key, L"PAUSE%d", i + 1);
        put(L"AcuVoiceSettings", key, std::to_wstring(cfg.pause[i]));
    }
    if (!ok) {
        DEBUG_LOG("could not write %ls (error %lu) -- the engine keeps its current "
                  "directories until it is next restarted", path.c_str(), GetLastError());
    }
    return ok;
}

}  // namespace AcuVoice
