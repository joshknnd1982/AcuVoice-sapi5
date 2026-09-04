// AcuVoiceDiagnostics.exe -- proves the engine and the SAPI5 wrapper actually work, and
// writes the evidence out as wave files.
//
//   AcuVoiceDiagnostics                 report on the engine and the registered voices
//   AcuVoiceDiagnostics samples <dir>   write the engine sample set into <dir>
//   AcuVoiceDiagnostics sapi <dir>      speak every registered voice through SAPI5 into <dir>
//   AcuVoiceDiagnostics all <dir>       both, plus the report
//   AcuVoiceDiagnostics say "<text>"    speak once, to the sound card
//
// The 32-bit build drives avcore.dll in process, exactly as the 32-bit SAPI engine does.
// The 64-bit build goes through AcuVoiceServer.exe, exactly as the 64-bit SAPI engine
// does, so running both is a real test of both paths.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <objbase.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "av_dsp.h"
#include "av_paths.hpp"
#include "text_prep.hpp"
#include "settings.hpp"
#include "voices.hpp"
#include "voice_attributes.hpp"
#include "pipe_client.h"

#ifndef BUILD_X64
#include "av_engine.h"
#endif

namespace {

std::wstring g_outdir = L".";

void ensure_dir(const std::wstring& dir)
{
    CreateDirectoryW(dir.c_str(), nullptr);
}

bool write_wav(const std::wstring& name, const std::vector<int16_t>& pcm, int rate)
{
    const std::wstring path = g_outdir + L"\\" + name;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        wprintf(L"  !! could not write %s\n", path.c_str());
        return false;
    }
    const uint32_t datasz = static_cast<uint32_t>(pcm.size() * 2);
    const uint32_t riff = 36 + datasz;
    const uint32_t fmtsz = 16;
    const uint16_t tag = 1, ch = 1, bits = 16, align = 2;
    const uint32_t sr = static_cast<uint32_t>(rate), avg = sr * align;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    wprintf(L"  %-38s %7.2f s\n", name.c_str(),
            static_cast<double>(pcm.size()) / rate);
    return true;
}

// -------------------------------------------------------------------------------
// The engine, reached the same way the matching SAPI build reaches it.
// -------------------------------------------------------------------------------

#ifdef BUILD_X64
PipeClient g_pipe;

struct collect { std::vector<int16_t>* out; };

bool collect_cb(const char* data, uint32_t size, void* user)
{
    auto* c = static_cast<collect*>(user);
    const size_t n = size / sizeof(int16_t);
    const auto* s = reinterpret_cast<const int16_t*>(data);
    c->out->insert(c->out->end(), s, s + n);
    return true;
}

bool engine_ready()
{
    EngineInfo info;
    return g_pipe.engineInfo(info);
}

bool render(const std::string& text, const AcuVoice::dsp::params& p,
            const int32_t pause[4], bool tags, std::vector<int16_t>& out)
{
    out.clear();
    collect c{ &out };
    return g_pipe.speak(text.c_str(), static_cast<uint32_t>(text.size()),
                        p.duration, p.pitch, p.gain, tags, pause, collect_cb, &c);
}
#else
AcuVoice::engine g_engine;

bool engine_ready()
{
    return g_engine.loaded() || g_engine.load(AcuVoice::avcore_path());
}

bool render(const std::string& text, const AcuVoice::dsp::params& p,
            const int32_t pause[4], bool tags, std::vector<int16_t>& out)
{
    out.clear();
    if (!engine_ready()) {
        return false;
    }
    for (int i = 0; i < AcuVoice::PAUSE_COUNT; ++i) {
        if (pause && pause[i] >= 0) {
            g_engine.set_pause(i + 1, pause[i]);
        }
    }
    std::vector<unsigned char> ulaw;
    if (!g_engine.speak_all(text.c_str(), tags, ulaw) && ulaw.empty()) {
        return false;
    }
    std::vector<int16_t> pcm;
    AcuVoice::dsp::ulaw_to_pcm16(ulaw.data(), ulaw.size(), pcm);
    AcuVoice::dsp::render(pcm, p, out);
    return true;
}
#endif

