#include "text_prep.hpp"

#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace AcuVoice {
namespace text {

namespace {

// Characters Windows-1252 has no room for but that turn up constantly in real text.
// Left as they are they become '?', which the engine spells out as "question mark".
struct fold_entry { wchar_t from; const wchar_t* to; };

constexpr fold_entry FOLDS[] = {
    { 0x2018, L"'" },  { 0x2019, L"'" },  { 0x201A, L"," },
    { 0x201C, L"\"" }, { 0x201D, L"\"" }, { 0x201E, L"\"" },
    { 0x2013, L"-" },  { 0x2014, L" - " },{ 0x2015, L" - " },
    { 0x2026, L"..." },{ 0x2032, L"'" },  { 0x2033, L"\"" },
    { 0x00A0, L" " },  { 0x2007, L" " },  { 0x202F, L" " },
    { 0x200B, L"" },   { 0x200E, L"" },   { 0x200F, L"" },  { 0xFEFF, L"" },
    { 0x2022, L" " },  { 0x00B7, L" " },  { 0x2010, L"-" }, { 0x2011, L"-" },
    { 0x00AB, L"\"" }, { 0x00BB, L"\"" },
    { 0x0152, L"OE" }, { 0x0153, L"oe" }, { 0x00C6, L"AE" }, { 0x00E6, L"ae" },
    { 0x00DF, L"ss" }, { 0x0178, L"Y" },  { 0x0160, L"S" },  { 0x0161, L"s" },
    { 0x017D, L"Z" },  { 0x017E, L"z" },
    { 0x20AC, L" euros " }, { 0x00A9, L" copyright " }, { 0x00AE, L" registered " },
    { 0x2122, L" trademark " }, { 0x00B0, L" degrees " },
    { 0x00BD, L" one half " }, { 0x00BC, L" one quarter " },
    { 0x00BE, L" three quarters " }, { 0x00D7, L" times " }, { 0x00F7, L" divided by " },
};

struct punct_entry { wchar_t ch; const wchar_t* name; };

constexpr punct_entry PUNCT[] = {
    { L'.', L" dot " },        { L',', L" comma " },        { L';', L" semicolon " },
    { L':', L" colon " },      { L'?', L" question mark " },{ L'!', L" exclamation " },
    { L'\'', L" apostrophe " },{ L'"', L" quote " },        { L'(', L" left paren " },
    { L')', L" right paren " },{ L'[', L" left bracket " }, { L']', L" right bracket " },
    { L'{', L" left brace " }, { L'}', L" right brace " },  { L'-', L" dash " },
    { L'_', L" underscore " }, { L'/', L" slash " },        { L'\\', L" backslash " },
    { L'@', L" at " },         { L'#', L" number " },       { L'$', L" dollar " },
    { L'%', L" percent " },    { L'^', L" caret " },        { L'&', L" and " },
    { L'*', L" star " },       { L'+', L" plus " },         { L'=', L" equals " },
    { L'<', L" less than " },  { L'>', L" greater than " }, { L'|', L" pipe " },
    { L'~', L" tilde " },      { L'`', L" backtick " },
};

[[nodiscard]] const wchar_t* fold_for(wchar_t c) noexcept
{
    for (const auto& f : FOLDS) {
        if (f.from == c) {
            return f.to;
        }
    }
    return nullptr;
}

}  // namespace

std::string to_engine_bytes(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    std::wstring folded;
    folded.reserve(text.size() + 16);
    for (const wchar_t c : text) {
        if (const wchar_t* replacement = fold_for(c)) {
            folded += replacement;
        } else {
            folded += c;
        }
    }

    // Windows-1252, with a space for anything still unmappable. A space is the one
    // substitution the engine says nothing about: '?' would be read aloud, and dropping
    // the character silently can run two words together.
    const int needed = WideCharToMultiByte(1252, 0, folded.c_str(),
                                           static_cast<int>(folded.size()),
                                           nullptr, 0, " ", nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(1252, 0, folded.c_str(), static_cast<int>(folded.size()),
                        out.data(), needed, " ", nullptr);
    return out;
}

std::wstring normalize(const std::wstring& text, bool honour_tags)
{
    std::wstring out;
    out.reserve(text.size());

    bool pending_space = false;
    for (const wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ' || c == L'\f' || c == L'\v') {
            pending_space = !out.empty();
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            continue;      // other control characters carry no speech
        }
        if (pending_space) {
            out += L' ';
            pending_space = false;
        }
        // A backslash the engine's tag parser cannot make sense of is harmless, but one
        // that happens to spell a real tag would swallow the text after it. With tags
        // off the parser is not running at all, so this only has to keep the character
        // itself from being confusing: the engine already reads it as "backslash".
        out += c;
    }

    if (!honour_tags) {
        return out;
    }

    // With tags on the text is the caller's to write, so it is left exactly as it is.
    return out;
}

std::wstring spell_out(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() * 2);
    for (const wchar_t c : text) {
        if (iswspace(c)) {
            continue;
        }
        if (!out.empty()) {
            out += L' ';
        }
        // A lone capital reads as the letter name; a lone lowercase letter can be read
        // as a word ("a", "i"), so every letter is uppercased for spelling.
        out += static_cast<wchar_t>(towupper(c));
    }
    return out;
}

std::wstring name_punctuation(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() * 3);
    for (const wchar_t c : text) {
        bool named = false;
        for (const auto& p : PUNCT) {
            if (p.ch == c) {
                out += p.name;
                named = true;
                break;
            }
        }
        if (!named) {
            out += c;
        }
    }
    return out;
}

std::vector<std::wstring> split_for_synthesis(const std::wstring& text, size_t soft_limit)
{
    std::vector<std::wstring> pieces;
    if (text.size() <= soft_limit) {
        if (!text.empty()) {
            pieces.push_back(text);
        }
        return pieces;
    }

    size_t start = 0;
    while (start < text.size()) {
        if (text.size() - start <= soft_limit) {
            pieces.push_back(text.substr(start));
            break;
        }

        // Look for a sentence end inside the window, then a clause end, then a space.
        // Splitting anywhere else would cut a word in half, and the engine would then
        // pronounce both halves as words of their own.
        const size_t window_end = start + soft_limit;
        size_t cut = std::wstring::npos;

        for (size_t i = window_end; i > start + soft_limit / 4; --i) {
            const wchar_t c = text[i - 1];
            if ((c == L'.' || c == L'!' || c == L'?') &&
                (i >= text.size() || iswspace(text[i]))) {
                cut = i;
                break;
            }
        }
        if (cut == std::wstring::npos) {
            for (size_t i = window_end; i > start + soft_limit / 4; --i) {
                const wchar_t c = text[i - 1];
                if (c == L';' || c == L':' || c == L',') {
                    cut = i;
                    break;
                }
            }
        }
        if (cut == std::wstring::npos) {
            cut = text.rfind(L' ', window_end);
            if (cut == std::wstring::npos || cut <= start) {
                cut = window_end;      // one very long token; it has to break somewhere
            }
        }

        std::wstring piece = text.substr(start, cut - start);
        // Trim, and drop a piece that came down to nothing but spaces.
        const size_t first = piece.find_first_not_of(L" ");
        if (first != std::wstring::npos) {
            const size_t last = piece.find_last_not_of(L" ");
            pieces.push_back(piece.substr(first, last - first + 1));
        }
        start = cut;
        while (start < text.size() && text[start] == L' ') {
            ++start;
        }
    }
    return pieces;
}

}  // namespace text
}  // namespace AcuVoice
