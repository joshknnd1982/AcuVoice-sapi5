#pragma once

#include <string>
#include <vector>

// Turning what SAPI hands over into something avcore will read well.
//
// avcore is a single-byte engine with an English front end: it does its own number,
// date, currency and abbreviation expansion (its dictionary holds "thousand",
// "quadrillion", "o'clock", "Wednesday", the ordinals and a large suffix table), so the
// less this layer does to the text the better it sounds. What it does have to do is
// narrow UTF-16 down to the engine's codepage without dropping words on the floor, and
// keep control characters and stray backslashes from confusing the tag parser.

namespace AcuVoice {
namespace text {

// UTF-16 to Windows-1252, the codepage the engine's dictionary was built against.
// Characters with no representation are transliterated where there is an obvious answer
// (curly quotes, dashes, the common ligatures) and dropped otherwise, which is better
// than the '?' a plain conversion leaves behind for the engine to spell out.
[[nodiscard]] std::string to_engine_bytes(const std::wstring& text);

// Collapses control characters and runs of whitespace, and -- when tags are off --
// neutralizes anything that looks like a tag so a Windows path is read as a path.
[[nodiscard]] std::wstring normalize(const std::wstring& text, bool honour_tags);

// One character per word, so the engine names letters and digits rather than trying to
// pronounce them as a word.
[[nodiscard]] std::wstring spell_out(const std::wstring& text);

// Replaces punctuation with its name, for hosts that ask for "all punctuation".
[[nodiscard]] std::wstring name_punctuation(const std::wstring& text);

// Splits a long utterance at sentence ends so the first audio starts before the whole
// thing has been synthesized, and so a cancel lands within a sentence. Text under
// `soft_limit` characters comes back as a single piece.
[[nodiscard]] std::vector<std::wstring> split_for_synthesis(const std::wstring& text,
                                                            size_t soft_limit = 400);

}  // namespace text
}  // namespace AcuVoice