const int32_t DEFAULT_PAUSES[4] = {
    AcuVoice::PAUSE_DEFAULT[0], AcuVoice::PAUSE_DEFAULT[1],
    AcuVoice::PAUSE_DEFAULT[2], AcuVoice::PAUSE_DEFAULT[3]
};

void sample(const wchar_t* name, const std::wstring& text,
            const AcuVoice::dsp::params& p = {},
            const int32_t* pause = DEFAULT_PAUSES, bool tags = false)
{
    std::vector<int16_t> pcm;
    const std::string bytes = AcuVoice::text::to_engine_bytes(text);
    if (!render(bytes, p, pause, tags, pcm) || pcm.empty()) {
        wprintf(L"  !! %s produced no audio\n", name);
        return;
    }
    write_wav(name, pcm, AcuVoice::OUTPUT_SAMPLE_RATE);
}

// -------------------------------------------------------------------------------
// Reports
// -------------------------------------------------------------------------------

void report_engine()
{
    wprintf(L"AcuVoice engine\n");
    wprintf(L"  install root      %s\n", AcuVoice::install_root().c_str());
    wprintf(L"  avcore.dll        %s\n", AcuVoice::avcore_path().c_str());
    wprintf(L"  configuration     %s\n", AcuVoice::ini_path().c_str());

    const AcuVoice::ini_config cfg = AcuVoice::read_ini();
    wprintf(L"  sound bank        %s\n", cfg.sound_bank.c_str());
    wprintf(L"  dictionary        %s\n", cfg.dict_dir.c_str());
    wprintf(L"  temp              %s\n", cfg.temp_dir.c_str());
    wprintf(L"  user dictionary   %s  (custom entries %s)\n",
            cfg.user_dict_dir.c_str(), cfg.custom_dictionary ? L"on" : L"off");

#ifdef BUILD_X64
    EngineInfo info = {};
    if (g_pipe.engineInfo(info)) {
        wprintf(L"  worker reports    avcore %S, %u Hz %u-bit format %u, %u channels\n",
                info.version, info.engine_rate, info.bits, info.format,
                info.channels_allowed);
        wprintf(L"  output to SAPI    %u Hz 16-bit mono\n", info.sample_rate);
    } else {
        wprintf(L"  !! AcuVoiceServer.exe did not answer\n");
    }
#else
    if (engine_ready()) {
        wprintf(L"  avcore version    %S\n", g_engine.version().c_str());
        wprintf(L"  engine output     %d Hz %d-bit format %d (%s)\n",
                g_engine.sample_rate(), g_engine.bits(), g_engine.format(),
                g_engine.format() == 7 ? L"mu-law" : L"?");
        wprintf(L"  output to SAPI    %d Hz 16-bit mono\n", AcuVoice::OUTPUT_SAMPLE_RATE);
        wprintf(L"  channels allowed  %d\n", g_engine.channels_allowed());
        wprintf(L"  pause lengths     %d / %d / %d / %d ms\n",
                g_engine.get_pause(1), g_engine.get_pause(2),
                g_engine.get_pause(3), g_engine.get_pause(4));
    } else {
        wprintf(L"  !! avcore.dll could not be loaded\n");
    }
#endif

    wprintf(L"\nParameter ranges the engine's own tag parser enforces\n");
    wprintf(L"  speed   \\spd=   %d .. %d   (default %d words per minute)\n",
            AcuVoice::SPEED_MIN, AcuVoice::SPEED_MAX, AcuVoice::SPEED_DEFAULT);
    wprintf(L"  pitch   \\pit=   %d .. %d    (default %d)\n",
            AcuVoice::PITCH_MIN, AcuVoice::PITCH_MAX, AcuVoice::PITCH_DEFAULT);
    wprintf(L"  volume  \\vol=   %d .. %d (default %d)\n",
            AcuVoice::VOLUME_MIN, AcuVoice::VOLUME_MAX, AcuVoice::VOLUME_DEFAULT);
    wprintf(L"  rel. speed \\rspd= %d .. %d   rel. pitch \\Rpit= %d .. %d\n",
            AcuVoice::RSPEED_MIN, AcuVoice::RSPEED_MAX,
            AcuVoice::RPITCH_MIN, AcuVoice::RPITCH_MAX);

    wprintf(L"\nVoices published to SAPI5\n");
    for (int i = 0; i < AcuVoice::sapi::total_token_count(); ++i) {
        const AcuVoice::sapi::voice_attributes v(i);
        wprintf(L"  %-28s %s\n", v.get_name().c_str(), v.get_summary().c_str());
    }
    wprintf(L"\n");
}

