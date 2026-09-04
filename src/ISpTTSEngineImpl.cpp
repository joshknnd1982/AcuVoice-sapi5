#include <new>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#include "utils.hpp"
#include "text_prep.hpp"
#include "settings.hpp"
#include "voices.hpp"
#include "av_dsp.h"
#include "av_paths.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "debug_log.h"

#include "pipe_client.h"
#ifndef BUILD_X64
#include "av_engine.h"
#endif

namespace AcuVoice {
namespace sapi {

namespace {

constexpr WORD AUDIO_CHANNELS = 1;
constexpr WORD AUDIO_BITS_PER_SAMPLE = 16;

// The scales SAPI hands rate, pitch and volume out on.
constexpr int SAPI_RATE_MIN = -10, SAPI_RATE_MAX = 10;
constexpr int SAPI_PITCH_MIN = -10, SAPI_PITCH_MAX = 10;

// SAPI rate -10..+10 spans a quarter speed to four times speed, on top of whatever pace
// the voice itself is set to. That covers a beginner's pace and the very fast rates
// experienced screen reader users work at, which the engine's own 85..350 words a minute
// does not reach on its own.
[[nodiscard]] double sapi_rate_to_factor(int rate)
{
    return std::pow(2.0, std::clamp(rate, SAPI_RATE_MIN, SAPI_RATE_MAX) / 5.0);
}

// SAPI pitch -10..+10 maps onto the same six semitones either way that the engine's own
// 45..91 scale covers, so a host at full pitch and the utility at full pitch ask for the
// same thing rather than compounding into a chipmunk.
[[nodiscard]] double sapi_pitch_to_semitones(int pitch)
{
    return std::clamp(pitch, SAPI_PITCH_MIN, SAPI_PITCH_MAX) * 0.6;
}

// Beyond an octave either way the resampler starts folding the engine's 4 kHz band back
// on itself, and a voice that was merely odd becomes unintelligible.
constexpr double PITCH_FACTOR_MIN = 0.5;
constexpr double PITCH_FACTOR_MAX = 2.0;

// Audio is handed to SAPI in pieces this size, and a cancel is noticed between them.
// 4 KB is an eighth of a second at 16 kHz -- fast enough that stopping a screen reader
// feels immediate, large enough that the loop is not the bottleneck.
constexpr ULONG SITE_CHUNK_BYTES = 4096;

#ifdef BUILD_X64
PipeClient* g_pipeClient = nullptr;
#else
// One engine per process, shared by every voice object in it. avcore keeps its sound
// bank and its channel counter per module, so loading it twice would buy nothing.
AcuVoice::engine& shared_engine()
{
    static AcuVoice::engine instance;
    return instance;
}
CRITICAL_SECTION* engine_lock()
{
    static CRITICAL_SECTION cs;
    static bool ready = false;
    if (!ready) {
        InitializeCriticalSection(&cs);
        ready = true;
    }
    return &cs;
}
#endif

struct SpeakContext {
    ISpTTSEngineSite* caller = nullptr;
    ULONGLONG bytes_written = 0;
    bool aborted = false;
};

// SAPI's Write does not promise to take everything offered, and its pcbWritten is not
// reliable enough to drive a loop from on its own -- trusting it truncates an utterance
// mid-word -- so this writes until the count it was given is gone or Write reports a
// real failure.
bool write_to_site(const char* data, size_t size, SpeakContext& ctx)
{
    if (!ctx.caller) {
        return false;
    }
    auto* p = reinterpret_cast<const BYTE*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        const DWORD actions = ctx.caller->GetActions();
        if (actions & SPVES_ABORT) {
            ctx.aborted = true;
            return false;
        }
        if (actions & SPVES_SKIP) {
            ctx.caller->CompleteSkip(0);
            ctx.aborted = true;
            return false;
        }

        const ULONG want = static_cast<ULONG>(std::min<size_t>(remaining, SITE_CHUNK_BYTES));
        ULONG written = 0;
        const HRESULT hr = ctx.caller->Write(p, want, &written);
        if (FAILED(hr)) {
            DEBUG_LOG("SAPI Write failed: hr=0x%08X, %zu bytes left", hr, remaining);
            return false;
        }
        if (written == 0 || written > want) {
            // Some hosts leave pcbWritten alone entirely. Taking it at face value here
            // would either spin forever or skip ahead past audio that was never sent, so
            // a successful call is credited with the whole chunk it was handed.
            written = want;
        }
        ctx.bytes_written += written;
        remaining -= written;
        p += written;
    }
    return true;
}

bool pipe_callback(const char* data, uint32_t size, void* user)
{
    return write_to_site(data, size, *static_cast<SpeakContext*>(user));
}

void add_event(ISpTTSEngineSite* site, SPEVENTENUM id, ULONGLONG offset,
               WPARAM wparam, LPARAM lparam, SPEVENTLPARAMTYPE ltype)
{
    SPEVENT ev = {};
    ev.eEventId = id;
    ev.elParamType = ltype;
    ev.ullAudioStreamOffset = offset;
    ev.ulStreamNum = 0;
    ev.wParam = wparam;
    ev.lParam = lparam;
    site->AddEvents(&ev, 1);
}

void write_silence(SpeakContext& ctx, ULONG msecs)
{
    if (msecs == 0) {
        return;
    }
    const size_t samples = static_cast<size_t>(OUTPUT_SAMPLE_RATE) * msecs / 1000u;
    std::vector<int16_t> zeros(std::min<size_t>(samples, 4096), 0);
    size_t remaining = samples;
    while (remaining > 0 && !ctx.aborted) {
        const size_t chunk = std::min(remaining, zeros.size());
        if (!write_to_site(reinterpret_cast<const char*>(zeros.data()),
                           chunk * sizeof(int16_t), ctx)) {
            break;
        }
        remaining -= chunk;
    }
}

// Everything one fragment needs, once the host's numbers and the voice's own have been
// combined.
struct utterance_params {
    dsp::params dsp;
    int32_t pause[PAUSE_COUNT] = { -1, -1, -1, -1 };
    bool honour_tags = false;
    bool speak_punctuation = false;
};

[[nodiscard]] utterance_params build_params(const settings::voice_params& voice,
                                            const settings::global_settings& user,
                                            int sapi_rate, int sapi_pitch,
                                            unsigned short sapi_volume)
{
    utterance_params out;

    // The user's own rate multiplies what the host asked for, so a screen reader's rate
    // control keeps working on top of the utility's.
    const double host_rate = sapi_rate_to_factor(sapi_rate) * (user.rate_percent / 100.0);
    out.dsp.duration = speed_to_duration(voice.speed) / host_rate;

    const double semitones = pitch_to_semitones(voice.pitch) + sapi_pitch_to_semitones(sapi_pitch);
    out.dsp.pitch = std::clamp(std::pow(2.0, semitones / 12.0),
                               PITCH_FACTOR_MIN, PITCH_FACTOR_MAX);

    out.dsp.gain = volume_to_gain(voice.volume) *
                   (std::clamp<int>(sapi_volume, 0, 100) / 100.0) *
                   (user.volume_percent / 100.0);

    for (int i = 0; i < PAUSE_COUNT; ++i) {
        out.pause[i] = user.override_pauses ? user.pause[i] : voice.pause[i];
    }
    out.honour_tags = voice.honour_tags || user.honour_tags;
    out.speak_punctuation = voice.speak_punctuation || user.speak_punctuation;
    return out;
}

}  // namespace

#ifdef BUILD_X64
void InitPipeClient()
{
    if (!g_pipeClient) {
        g_pipeClient = new PipeClient();
    }
}

void CleanupPipeClient()
{
    delete g_pipeClient;
    g_pipeClient = nullptr;
}

void ShutdownPipeServer()
{
    if (g_pipeClient) {
        g_pipeClient->shutdownServer();
    }
}
#endif

ISpTTSEngineImpl::ISpTTSEngineImpl() = default;
ISpTTSEngineImpl::~ISpTTSEngineImpl() = default;

#ifndef BUILD_X64
bool ISpTTSEngineImpl::ensure_engine_loaded()
{
    CriticalLock lock(engine_lock());
    if (shared_engine().loaded()) {
        return true;
    }
    const std::wstring core = avcore_path();
    if (!shared_engine().load(core)) {
        DEBUG_LOG("FAILED to load the AcuVoice engine from %ls -- every voice will be "
                  "listed but silent", core.c_str());
        return false;
    }
    return true;
}
#endif

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attr;
        if (FAILED(pToken->OpenKey(L"Attributes", &attr))) {
            return E_INVALIDARG;
        }

