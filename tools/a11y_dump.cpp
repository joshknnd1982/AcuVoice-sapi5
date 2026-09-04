// a11y_dump.exe -- what a screen reader actually sees in the configuration utility.
//
// This walks the window with MSAA (oleacc), not UI Automation: a PowerShell UIA client
// reports every classic Win32 control as a generic Pane, which tells you nothing about
// whether a control is named. oleacc is the interface NVDA and JAWS fall back to for a
// plain dialog, so it is the one worth checking.
//
//   a11y_dump "AcuVoice Speech Configuration"
//
// Prints, in tab order, every control's name, role, keyboard shortcut and value, and
// flags anything unnamed or unreachable.

#include <windows.h>
#include <oleacc.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "user32.lib")

namespace {

struct found {
    HWND window = nullptr;
    const wchar_t* title = nullptr;
};

BOOL CALLBACK find_window(HWND hwnd, LPARAM param)
{
    auto* f = reinterpret_cast<found*>(param);
    wchar_t text[256] = {};
    GetWindowTextW(hwnd, text, 256);
    if (wcsstr(text, f->title) && IsWindowVisible(hwnd)) {
        f->window = hwnd;
        return FALSE;
    }
    return TRUE;
}

// The out parameter has to be filled in before it is read, which rules out writing this
// as bstr_of(acc->get_accName(self, &b), b): MSVC evaluates function arguments
// right-to-left, so `b` would be read -- still null -- before the call that fills it.
using bstr_getter = HRESULT(STDMETHODCALLTYPE IAccessible::*)(VARIANT, BSTR*);

std::wstring ask(IAccessible* acc, bstr_getter method, VARIANT self)
{
    BSTR b = nullptr;
    const HRESULT hr = (acc->*method)(self, &b);
    std::wstring out = (SUCCEEDED(hr) && b) ? b : L"";
    if (b) {
        SysFreeString(b);
    }
    return out;
}

const wchar_t* role_name(long role)
{
    static wchar_t buf[128];
    if (GetRoleTextW(static_cast<DWORD>(role), buf, 128)) {
        return buf;
    }
    return L"?";
}

std::wstring state_names(long state)
{
    std::wstring out;
    const struct { long bit; const wchar_t* name; } bits[] = {
        { STATE_SYSTEM_UNAVAILABLE, L"disabled" },
        { STATE_SYSTEM_FOCUSED, L"focused" },
        { STATE_SYSTEM_FOCUSABLE, L"focusable" },
        { STATE_SYSTEM_INVISIBLE, L"invisible" },
        { STATE_SYSTEM_CHECKED, L"checked" },
        { STATE_SYSTEM_READONLY, L"readonly" },
        { STATE_SYSTEM_OFFSCREEN, L"offscreen" },
    };
    for (const auto& b : bits) {
        if (state & b.bit) {
            if (!out.empty()) out += L",";
            out += b.name;
        }
    }
    return out.empty() ? L"-" : out;
}

int g_unnamed = 0;
int g_controls = 0;

void describe(HWND hwnd, int tab_index)
{
    // OBJID_CLIENT is the control's client area; for some control classes the name
    // lives on the window object instead, so both are asked and the first answer wins.
    // This is the same pair Inspect.exe shows for a classic Win32 dialog.
    IAccessible* acc = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
                                            reinterpret_cast<void**>(&acc));
    if (FAILED(hr) || !acc) {
        hr = AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, IID_IAccessible,
                                        reinterpret_cast<void**>(&acc));
    }
    if (FAILED(hr) || !acc) {
        wprintf(L"  %2d  !! no accessible object (0x%08lX)\n", tab_index, hr);
        return;
    }

    VARIANT self;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;

    std::wstring name = ask(acc, &IAccessible::get_accName, self);
    if (name.empty()) {
        IAccessible* win = nullptr;
        if (SUCCEEDED(AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, IID_IAccessible,
                                                 reinterpret_cast<void**>(&win))) && win) {
            name = ask(win, &IAccessible::get_accName, self);
            win->Release();
        }
    }
    const std::wstring value = ask(acc, &IAccessible::get_accValue, self);
    const std::wstring shortcut = ask(acc, &IAccessible::get_accKeyboardShortcut, self);

    VARIANT role = {}, state = {};
    acc->get_accRole(self, &role);
    acc->get_accState(self, &state);

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);

    const bool tabstop = (GetWindowLongW(hwnd, GWL_STYLE) & WS_TABSTOP) != 0;
    const bool needs_name = tabstop;

    ++g_controls;
    if (needs_name && name.empty()) {
        ++g_unnamed;
    }

    wprintf(L"  %2d  %-16s %-14s tabstop=%s\n", tab_index, cls,
            role.vt == VT_I4 ? role_name(role.lVal) : L"?",
            tabstop ? L"yes" : L"no ");
    wprintf(L"      name     : %s%s\n",
            name.empty() ? L"(none)" : name.c_str(),
            (needs_name && name.empty()) ? L"   <-- UNNAMED TAB STOP" : L"");
    if (!value.empty()) {
        wprintf(L"      value    : %s\n", value.c_str());
    }
    if (!shortcut.empty()) {
        wprintf(L"      shortcut : %s\n", shortcut.c_str());
    }
    wprintf(L"      state    : %s\n", state.vt == VT_I4 ? state_names(state.lVal).c_str() : L"?");

    VariantClear(&role);
    VariantClear(&state);
    acc->Release();
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    _wsetlocale(0, L"");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t* title = (argc > 1) ? argv[1] : L"AcuVoice Speech Configuration";
    found f{ nullptr, title };
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&f));
    if (!f.window) {
        wprintf(L"No visible window whose title contains \"%s\".\n", title);
        CoUninitialize();
        return 1;
    }

    wchar_t text[256] = {};
    GetWindowTextW(f.window, text, 256);
    wprintf(L"Window: %s\n\n", text);
    wprintf(L"Controls in tab order, as MSAA reports them\n");

    // Walking with GetNextDlgTabItem is the point: it follows the same order the Tab key
    // does, so a control missing from this list is a control a keyboard user cannot get
    // to at all.
    HWND first = GetNextDlgTabItem(f.window, nullptr, FALSE);
    HWND current = first;
    int index = 0;
    std::vector<HWND> seen;
    while (current) {
        bool repeat = false;
        for (HWND h : seen) {
            if (h == current) {
                repeat = true;
                break;
            }
        }
        if (repeat) {
            break;
        }
        seen.push_back(current);
        describe(current, ++index);
        current = GetNextDlgTabItem(f.window, current, FALSE);
    }

    // Anything with WS_TABSTOP that the walk above never reached is unreachable.
    int missed = 0;
    HWND child = GetWindow(f.window, GW_CHILD);
    while (child) {
        if ((GetWindowLongW(child, GWL_STYLE) & (WS_TABSTOP | WS_VISIBLE)) ==
            (WS_TABSTOP | WS_VISIBLE)) {
            bool reached = false;
            for (HWND h : seen) {
                if (h == child) {
                    reached = true;
                    break;
                }
            }
            if (!reached) {
                wchar_t cls[64] = {};
                GetClassNameW(child, cls, 64);
                wprintf(L"  !! %s (id %d) has WS_TABSTOP but Tab never reaches it\n",
                        cls, GetDlgCtrlID(child));
                ++missed;
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }

    wprintf(L"\n%d controls in the tab order, %d unnamed, %d unreachable\n",
            g_controls, g_unnamed, missed);
    CoUninitialize();
    return (g_unnamed > 0 || missed > 0) ? 1 : 0;
}