void report_registered_voices()
{
    int mine = 0, total = 0;
    wprintf(L"AcuVoice voices in the machine's SAPI5 voice list\n");
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&cat)) || !cat) {
        wprintf(L"  !! could not open the voice category\n");
        return;
    }
    IEnumSpObjectTokens* en = nullptr;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) &&
        SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en) {
        ULONG count = 0;
        en->GetCount(&count);
        for (ULONG i = 0; i < count; ++i) {
            ISpObjectToken* tok = nullptr;
            if (FAILED(en->Item(i, &tok)) || !tok) {
                continue;
            }
            LPWSTR desc = nullptr;
            if (SUCCEEDED(tok->GetStringValue(nullptr, &desc)) && desc) {
                if (wcsstr(desc, L"AcuVoice") != nullptr) {
                    wprintf(L"  * %s\n", desc);
                    ++mine;
                }
                ++total;
                CoTaskMemFree(desc);
            }
            tok->Release();
        }
        en->Release();
    }
    cat->Release();
    wprintf(L"  %d of the %d voices installed on this machine are AcuVoice\n", mine, total);
    if (mine == 0) {
        wprintf(L"  (nothing installed machine-wide yet, so the SAPI test below falls "
                L"back to the per-user tokens reg_test writes)\n");
    }
    wprintf(L"\n");
}

// -------------------------------------------------------------------------------
// The engine sample set
// -------------------------------------------------------------------------------

const wchar_t* PANGRAM =
    L"The quick brown fox jumps over the lazy dog, and then it does so once again.";