        // The token names the voice directly, so the exact one is recovered without
        // having to parse a localized display name back apart.
        int index = 0;
        utils::out_ptr<wchar_t> voice_id(CoTaskMemFree);
        if (SUCCEEDED(attr->GetStringValue(L"AcuVoice", voice_id.address())) && voice_id.get()) {
            const std::wstring id = voice_id.get();
            if (id == CUSTOM_ID) {
                index = custom_token_index();
            } else {
                const int preset = preset_by_id(id);
                index = (preset >= 0) ? preset : 0;
            }
        }

        voice_ = voice_attributes(index);
        token_ = pToken;
        DEBUG_LOG("SetObjectToken: %ls", voice_.get_name().c_str());
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(
    const GUID* /*pTargetFmtId*/,
    const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId,
    WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }

    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    auto* wfex = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfex) {
        return E_OUTOFMEMORY;
    }

    // Declared from the constant rather than by asking the engine: GetOutputFormat is
    // called before anything has spoken, and loading a 154 MB sound bank to be told a
    // number that cannot change would put that cost on the voice list.
    wfex->wFormatTag = WAVE_FORMAT_PCM;
    wfex->nChannels = AUDIO_CHANNELS;
    wfex->nSamplesPerSec = OUTPUT_SAMPLE_RATE;
    wfex->wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    wfex->nBlockAlign = static_cast<WORD>(wfex->nChannels * wfex->wBitsPerSample / 8);
    wfex->nAvgBytesPerSec = wfex->nSamplesPerSec * wfex->nBlockAlign;
    wfex->cbSize = 0;

    *ppCoMemOutputWaveFormatEx = wfex;
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(
    DWORD /*dwSpeakFlags*/,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }

