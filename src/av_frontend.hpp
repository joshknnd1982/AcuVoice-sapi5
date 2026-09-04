#pragma once

// One way to reach the engine, whichever architecture the caller was built for.
//
// avcore.dll is a 1999 32-bit binary and there is no 64-bit build of it, so a 64-bit
// process cannot load it at all -- not now and not ever, short of someone rewriting the
// engine. A 64-bit tool therefore reaches it through AcuVoiceServer.exe, the 32-bit
// worker, over the same named pipe the 64-bit SAPI engine uses; a 32-bit tool loads it
// directly. Everything above this header is written once and works either way.

#include <cstdint>
#include <string>
#include <vector>

#include "av_dsp.h"
#include "av_engine.h"
#include "av_paths.hpp"
#include "pipe_client.h"

namespace AcuVoice {
namespace frontend {

// What the About box and the diagnostics report show. Filled in from avcore directly on
// a 32-bit build and from the worker on a 64-bit one; identical either way, because it
// is the same engine answering.
struct engine_facts {
    bool available = false;
    std::string version = "not loaded";
    int engine_rate = ENGINE_SAMPLE_RATE;
    int bits = 8;
    int format = 7;
    int channels_allowed = 0;
    int output_rate = OUTPUT_SAMPLE_RATE;
    // Only a 32-bit build can ask avcore these directly; the worker owns them otherwise.
    int pause[PAUSE_COUNT] = { PAUSE_DEFAULT[0], PAUSE_DEFAULT[1],
                               PAUSE_DEFAULT[2], PAUSE_DEFAULT[3] };
};

#ifdef BUILD_X64

inline PipeClient& pipe()
{
    static PipeClient client;
    return client;
}

inline bool ready()
{
    EngineInfo info;
    return pipe().engineInfo(info);
}

inline engine_facts facts()
{
    engine_facts out;
    EngineInfo info = {};
    if (!pipe().engineInfo(info)) {
        return out;
    }
    out.available = true;
    out.version = info.version;
    out.engine_rate = static_cast<int>(info.engine_rate);
    out.bits = static_cast<int>(info.bits);
    out.format = static_cast<int>(info.format);
    out.channels_allowed = static_cast<int>(info.channels_allowed);
    out.output_rate = static_cast<int>(info.sample_rate);
    return out;
}

namespace detail {
struct sink_state { std::vector<int16_t>* out; };

inline bool sink(const char* data, uint32_t size, void* user)
{
    auto* s = static_cast<sink_state*>(user);
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    s->out->insert(s->out->end(), samples, samples + size / sizeof(int16_t));
    return true;
}
}

// `text` is already Windows-1252 and already prepared. `out` comes back at
// OUTPUT_SAMPLE_RATE, 16-bit mono, with the prosody applied.
inline bool render(const std::string& text, const dsp::params& p,
                   const int32_t pause[PAUSE_COUNT], bool honour_tags,
                   std::vector<int16_t>& out)
{
    out.clear();
    detail::sink_state state{ &out };
    return pipe().speak(text.c_str(), static_cast<uint32_t>(text.size()),
                        p.duration, p.pitch, p.gain, honour_tags, pause,
                        detail::sink, &state);
}

#else  // 32-bit: avcore loads in process

inline engine& core()
{
    static engine instance;
    return instance;
}

inline bool ready()
{
    return core().loaded() || core().load(avcore_path());
}

inline engine_facts facts()
{
    engine_facts out;
    if (!ready()) {
        return out;
    }
    out.available = true;
    out.version = core().version();
    out.engine_rate = core().sample_rate();
    out.bits = core().bits();
    out.format = core().format();
    out.channels_allowed = core().channels_allowed();
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        out.pause[i] = core().get_pause(i + 1);
    }
    return out;
}

inline bool render(const std::string& text, const dsp::params& p,
                   const int32_t pause[PAUSE_COUNT], bool honour_tags,
                   std::vector<int16_t>& out)
{
    out.clear();
    if (!ready()) {
        return false;
    }
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        if (pause && pause[i] >= 0) {
            core().set_pause(i + 1, pause[i]);
        }
    }
    std::vector<unsigned char> ulaw;
    if (!core().speak_all(text.c_str(), honour_tags, ulaw) && ulaw.empty()) {
        return false;
    }
    std::vector<int16_t> pcm;
    dsp::ulaw_to_pcm16(ulaw.data(), ulaw.size(), pcm);
    dsp::render(pcm, p, out);
    return true;
}

#endif

inline constexpr int32_t DEFAULT_PAUSES[PAUSE_COUNT] = {
    PAUSE_DEFAULT[0], PAUSE_DEFAULT[1], PAUSE_DEFAULT[2], PAUSE_DEFAULT[3]
};

}  // namespace frontend
}  // namespace AcuVoice
