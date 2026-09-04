// AcuVoiceConfig.exe -- the AcuVoice speech configuration utility.
//
// Everything the engine can be told to do is here, on one keyboard-reachable window:
// the AcuVoice Custom Voice's rate, pitch and volume, the four lengths of silence the
// engine inserts, the switches that apply to every voice, and a test button.
//
// Accessibility is the point of this program, not a coat of paint on it. Every control
// is a tab stop with an Alt shortcut and a static label directly before it in the
// dialog's control order, which is where MSAA takes a control's name from. Every slider
// runs 0 to 100 so that what a screen reader announces -- a trackbar is reported as a
// percentage of its range -- is the number on the slider, and so that 0 always means
// the least the engine will do and 100 always the most.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <string>
#include <vector>

#include "acuvoice_config_res.h"
#include "av_engine.h"
#include "av_dsp.h"
#include "av_paths.hpp"
#include "settings.hpp"
#include "text_prep.hpp"
#include "voices.hpp"
#include "debug_log.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

using namespace AcuVoice;

namespace {

HINSTANCE g_instance = nullptr;
HWND g_dialog = nullptr;

// Set while the dialog is being filled in from the saved settings. A trackbar or a
// checkbox fires its notification while the dialog is still being created -- before
// WM_INITDIALOG has even run -- so this starts true and is only cleared once the window
// is genuinely ready. Without it the first paint saves whatever uninitialized controls
// happened to hold.
bool g_loading = true;

engine g_engine;
std::vector<char> g_test_wav;

struct slider_spec {
    int control;
    int lo;
    int hi;
};

// Each slider's 0..100 maps onto the engine's own scale for that parameter.
constexpr slider_spec SLIDERS[] = {
    { IDC_RATE,    SPEED_MIN,  SPEED_MAX  },
    { IDC_PITCH,   PITCH_MIN,  PITCH_MAX  },
    { IDC_VOLUME,  VOLUME_MIN, VOLUME_MAX },
    { IDC_PAUSE1,  0,          PAUSE_MAX[0] },
    { IDC_PAUSE2,  0,          PAUSE_MAX[1] },
    { IDC_PAUSE3,  0,          PAUSE_MAX[2] },
    { IDC_PAUSE4,  0,          PAUSE_MAX[3] },
    { IDC_GRATE,   0,          100 },
    { IDC_GVOLUME, 0,          100 },
};

void set_slider(HWND dlg, int control, int percent)
{
    SendDlgItemMessageW(dlg, control, TBM_SETPOS, TRUE, std::clamp(percent, 0, 100));
}

[[nodiscard]] int get_slider(HWND dlg, int control)
{
    return static_cast<int>(SendDlgItemMessageW(dlg, control, TBM_GETPOS, 0, 0));
}

void init_sliders(HWND dlg)
{
    for (const slider_spec& s : SLIDERS) {
        SendDlgItemMessageW(dlg, s.control, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(dlg, s.control, TBM_SETTICFREQ, 10, 0);
        SendDlgItemMessageW(dlg, s.control, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(dlg, s.control, TBM_SETLINESIZE, 0, 1);
    }
}

// The global rate and volume multipliers are not on the engine's own scales, so they
// get their own mapping: the midpoint leaves the voice exactly as it is, which is the
// value a user is most likely to want to return to.
[[nodiscard]] int rate_percent_to_slider(int percent)
{
    // 25 % .. 100 % .. 400 %, logarithmic so 50 on the slider is exactly unchanged.
    const double t = std::log(std::clamp(percent, 25, 400) / 100.0) / std::log(4.0);
    return static_cast<int>(std::lround((t + 1.0) * 50.0));
}

[[nodiscard]] int slider_to_rate_percent(int slider)
{
    const double t = std::clamp(slider, 0, 100) / 50.0 - 1.0;
    return static_cast<int>(std::lround(100.0 * std::pow(4.0, t)));
}

[[nodiscard]] int volume_percent_to_slider(int percent)
{
    return std::clamp(percent, 0, 200) / 2;
}

[[nodiscard]] int slider_to_volume_percent(int slider)
{
    return std::clamp(slider, 0, 100) * 2;
}

// -----------------------------------------------------------------------------------

void load_into_dialog(HWND dlg)
{
    g_loading = true;

    const settings::voice_params v = settings::load_custom();
    const settings::global_settings g = settings::load_global();

    set_slider(dlg, IDC_RATE, range_to_percent(v.speed, SPEED_MIN, SPEED_MAX));
    set_slider(dlg, IDC_PITCH, range_to_percent(v.pitch, PITCH_MIN, PITCH_MAX));
    set_slider(dlg, IDC_VOLUME, range_to_percent(v.volume, VOLUME_MIN, VOLUME_MAX));
    set_slider(dlg, IDC_PAUSE1, range_to_percent(v.pause[0], 0, PAUSE_MAX[0]));
    set_slider(dlg, IDC_PAUSE2, range_to_percent(v.pause[1], 0, PAUSE_MAX[1]));
    set_slider(dlg, IDC_PAUSE3, range_to_percent(v.pause[2], 0, PAUSE_MAX[2]));
    set_slider(dlg, IDC_PAUSE4, range_to_percent(v.pause[3], 0, PAUSE_MAX[3]));

    set_slider(dlg, IDC_GRATE, rate_percent_to_slider(g.rate_percent));
    set_slider(dlg, IDC_GVOLUME, volume_percent_to_slider(g.volume_percent));

    CheckDlgButton(dlg, IDC_GLOBAL_PAUSES, g.override_pauses ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PUNCTUATION,
                   (v.speak_punctuation || g.speak_punctuation) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TAGS,
                   (v.honour_tags || g.honour_tags) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_LOGGING,
                   settings::read_dword(settings::ROOT_KEY, L"Logging", 1) ? BST_CHECKED
                                                                           : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_USERDICT_ENABLE,
                   read_ini().custom_dictionary ? BST_CHECKED : BST_UNCHECKED);

    g_loading = false;
}

void save_from_dialog(HWND dlg, bool announce)
{
    if (g_loading) {
        return;
    }

    settings::voice_params v;
    v.speed = percent_to_range(get_slider(dlg, IDC_RATE), SPEED_MIN, SPEED_MAX);
    v.pitch = percent_to_range(get_slider(dlg, IDC_PITCH), PITCH_MIN, PITCH_MAX);
    v.volume = percent_to_range(get_slider(dlg, IDC_VOLUME), VOLUME_MIN, VOLUME_MAX);
    v.pause[0] = percent_to_range(get_slider(dlg, IDC_PAUSE1), 0, PAUSE_MAX[0]);
    v.pause[1] = percent_to_range(get_slider(dlg, IDC_PAUSE2), 0, PAUSE_MAX[1]);
    v.pause[2] = percent_to_range(get_slider(dlg, IDC_PAUSE3), 0, PAUSE_MAX[2]);
    v.pause[3] = percent_to_range(get_slider(dlg, IDC_PAUSE4), 0, PAUSE_MAX[3]);
    v.honour_tags = IsDlgButtonChecked(dlg, IDC_TAGS) == BST_CHECKED;
    v.speak_punctuation = IsDlgButtonChecked(dlg, IDC_PUNCTUATION) == BST_CHECKED;

    settings::global_settings g;
    g.rate_percent = slider_to_rate_percent(get_slider(dlg, IDC_GRATE));
    g.volume_percent = slider_to_volume_percent(get_slider(dlg, IDC_GVOLUME));
    g.override_pauses = IsDlgButtonChecked(dlg, IDC_GLOBAL_PAUSES) == BST_CHECKED;
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        g.pause[i] = v.pause[i];
    }
    g.honour_tags = v.honour_tags;
    g.speak_punctuation = v.speak_punctuation;

    const bool ok = settings::save_custom(v) && settings::save_global(g);
    settings::write_dword(settings::ROOT_KEY, L"Logging",
                          IsDlgButtonChecked(dlg, IDC_LOGGING) == BST_CHECKED ? 1 : 0);

    // The custom dictionary switch is the engine's own, and it lives in the ini rather
    // than the registry because that is the only place the engine reads it from.
    ini_config cfg = read_ini();
    const bool want_dict = IsDlgButtonChecked(dlg, IDC_USERDICT_ENABLE) == BST_CHECKED;
    bool ini_ok = true;
    if (cfg.custom_dictionary != want_dict) {
        cfg.custom_dictionary = want_dict;
        ini_ok = write_ini(cfg);
    }

    if (announce) {
        std::wstring message = ok
            ? L"Settings saved. They take effect on the next thing any program speaks."
            : L"Settings could not be saved to the registry.";
        if (!ini_ok) {
            message += L"\n\nThe custom dictionary switch could not be written to "
                       L"acuvoice.ini in the Windows directory. Run this utility as an "
                       L"administrator to change it.";
        }
        MessageBoxW(dlg, message.c_str(), L"AcuVoice Speech Configuration",
                    MB_OK | (ok && ini_ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
}

void reset_to_defaults(HWND dlg)
{
    g_loading = true;
    set_slider(dlg, IDC_RATE, range_to_percent(SPEED_DEFAULT, SPEED_MIN, SPEED_MAX));
    set_slider(dlg, IDC_PITCH, range_to_percent(PITCH_DEFAULT, PITCH_MIN, PITCH_MAX));
    set_slider(dlg, IDC_VOLUME, range_to_percent(VOLUME_DEFAULT, VOLUME_MIN, VOLUME_MAX));
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        set_slider(dlg, IDC_PAUSE1 + i * 2, range_to_percent(PAUSE_DEFAULT[i], 0, PAUSE_MAX[i]));
    }
    set_slider(dlg, IDC_GRATE, rate_percent_to_slider(100));
    set_slider(dlg, IDC_GVOLUME, volume_percent_to_slider(100));
    CheckDlgButton(dlg, IDC_GLOBAL_PAUSES, BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PUNCTUATION, BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TAGS, BST_UNCHECKED);
    g_loading = false;
    SetDlgItemTextW(dlg, IDC_TESTTEXT,
                    L"AcuVoice is back to the settings the engine was shipped with.");
    SetFocus(GetDlgItem(dlg, IDC_RATE));
}

void copy_from_preset(HWND dlg)
{
    const LRESULT sel = SendDlgItemMessageW(dlg, IDC_COPYFROM, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        return;
    }
    const int index = static_cast<int>(
        SendDlgItemMessageW(dlg, IDC_COPYFROM, CB_GETITEMDATA, sel, 0));
    if (index < 0 || index >= PRESET_COUNT) {
        return;
    }
    const settings::voice_params v = settings::preset_params(index);

    g_loading = true;
    set_slider(dlg, IDC_RATE, range_to_percent(v.speed, SPEED_MIN, SPEED_MAX));
    set_slider(dlg, IDC_PITCH, range_to_percent(v.pitch, PITCH_MIN, PITCH_MAX));
    set_slider(dlg, IDC_VOLUME, range_to_percent(v.volume, VOLUME_MIN, VOLUME_MAX));
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        set_slider(dlg, IDC_PAUSE1 + i * 2, range_to_percent(v.pause[i], 0, PAUSE_MAX[i]));
    }
    g_loading = false;

    std::wstring note = std::wstring(L"The sliders now hold the settings of ") +
                        PRESETS[index].name + L".";
    SetDlgItemTextW(dlg, IDC_TESTTEXT, note.c_str());
    SetFocus(GetDlgItem(dlg, IDC_RATE));
}

// -----------------------------------------------------------------------------------

void build_wav(const std::vector<int16_t>& pcm, int rate, std::vector<char>& out)
{
    const uint32_t datasz = static_cast<uint32_t>(pcm.size() * 2);
    out.assign(44 + datasz, 0);
    char* p = out.data();
    const auto put = [&p](const void* d, size_t n) { memcpy(p, d, n); p += n; };
    const uint32_t riff = 36 + datasz, fmtsz = 16, sr = static_cast<uint32_t>(rate);
    const uint16_t tag = 1, ch = 1, bits = 16, align = 2;
    const uint32_t avg = sr * align;
    put("RIFF", 4); put(&riff, 4); put("WAVE", 4);
    put("fmt ", 4); put(&fmtsz, 4);
    put(&tag, 2); put(&ch, 2); put(&sr, 4); put(&avg, 4); put(&align, 2); put(&bits, 2);
    put("data", 4); put(&datasz, 4);
    if (datasz) {
        put(pcm.data(), datasz);
    }
}

void speak_test(HWND dlg)
{
    wchar_t buffer[512] = {};
    GetDlgItemTextW(dlg, IDC_TESTTEXT, buffer, 512);
    std::wstring text = buffer;
    if (text.empty()) {
        text = L"The quick brown fox jumps over the lazy dog.";
        SetDlgItemTextW(dlg, IDC_TESTTEXT, text.c_str());
    }

    if (!g_engine.loaded() && !g_engine.load(avcore_path())) {
        MessageBoxW(dlg,
                    L"The AcuVoice engine could not be loaded, so there is nothing to "
                    L"listen to. The About box below says where it was looked for.",
                    L"AcuVoice Speech Configuration", MB_OK | MB_ICONERROR);
        return;
    }

    // Exactly the numbers the SAPI engine would compute for this voice, so the test
    // button is a test of what a screen reader will hear and not of something adjacent.
    const int speed = percent_to_range(get_slider(dlg, IDC_RATE), SPEED_MIN, SPEED_MAX);
    const int pitch = percent_to_range(get_slider(dlg, IDC_PITCH), PITCH_MIN, PITCH_MAX);
    const int volume = percent_to_range(get_slider(dlg, IDC_VOLUME), VOLUME_MIN, VOLUME_MAX);
    const int grate = slider_to_rate_percent(get_slider(dlg, IDC_GRATE));
    const int gvol = slider_to_volume_percent(get_slider(dlg, IDC_GVOLUME));
    const bool tags = IsDlgButtonChecked(dlg, IDC_TAGS) == BST_CHECKED;
    const bool punct = IsDlgButtonChecked(dlg, IDC_PUNCTUATION) == BST_CHECKED;

    for (int i = 0; i < PAUSE_COUNT; ++i) {
        g_engine.set_pause(i + 1,
            percent_to_range(get_slider(dlg, IDC_PAUSE1 + i * 2), 0, PAUSE_MAX[i]));
    }

    std::wstring prepared = text::normalize(text, tags);
    if (punct) {
        prepared = text::name_punctuation(prepared);
    }
    const std::string bytes = text::to_engine_bytes(prepared);

    std::vector<unsigned char> ulaw;
    (void)g_engine.speak_all(bytes.c_str(), tags, ulaw);
    if (ulaw.empty()) {
        MessageBoxW(dlg, L"The engine found nothing to say in that sentence.",
                    L"AcuVoice Speech Configuration", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<int16_t> pcm;
    dsp::ulaw_to_pcm16(ulaw.data(), ulaw.size(), pcm);

    dsp::params p;
    p.duration = speed_to_duration(speed) / (grate / 100.0);
    p.pitch = pitch_to_factor(pitch);
    p.gain = volume_to_gain(volume) * (gvol / 100.0);

    std::vector<int16_t> out;
    dsp::render(pcm, p, out);

    build_wav(out, OUTPUT_SAMPLE_RATE, g_test_wav);
    PlaySoundW(nullptr, nullptr, SND_PURGE);
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_test_wav.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void fill_about(HWND dlg)
{
    const ini_config cfg = read_ini();
    std::wstring text;

    if (!g_engine.loaded()) {
        (void)g_engine.load(avcore_path());
    }

    text += L"AcuVoice speech engine, version ";
    {
        const std::string v = g_engine.loaded() ? g_engine.version() : std::string("not loaded");
        text += std::wstring(v.begin(), v.end());
    }
    text += L".  One recorded voice, Roger, in American English -- the only voice and the "
            L"only language this engine has.\r\n";
    text += L"Engine output: 8000 Hz 8-bit mu-law; this wrapper hands SAPI 16000 Hz "
            L"16-bit mono.\r\n";
    text += L"Engine files: " + avcore_path() + L"\r\n";
    text += L"Sound bank: " + cfg.sound_bank + L"\r\n";
    text += L"Dictionary: " + cfg.dict_dir + L"\r\n";
    text += L"Settings file: " + ini_path() + L"\r\n";
    text += L"Rate 85 to 350 words a minute, pitch 45 to 91 (six semitones either way), "
            L"volume 0 to 65535.\r\n";
    text += L"The engine core parses its own rate, pitch and volume tags but never acts "
            L"on them; this wrapper applies all three.";

    SetDlgItemTextW(dlg, IDC_INFO, text.c_str());
}

void open_log_folder(HWND dlg)
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
        return;
    }
    const std::wstring dir = std::wstring(base) + L"\\AcuVoice SAPI5";
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(dlg, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ----------------------------------------------------------------------------------
// Scrolling.
//
// The window is taller than the work area of a 1366x768 laptop. Rather than cut the
// controls down until they fit the smallest screen anyone might have, the dialog is left
// whole and scrolls: a sighted user gets a scroll bar and the wheel, and a keyboard user
// gets whatever they tab to brought into view, which is checked on a timer because a
// trackbar does not report a focus change to its parent.
// ----------------------------------------------------------------------------------

int g_content_height = 0;   // the dialog's full height in pixels
int g_scroll_pos = 0;
constexpr UINT_PTR FOCUS_TIMER = 1;

void update_scroll_info(HWND dlg)
{
    RECT client;
    GetClientRect(dlg, &client);
    const int page = client.bottom - client.top;

    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (g_content_height > 0) ? g_content_height - 1 : 0;
    si.nPage = static_cast<UINT>(page);
    si.nPos = g_scroll_pos;
    SetScrollInfo(dlg, SB_VERT, &si, TRUE);
}

void scroll_to(HWND dlg, int target)
{
    RECT client;
    GetClientRect(dlg, &client);
    const int page = client.bottom - client.top;
    const int limit = (g_content_height > page) ? (g_content_height - page) : 0;

    target = std::clamp(target, 0, limit);
    const int delta = g_scroll_pos - target;
    if (delta == 0) {
        return;
    }
    g_scroll_pos = target;
    ScrollWindowEx(dlg, 0, delta, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    update_scroll_info(dlg);
    UpdateWindow(dlg);
}

// Brings the focused control fully into view. Called from a timer: a trackbar, unlike an
// edit box, sends its parent nothing when it gains focus, and subclassing eleven controls
// to find out would be a lot of machinery for something a 200 ms poll settles.
void follow_focus(HWND dlg)
{
    const HWND focus = GetFocus();
    if (!focus || GetParent(focus) != dlg) {
        return;
    }
    RECT control, client;
    GetWindowRect(focus, &control);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&control), 2);
    GetClientRect(dlg, &client);

    // Include the label above the control, which is what names it.
    const int margin = 16;
    if (control.top - margin < client.top) {
        scroll_to(dlg, g_scroll_pos + (control.top - margin - client.top));
    } else if (control.bottom + 4 > client.bottom) {
        scroll_to(dlg, g_scroll_pos + (control.bottom + 4 - client.bottom));
    }
}

// Shrinks the window to what the screen can actually show, and remembers how tall the
// content really is so the scroll range is right.
void fit_to_screen(HWND dlg)
{
    RECT window, client;
    GetWindowRect(dlg, &window);
    GetClientRect(dlg, &client);
    g_content_height = client.bottom - client.top;

    HMONITOR monitor = MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const int available = info.rcWork.bottom - info.rcWork.top;
    const int height = window.bottom - window.top;
    if (height <= available) {
        update_scroll_info(dlg);
        return;
    }

    const int chrome = height - (client.bottom - client.top);
    const int new_height = available - 8;
    SetWindowPos(dlg, nullptr,
                 window.left,
                 info.rcWork.top + 4,
                 window.right - window.left,
                 new_height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    (void)chrome;
    update_scroll_info(dlg);
}

void open_user_dictionary(HWND dlg)
{
    const std::wstring exe = install_root() + L"engine\\UserDict\\Userdict.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(dlg,
                    L"The AcuVoice user dictionary editor is not installed.",
                    L"AcuVoice Speech Configuration", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring dir = install_root() + L"engine\\UserDict";
    const HINSTANCE rc = ShellExecuteW(dlg, L"open", exe.c_str(), nullptr,
                                       dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        MessageBoxW(dlg, L"The user dictionary editor could not be started.",
                    L"AcuVoice Speech Configuration", MB_OK | MB_ICONWARNING);
    }
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
        case WM_INITDIALOG: {
            g_dialog = dlg;
            init_sliders(dlg);

            for (int i = 0; i < PRESET_COUNT; ++i) {
                const LRESULT item = SendDlgItemMessageW(
                    dlg, IDC_COPYFROM, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(PRESETS[i].name));
                SendDlgItemMessageW(dlg, IDC_COPYFROM, CB_SETITEMDATA, item, i);
            }
            SendDlgItemMessageW(dlg, IDC_COPYFROM, CB_SETCURSEL, 0, 0);

            SetDlgItemTextW(dlg, IDC_TESTTEXT,
                            L"The quick brown fox jumps over the lazy dog.");
            load_into_dialog(dlg);
            fill_about(dlg);
            fit_to_screen(dlg);
            SetTimer(dlg, FOCUS_TIMER, 200, nullptr);

            SetFocus(GetDlgItem(dlg, IDC_RATE));
            return FALSE;   // focus was set by hand
        }

        case WM_VSCROLL: {
            // A trackbar sends WM_HSCROLL, so anything arriving here with a control
            // handle is not one of ours; the scroll bar sends it with lParam zero.
            if (lparam != 0) {
                break;
            }
            SCROLLINFO si = { sizeof(si) };
            si.fMask = SIF_ALL;
            GetScrollInfo(dlg, SB_VERT, &si);
            int pos = g_scroll_pos;
            switch (LOWORD(wparam)) {
                case SB_LINEUP:    pos -= 20; break;
                case SB_LINEDOWN:  pos += 20; break;
                case SB_PAGEUP:    pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN:  pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP:       pos = 0; break;
                case SB_BOTTOM:    pos = si.nMax; break;
                default: return TRUE;
            }
            scroll_to(dlg, pos);
            return TRUE;
        }

        case WM_MOUSEWHEEL:
            scroll_to(dlg, g_scroll_pos - GET_WHEEL_DELTA_WPARAM(wparam) / 3);
            return TRUE;

        case WM_SIZE:
            update_scroll_info(dlg);
            return FALSE;

        case WM_TIMER:
            if (wparam == FOCUS_TIMER) {
                follow_focus(dlg);
            }
            return TRUE;

        case WM_COMMAND:
            // Controls notify while the dialog is still being built, before
            // WM_INITDIALOG has run. Acting on those would save whatever the
            // uninitialized controls happened to hold.
            if (g_loading && LOWORD(wparam) != IDCANCEL) {
                return TRUE;
            }
            switch (LOWORD(wparam)) {
                case IDC_SAVE:
                    save_from_dialog(dlg, true);
                    return TRUE;
                case IDC_DEFAULTS:
                    reset_to_defaults(dlg);
                    return TRUE;
                case IDC_COPYFROM_APPLY:
                    copy_from_preset(dlg);
                    return TRUE;
                case IDC_SPEAK:
                    speak_test(dlg);
                    return TRUE;
                case IDC_USERDICT:
                    open_user_dictionary(dlg);
                    return TRUE;
                case IDC_OPENLOG:
                    open_log_folder(dlg);
                    return TRUE;
                case IDCANCEL:
                    KillTimer(dlg, FOCUS_TIMER);
                    PlaySoundW(nullptr, nullptr, SND_PURGE);
                    EndDialog(dlg, 0);
                    return TRUE;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            KillTimer(dlg, FOCUS_TIMER);
            PlaySoundW(nullptr, nullptr, SND_PURGE);
            EndDialog(dlg, 0);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR command_line, int)
{
    g_instance = instance;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // A command line of /reset puts everything back without opening the window, for a
    // user who has made the voice unintelligible and cannot read the screen to fix it.
    if (command_line && wcsstr(command_line, L"/reset")) {
        settings::save_custom(settings::voice_params{});
        settings::save_global(settings::global_settings{});
        return 0;
    }

    DEBUG_LOG("configuration utility started");
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr, dialog_proc, 0);
    PlaySoundW(nullptr, nullptr, SND_PURGE);
    return 0;
}
