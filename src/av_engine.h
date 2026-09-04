#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// The AcuVoice engine core, avcore.dll.
//
// avcore.dll is the whole engine. AcuEng.dll -- the 1999 SAPI4 wrapper that sat on top
// of it -- is not used, is not installed, and is not needed: avcore exports a plain
// stdcall C API and imports nothing but kernel32 and user32, so there is no COM, no
// SAPI4 and no registry anywhere under this.
//
// The one thing avcore does *not* do is prosody. It parses the backslash tag language
// out of the text and queues the values for the caller to act on -- every rate, pitch
// and volume renders byte-identical audio -- so rate, pitch and volume are applied by
// av_dsp on the way out. See README.md for how that was established.

namespace AcuVoice {

// The engine's native output: 8 kHz mono 8-bit mu-law.
inline constexpr int ENGINE_SAMPLE_RATE = 8000;

// What this wrapper hands to SAPI. Twice the engine rate, so raising the pitch has
// somewhere to put the frequencies it moves up: at 8 kHz, a pitch factor above 1.0
// pushes the top of the engine's 4 kHz band past Nyquist and it folds back as aliasing.
inline constexpr int OUTPUT_SAMPLE_RATE = 16000;

// The tag parser's own limits, read out of avcore's range checks.
inline constexpr int SPEED_MIN = 85,    SPEED_DEFAULT = 175,   SPEED_MAX = 350;
inline constexpr int PITCH_MIN = 45,    PITCH_DEFAULT = 63,    PITCH_MAX = 91;
inline constexpr int VOLUME_MIN = 0,    VOLUME_DEFAULT = 32767, VOLUME_MAX = 65535;
inline constexpr int RSPEED_MIN = 50,   RSPEED_DEFAULT = 100,  RSPEED_MAX = 200;
inline constexpr int RPITCH_MIN = 70,   RPITCH_DEFAULT = 100,  RPITCH_MAX = 145;

// Pausation slots, in milliseconds, with the values avcore falls back to when the ini
// says nothing. Unlike the prosody tags these are real: they change what the engine
// synthesizes, because it is the engine that inserts the silence.
inline constexpr int PAUSE_COUNT = 4;
inline constexpr int PAUSE_DEFAULT[PAUSE_COUNT] = { 650, 500, 350, 7 };
inline constexpr int PAUSE_MAX[PAUSE_COUNT] = { 3000, 3000, 3000, 100 };

// Values the tag parser queued during the last utterance. Only meaningful when the
// caller turned tags on; avcore never acts on them itself.
struct queued_prosody {
    bool has_speed = false;   int speed = SPEED_DEFAULT;
    bool has_pitch = false;   int pitch = PITCH_DEFAULT;
    bool has_volume = false;  int volume = VOLUME_DEFAULT;
    bool has_rspeed = false;  int rspeed = RSPEED_DEFAULT;
    bool has_rpitch = false;  int rpitch = RPITCH_DEFAULT;
    bool has_bookmark = false; int bookmark = 0;
};

// Return false to stop synthesis; the engine's remaining segments are dropped.
using segment_sink = bool (*)(const unsigned char* ulaw, size_t count, void* user);

class engine {
public:
    engine() = default;
    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    // Loads avcore.dll from an explicit path. Safe to call more than once; the dll is
    // loaded once per process and shared, which is what the engine expects -- its
    // channel counter lives in a shared section.
    [[nodiscard]] bool load(const std::wstring& avcore_path);
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

    [[nodiscard]] std::string version() const;
    [[nodiscard]] int sample_rate() const;   // 8000
    [[nodiscard]] int bits() const;          // 8
    [[nodiscard]] int format() const;        // 7 == WAVE_FORMAT_MULAW
    [[nodiscard]] int channels_allowed() const;

    [[nodiscard]] int get_pause(int which) const;         // which is 1..4
    void set_pause(int which, int milliseconds) const;

    // Synthesizes one utterance, handing every segment to the sink as it arrives.
    // `text` is single-byte, in the engine's own codepage (Windows-1252).
    // `honour_tags` turns on avcore's backslash tag parser; with it off the text is
    // spoken exactly as written, which is what a screen reader reading a Windows path
    // needs.
    [[nodiscard]] bool speak(const char* text, bool honour_tags,
                             segment_sink sink, void* user,
                             queued_prosody* queued = nullptr) const;

    // Convenience wrapper that collects the whole utterance.
    [[nodiscard]] bool speak_all(const char* text, bool honour_tags,
                                 std::vector<unsigned char>& ulaw,
                                 queued_prosody* queued = nullptr) const;

    // The last engine status code, for the log.
    [[nodiscard]] int last_error() const noexcept { return last_error_; }
    [[nodiscard]] static const char* error_text(int code) noexcept;

private:
    HMODULE dll_ = nullptr;
    bool loaded_ = false;
    mutable int last_error_ = 0;

    // avcore's exports, all __stdcall.
    int(__stdcall *get_av_version_)(char*) = nullptr;
    int(__stdcall *initialize_SSB_)(void*, const char*, int) = nullptr;
    int(__stdcall *close_SSB_)(void*) = nullptr;
    int(__stdcall *set_tag_flag_)(void*, int) = nullptr;
    int(__stdcall *isSeg_Available_)(void*) = nullptr;
    int(__stdcall *synth_to_buffer_)(void*, void*, unsigned long*) = nullptr;
    int(__stdcall *get_snd_fmt_)() = nullptr;
    int(__stdcall *get_snd_bps_)() = nullptr;
    int(__stdcall *get_snd_sps_)() = nullptr;
    int(__stdcall *get_channels_allowed_)() = nullptr;
    int(__stdcall *get_pausation_)(int) = nullptr;
    int(__stdcall *set_pausation_)(int, int) = nullptr;
    int(__stdcall *pitch_dequeue_)(void*, void*) = nullptr;
    int(__stdcall *speed_dequeue_)(void*, void*) = nullptr;
    int(__stdcall *volume_dequeue_)(void*, void*) = nullptr;
    int(__stdcall *Rpit_dequeue_)(void*, void*) = nullptr;
    int(__stdcall *Rspd_dequeue_)(void*, void*) = nullptr;
    int(__stdcall *get_bookmark_)(void*, void*) = nullptr;
};

// The ini avcore reads its directories out of. It is opened by bare filename through
// GetPrivateProfileStringA, so Windows resolves it against the Windows directory and
// there is nowhere else it can live.
[[nodiscard]] std::wstring ini_path();

struct ini_config {
    std::wstring sound_bank;
    std::wstring temp_dir;
    std::wstring dict_dir;
    std::wstring user_dict_dir;
    bool custom_dictionary = false;
    int pause[PAUSE_COUNT] = { PAUSE_DEFAULT[0], PAUSE_DEFAULT[1],
                               PAUSE_DEFAULT[2], PAUSE_DEFAULT[3] };
};

[[nodiscard]] ini_config read_ini();
[[nodiscard]] bool write_ini(const ini_config& cfg);

}  // namespace AcuVoice