void write_engine_samples()
{
    wprintf(L"Engine samples in %s\n", g_outdir.c_str());

    sample(L"engine_01_default.wav",
           L"This is AcuVoice, speaking with the parameters the engine itself was "
           L"shipped with.");

    // Rate, at the engine's own limits and at what the host can ask for on top.
    for (const int wpm : { 85, 120, 175, 260, 350 }) {
        AcuVoice::dsp::params p;
        p.duration = AcuVoice::speed_to_duration(wpm);
        wchar_t name[64];
        swprintf_s(name, L"engine_02_speed_%03d_wpm.wav", wpm);
        wchar_t text[256];
        swprintf_s(text, L"This is %d words a minute on the AcuVoice speed scale. %s",
                   wpm, PANGRAM);
        sample(name, text, p);
    }

    // Pitch, across the engine's 45 to 91.
    for (const int pit : { 45, 54, 63, 77, 91 }) {
        AcuVoice::dsp::params p;
        p.pitch = AcuVoice::pitch_to_factor(pit);
        wchar_t name[64];
        swprintf_s(name, L"engine_03_pitch_%02d.wav", pit);
        wchar_t text[256];
        swprintf_s(text, L"Pitch %d on the AcuVoice scale, %+.1f semitones. %s",
                   pit, AcuVoice::pitch_to_semitones(pit), PANGRAM);
        sample(name, text, p);
    }

    // Volume, across the engine's 0 to 65535.
    for (const int vol : { 6553, 16384, 32767, 49151, 65535 }) {
        AcuVoice::dsp::params p;
        p.gain = AcuVoice::volume_to_gain(vol);
        wchar_t name[64];
        swprintf_s(name, L"engine_04_volume_%05d.wav", vol);
        wchar_t text[256];
        swprintf_s(text, L"Volume %d on the AcuVoice scale. %s", vol, PANGRAM);
        sample(name, text, p);
    }

    // Pause lengths, the one control the engine really does apply itself.
    {
        const int32_t none[4] = { 0, 0, 0, 0 };
        const int32_t wide[4] = { 1500, 1200, 900, 30 };
        sample(L"engine_05_pauses_default.wav",
               L"First sentence. Second sentence, with a clause. Third sentence.",
               {}, DEFAULT_PAUSES);
        sample(L"engine_05_pauses_none.wav",
               L"First sentence. Second sentence, with a clause. Third sentence.",
               {}, none);
        sample(L"engine_05_pauses_long.wav",
               L"First sentence. Second sentence, with a clause. Third sentence.",
               {}, wide);
    }

    // Every published voice, with its own parameters.
    for (int i = 0; i < AcuVoice::PRESET_COUNT; ++i) {
        const AcuVoice::settings::voice_params vp = AcuVoice::settings::preset_params(i);
        AcuVoice::dsp::params p;
        p.duration = AcuVoice::speed_to_duration(vp.speed);
        p.pitch = AcuVoice::pitch_to_factor(vp.pitch);
        p.gain = AcuVoice::volume_to_gain(vp.volume);
        const int32_t pause[4] = { vp.pause[0], vp.pause[1], vp.pause[2], vp.pause[3] };
        wchar_t name[96];
        swprintf_s(name, L"engine_06_voice_%d_%s.wav", i + 1, AcuVoice::PRESETS[i].id);
        std::wstring text = std::wstring(L"This is ") + AcuVoice::PRESETS[i].name +
                            L". " + AcuVoice::PRESETS[i].summary +
                            L" The quick brown fox jumps over the lazy dog.";
        sample(name, text, p, pause);
    }

    // What the front end does with real text. None of this is the wrapper's work:
    // avcore has its own number, date, money and abbreviation expansion.
    sample(L"engine_07_numbers.wav",
           L"In 1998 the price rose 7 percent to $42.50, and 3.14159 is close enough to pi. "
           L"Call 555-1234 at 9:30 a.m. on July 4th.");
    sample(L"engine_08_abbreviations.wav",
           L"Dr. Smith lives at 100 N. Main St. and works Mon. through Fri., approx. 40 hrs.");
    sample(L"engine_09_punctuation.wav",
           L"Wait -- what? Really! (Yes.) A; B: C, D... and so it ends.");
    sample(L"engine_10_symbols.wav",
           L"Write to joe@example.com, or read http://www.acuvoice.com for 50% off.");
    sample(L"engine_11_accents.wav",
           L"A caf\u00e9 in Z\u00fcrich served a na\u00efve r\u00e9sum\u00e9 with jalape\u00f1os \u2014 "
           L"\u201cvery good,\u201d he said.");
    sample(L"engine_12_long_paragraph.wav",
           L"AcuVoice was a concatenative text to speech engine built in San Jose, California "
           L"during the middle nineteen nineties, and acquired by Fonix in nineteen ninety "
           L"eight. Its sound bank stores real recorded speech rather than a formant model, "
           L"which is why it still sounds more human than most synthesizers of its own decade. "
           L"This paragraph is here to show how it handles running text over several sentences.");

    // Spelling, which the wrapper does by separating the characters.
    sample(L"engine_13_spelled.wav",
           AcuVoice::text::spell_out(L"AcuVoice 2001"));
    sample(L"engine_14_punctuation_named.wav",
           AcuVoice::text::name_punctuation(L"C:\\Program Files\\AcuVoice (x86)"));

    // The engine's own tag language, with the parser turned on.
    sample(L"engine_15_tags_bookmark.wav",
           L"\\mrk=42\\ A bookmark tag reaches the engine and comes back out of get_bookmark.",
           {}, DEFAULT_PAUSES, true);
    sample(L"engine_16_tags_literal.wav",
           L"With tags off, a path like C:\\spd=250\\notes is read as a path.",
           {}, DEFAULT_PAUSES, false);

    // Rate and pitch together, at the extremes the wrapper allows.
    {
        AcuVoice::dsp::params p;
        p.duration = AcuVoice::speed_to_duration(350) / 2.0;   // host asking for more on top
        p.pitch = AcuVoice::pitch_to_factor(45);
        sample(L"engine_17_fast_and_low.wav",
               L"This is as fast and as low as the wrapper will go. " + std::wstring(PANGRAM), p);
    }
    {
        AcuVoice::dsp::params p;
        p.duration = AcuVoice::speed_to_duration(85) * 2.0;
        p.pitch = AcuVoice::pitch_to_factor(91);
        sample(L"engine_18_slow_and_high.wav",
               L"This is as slow and as high as the wrapper will go. " + std::wstring(PANGRAM), p);
    }

    wprintf(L"\n");
}