#ifdef BUILD_X64
    if (!g_pipeClient) {
        return E_FAIL;
    }
#else
    if (!ensure_engine_loaded()) {
        return E_FAIL;
    }
#endif

    try {
        // Re-read for every utterance, so a change made in the configuration utility
        // lands on the next thing a running screen reader says.
        const settings::global_settings user = settings::load_global();
        const settings::voice_params voice = voice_.is_custom()
            ? settings::load_custom()
            : settings::preset_params(voice_.index());

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        unsigned short sapi_volume = 100;
        pOutputSite->GetVolume(&sapi_volume);

        ULONGLONG interest = 0;
        pOutputSite->GetEventInterest(&interest);
        const bool want_sentence = (interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_word = (interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;

        DEBUG_LOG("=== Speak: voice=%ls, host rate=%d volume=%u ===",
                  voice_.get_name().c_str(), static_cast<int>(sapi_rate), sapi_volume);

        SpeakContext ctx;
        ctx.caller = pOutputSite;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                break;
            }
            if (actions & SPVES_SKIP) {
                pOutputSite->CompleteSkip(0);
                break;
            }
            // Rate and volume can be changed while an utterance is already playing.
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&sapi_volume);
            }

            // Not every fragment is speech. A bookmark spoken aloud is how a screen
            // reader ends up reading its own position numbers out.
            if (frag->State.eAction == SPVA_Bookmark) {
                const std::wstring mark = (frag->ulTextLen && frag->pTextStart)
                    ? std::wstring(frag->pTextStart, frag->ulTextLen) : std::wstring();
                add_event(pOutputSite, SPEI_TTS_BOOKMARK, ctx.bytes_written,
                          static_cast<WPARAM>(mark.empty() ? 0 : _wtol(mark.c_str())),
                          reinterpret_cast<LPARAM>(mark.c_str()),
                          mark.empty() ? SPET_LPARAM_IS_UNDEFINED : SPET_LPARAM_IS_STRING);
                continue;
            }
            if (frag->State.eAction == SPVA_Silence) {
                write_silence(ctx, frag->State.SilenceMSecs);
                if (ctx.aborted) {
                    break;
                }
                continue;
            }
            // SPVA_Pronounce carries a phoneme string in a notation this engine has no
            // way to read, so its text is spoken normally rather than dropped.
            if (frag->State.eAction != SPVA_Speak &&
                frag->State.eAction != SPVA_SpellOut &&
                frag->State.eAction != SPVA_Pronounce) {
                continue;
            }
            if (frag->ulTextLen == 0 || !frag->pTextStart) {
                continue;
            }

            const std::wstring raw(frag->pTextStart, frag->ulTextLen);

            if (want_sentence) {
                add_event(pOutputSite, SPEI_SENTENCE_BOUNDARY, ctx.bytes_written,
                          frag->ulTextLen, frag->ulTextSrcOffset, SPET_LPARAM_IS_UNDEFINED);
            }
            if (want_word) {
                bool in_word = false;
                ULONG start = 0;
                for (ULONG i = 0; i <= frag->ulTextLen; ++i) {
                    const bool word_char = (i < frag->ulTextLen) &&
                        (iswalnum(raw[i]) || raw[i] == L'\'' || raw[i] == L'-');
                    if (word_char && !in_word) {
                        start = i;
                        in_word = true;
                    } else if (!word_char && in_word) {
                        add_event(pOutputSite, SPEI_WORD_BOUNDARY, ctx.bytes_written,
                                  i - start, frag->ulTextSrcOffset + start,
                                  SPET_LPARAM_IS_UNDEFINED);
                        in_word = false;
                    }
                }
            }

            // Fragment adjustments ride on top of the stream-wide settings.
            const int frag_rate = std::clamp<int>(
                static_cast<int>(sapi_rate) + frag->State.RateAdj, SAPI_RATE_MIN, SAPI_RATE_MAX);
            const int frag_pitch = std::clamp<int>(
                static_cast<int>(frag->State.PitchAdj.MiddleAdj), SAPI_PITCH_MIN, SAPI_PITCH_MAX);
            const unsigned short frag_volume = static_cast<unsigned short>(
                std::clamp<int>(sapi_volume * frag->State.Volume / 100, 0, 100));

            const utterance_params p =
                build_params(voice, user, frag_rate, frag_pitch, frag_volume);

            std::wstring body = text::normalize(raw, p.honour_tags);
            if (frag->State.eAction == SPVA_SpellOut) {
                body = text::spell_out(body);
            } else if (p.speak_punctuation) {
                body = text::name_punctuation(body);
            }
            if (body.empty()) {
                continue;
            }

            DEBUG_LOG("  fragment: action=%d rate=%d pitch=%d volume=%u -> "
                      "duration x%.3f pitch x%.3f gain x%.3f pauses %d/%d/%d/%d tags=%d",
                      static_cast<int>(frag->State.eAction), frag_rate, frag_pitch, frag_volume,
                      p.dsp.duration, p.dsp.pitch, p.dsp.gain,
                      p.pause[0], p.pause[1], p.pause[2], p.pause[3],
                      static_cast<int>(p.honour_tags));

            // A long fragment is synthesized a sentence or so at a time so the first
            // audio starts before the whole thing exists, and so a cancel lands inside a
            // sentence rather than after the paragraph.
            const std::vector<std::wstring> pieces = text::split_for_synthesis(body);
            const ULONGLONG before = ctx.bytes_written;

            for (const std::wstring& piece : pieces) {
                if (ctx.aborted) {
                    break;
                }
                const std::string encoded = text::to_engine_bytes(piece);
                if (encoded.empty()) {
                    continue;
                }

#ifdef BUILD_X64
                if (!g_pipeClient->speak(encoded.c_str(),
                                         static_cast<uint32_t>(encoded.size()),
                                         p.dsp.duration, p.dsp.pitch, p.dsp.gain,
                                         p.honour_tags, p.pause,
                                         pipe_callback, &ctx)) {
                    DEBUG_LOG("  the worker did not answer; the utterance stops here");
                    break;
                }
#else
                std::vector<unsigned char> ulaw;
                {
                    CriticalLock lock(engine_lock());
                    for (int i = 0; i < PAUSE_COUNT; ++i) {
                        if (p.pause[i] >= 0) {
                            shared_engine().set_pause(i + 1, p.pause[i]);
                        }
                    }
                    (void)shared_engine().speak_all(encoded.c_str(), p.honour_tags, ulaw);
                }
                if (ulaw.empty()) {
                    continue;
                }

                std::vector<int16_t> pcm;
                dsp::ulaw_to_pcm16(ulaw.data(), ulaw.size(), pcm);
                std::vector<int16_t> out;
                dsp::render(pcm, p.dsp, out);
                write_to_site(reinterpret_cast<const char*>(out.data()),
                              out.size() * sizeof(int16_t), ctx);
#endif
            }

            DEBUG_LOG("  <- %llu bytes this fragment, %llu total%s",
                      ctx.bytes_written - before, ctx.bytes_written,
                      ctx.aborted ? " (host cancelled)" : "");

            if (ctx.aborted) {
                break;
            }
        }

        return S_OK;
    }
    catch (const std::bad_alloc&) {
        DEBUG_LOG("Speak failed: out of memory");
        return E_OUTOFMEMORY;
    }
    catch (...) {
        DEBUG_LOG("Speak failed: unexpected exception");
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace AcuVoice