// -------------------------------------------------------------------------------
// The SAPI5 round trip: every registered AcuVoice voice, spoken into a wave file.
// -------------------------------------------------------------------------------

bool speak_through_sapi(ISpObjectToken* token, const std::wstring& text,
                        const std::wstring& file, long rate, long volume)
{
    ISpVoice* voice = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                IID_ISpVoice, (void**)&voice)) || !voice) {
        wprintf(L"  !! could not create SpVoice\n");
        return false;
    }
    bool ok = false;
    ISpStream* stream = nullptr;

    do {
        if (FAILED(voice->SetVoice(token))) {
            wprintf(L"  !! SetVoice failed\n");
            break;
        }
        voice->SetRate(rate);
        voice->SetVolume(static_cast<USHORT>(volume));

        if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                    IID_ISpStream, (void**)&stream)) || !stream) {
            break;
        }

        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = AcuVoice::OUTPUT_SAMPLE_RATE;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        const std::wstring path = g_outdir + L"\\" + file;
        if (FAILED(stream->BindToFile(path.c_str(), SPFM_CREATE_ALWAYS,
                                      &SPDFID_WaveFormatEx, &wf,
                                      SPFEI_ALL_EVENTS))) {
            wprintf(L"  !! could not open %s for writing\n", path.c_str());
            break;
        }
        if (FAILED(voice->SetOutput(stream, TRUE))) {
            break;
        }
        if (FAILED(voice->Speak(text.c_str(), SPF_DEFAULT, nullptr))) {
            wprintf(L"  !! Speak failed\n");
            break;
        }
        voice->WaitUntilDone(60000);
        ok = true;
    } while (false);

    if (stream) {
        stream->Close();
        stream->Release();
    }
    voice->Release();

    if (ok) {
        const std::wstring path = g_outdir + L"\\" + file;
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
            const double seconds =
                (info.nFileSizeLow > 44 ? (info.nFileSizeLow - 44) : 0) /
                (2.0 * AcuVoice::OUTPUT_SAMPLE_RATE);
            wprintf(L"  %-44s %7.2f s\n", file.c_str(), seconds);
            ok = seconds > 0.05;
            if (!ok) {
                wprintf(L"  !! that file holds no audio\n");
            }
        }
    }
    return ok;
}

// Opens one of our tokens by its full registry path. SAPI's voice category is rooted at
// HKEY_LOCAL_MACHINE, so tokens written under HKEY_CURRENT_USER never turn up in an
// enumeration -- but ISpObjectToken::SetId takes an explicit path, and SetVoice then
// builds the engine from the CLSID in that token exactly as it would for an installed
// voice. That makes the whole SAPI path testable before there is an installer to run.
ISpObjectToken* open_token_by_path(const std::wstring& id)
{
    ISpObjectToken* token = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                IID_ISpObjectToken, (void**)&token)) || !token) {
        return nullptr;
    }
    if (FAILED(token->SetId(nullptr, id.c_str(), FALSE))) {
        token->Release();
        return nullptr;
    }
    return token;
}

// Every AcuVoice token the machine has, collected in one pass. A machine with several
// speech engines on it can have several hundred voices, and enumerating the lot once per
// voice we are looking for turns a two-second check into a minute of setup sitting still.
std::vector<std::pair<std::wstring, ISpObjectToken*>> g_installed;
bool g_installed_scanned = false;

void scan_installed_tokens()
{
    if (g_installed_scanned) {
        return;
    }
    g_installed_scanned = true;

    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory, (void**)&cat)) || !cat) {
        return;
    }
    IEnumSpObjectTokens* en = nullptr;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) &&
        SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en) {
        ULONG count = 0;
        en->GetCount(&count);
        for (ULONG i = 0; i < count; ++i) {
            ISpObjectToken* tok = nullptr;
            if (FAILED(en->Item(i, &tok)) || !tok) {
                continue;
            }
            LPWSTR desc = nullptr;
            bool kept = false;
            if (SUCCEEDED(tok->GetStringValue(nullptr, &desc)) && desc) {
                if (wcsstr(desc, L"AcuVoice") != nullptr) {
                    g_installed.emplace_back(desc, tok);
                    kept = true;
                }
                CoTaskMemFree(desc);
            }
            if (!kept) {
                tok->Release();
            }
        }
        en->Release();
    }
    cat->Release();
}

ISpObjectToken* find_installed_token(const std::wstring& name)
{
    scan_installed_tokens();
    for (const auto& entry : g_installed) {
        if (entry.first == name) {
            entry.second->AddRef();
            return entry.second;
        }
    }
    return nullptr;
}

int write_sapi_samples()
{
    wprintf(L"SAPI5 samples in %s\n", g_outdir.c_str());

    int failures = 0;
    int spoken = 0;

    for (int i = 0; i < AcuVoice::sapi::total_token_count(); ++i) {
        const AcuVoice::sapi::voice_attributes v(i);
        const std::wstring name = v.get_name();

        const wchar_t* where = L"installed";
        ISpObjectToken* token = find_installed_token(name);
        if (!token) {
            token = open_token_by_path(
                std::wstring(L"HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Speech\\Voices"
                             L"\\Tokens\\") + v.get_token_id());
            where = L"per-user";
        }
        if (!token) {
            wprintf(L"  !! %s is not registered anywhere\n", name.c_str());
            ++failures;
            continue;
        }

        std::wstring safe = name;
        for (auto& c : safe) {
            if (c == L' ') c = L'_';
        }
        const std::wstring text =
            std::wstring(L"This is ") + name +
            L", speaking through the SAPI 5 interface. "
            L"The quick brown fox jumps over the lazy dog. "
            L"In 1998 the price rose 7 percent to $42.50.";

        wprintf(L"  [%-9s]", where);
        if (!speak_through_sapi(token, text, L"sapi_" + safe + L".wav", 0, 100)) {
            ++failures;
        }
        ++spoken;

        // Rate and volume through the host's own controls, on the first voice only --
        // proving a screen reader's own sliders reach the engine as well as the
        // utility's do.
        if (i == 0) {
            wprintf(L"  [%-9s]", where);
            speak_through_sapi(token,
                L"Host rate minus ten. The quick brown fox jumps over the lazy dog.",
                L"sapi_host_rate_minus10.wav", -10, 100);
            wprintf(L"  [%-9s]", where);
            speak_through_sapi(token,
                L"Host rate plus ten. The quick brown fox jumps over the lazy dog.",
                L"sapi_host_rate_plus10.wav", 10, 100);
            wprintf(L"  [%-9s]", where);
            speak_through_sapi(token,
                L"Host volume twenty five percent. The quick brown fox jumps over the lazy dog.",
                L"sapi_host_volume_25.wav", 0, 25);
            wprintf(L"  [%-9s]", where);
            speak_through_sapi(token,
                L"Host volume one hundred percent. The quick brown fox jumps over the lazy dog.",
                L"sapi_host_volume_100.wav", 0, 100);
        }
        token->Release();
    }

    wprintf(L"  %d AcuVoice voices spoken through SAPI5, %d failed\n\n", spoken, failures);
    return (spoken == 0 || failures > 0) ? 1 : 0;
}

// -------------------------------------------------------------------------------

void measure_latency()
{
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    const std::string short_text =
        AcuVoice::text::to_engine_bytes(L"Menu bar, File, submenu.");
    const std::string long_text = AcuVoice::text::to_engine_bytes(
        L"AcuVoice was a concatenative text to speech engine built in San Jose, "
        L"California during the middle nineteen nineties, and acquired by Fonix in "
        L"nineteen ninety eight. Its sound bank stores real recorded speech.");

    std::vector<int16_t> pcm;
    AcuVoice::dsp::params p;

    // Warm the engine first: the very first utterance also pays for opening the
    // sound bank, which is not what a keystroke costs.
    render(short_text, p, DEFAULT_PAUSES, false, pcm);

    wprintf(L"Latency (a screen reader's keystroke is the short one)\n");
    for (int i = 0; i < 3; ++i) {
        QueryPerformanceCounter(&t0);
        render(short_text, p, DEFAULT_PAUSES, false, pcm);
        QueryPerformanceCounter(&t1);
        wprintf(L"  short utterance  %6.2f ms  for %6.2f s of audio\n",
                (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
                pcm.size() / static_cast<double>(AcuVoice::OUTPUT_SAMPLE_RATE));
    }
    QueryPerformanceCounter(&t0);
    render(long_text, p, DEFAULT_PAUSES, false, pcm);
    QueryPerformanceCounter(&t1);
    wprintf(L"  long  utterance  %6.2f ms  for %6.2f s of audio\n",
            (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
            pcm.size() / static_cast<double>(AcuVoice::OUTPUT_SAMPLE_RATE));

    p.duration = AcuVoice::speed_to_duration(300);
    p.pitch = AcuVoice::pitch_to_factor(80);
    p.gain = 1.3;
    QueryPerformanceCounter(&t0);
    render(long_text, p, DEFAULT_PAUSES, false, pcm);
    QueryPerformanceCounter(&t1);
    wprintf(L"  long, with rate, pitch and volume applied  %6.2f ms\n\n",
            (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
}

void say(const std::wstring& text)
{
    std::vector<int16_t> pcm;
    AcuVoice::dsp::params p;
    if (!render(AcuVoice::text::to_engine_bytes(text), p, DEFAULT_PAUSES, false, pcm) ||
        pcm.empty()) {
        wprintf(L"nothing to play\n");
        return;
    }
    const std::wstring path = g_outdir + L"\\acuvoice_say.wav";
    write_wav(L"acuvoice_say.wav", pcm, AcuVoice::OUTPUT_SAMPLE_RATE);
    PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_SYNC);
    DeleteFileW(path.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    _wsetlocale(0, L"");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring mode = (argc > 1) ? argv[1] : L"report";

    // The installer runs "selftest" while its progress bar is still on screen, so a
    // broken install is reported there and not, later, as a screen reader that has gone
    // quiet. Everything it prints goes to a file beside the engine's own log, because
    // there is no console to print to when setup runs it hidden.
    if (mode == L"selftest") {
        wchar_t base[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
            GetTempPathW(MAX_PATH, base);
        }
        const std::wstring dir = std::wstring(base) + L"\\AcuVoice SAPI5";
        ensure_dir(dir);
        g_outdir = dir + L"\\install-check";
        ensure_dir(g_outdir);
        FILE* redirected = nullptr;
        _wfreopen_s(&redirected, (dir + L"\\install-check.txt").c_str(), L"w", stdout);
    } else if (mode == L"say") {
        // argv[2] is the sentence, not a directory; the wave file goes somewhere
        // writable and is deleted on the way out.
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        g_outdir = temp;
        if (!g_outdir.empty() && g_outdir.back() == L'\\') {
            g_outdir.pop_back();
        }
    } else if (argc > 2) {
        g_outdir = argv[2];
        ensure_dir(g_outdir);
    }

    wprintf(L"AcuVoice diagnostics, %d-bit build\n\n", static_cast<int>(sizeof(void*) * 8));

    int rc = 0;
    if (mode == L"say") {
        say(argc > 2 ? argv[2] : L"AcuVoice is working.");
    } else if (mode == L"samples") {
        report_engine();
        write_engine_samples();
        measure_latency();
    } else if (mode == L"sapi") {
        report_registered_voices();
        rc = write_sapi_samples();
    } else if (mode == L"all") {
        report_engine();
        report_registered_voices();
        write_engine_samples();
        rc = write_sapi_samples();
        measure_latency();
    } else if (mode == L"selftest") {
        report_engine();
        report_registered_voices();
        rc = write_sapi_samples();
        measure_latency();
        wprintf(L"\n%s\n", rc == 0
            ? L"RESULT: the AcuVoice engine loaded and every registered voice spoke."
            : L"RESULT: something is wrong -- see above. The engine, the voice "
              L"registration or the sound bank did not come up.");
        fflush(stdout);
    } else {
        report_engine();
        report_registered_voices();
        measure_latency();
    }

    CoUninitialize();
    return rc;
}
