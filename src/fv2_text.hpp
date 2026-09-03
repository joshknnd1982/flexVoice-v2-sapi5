// fv2_text.hpp -- turning what a SAPI client hands us into what the engine
// will accept, while keeping track of where every byte came from.
//
// FlexVoice 2.0 needs much less defending than FlexVoice 3.01 did:
//
//   * Numerals are fine. 3.01 faulted or wedged on a bare digit, so its
//     wrapper had to spell every number out in words. 2.0 speaks "Chapter 5",
//     "1984" and "3.50" correctly, so they are left alone -- rewriting them
//     would make the engine *worse* at its job, and would move every word
//     boundary.
//   * There is no embedded-command syntax to escape. Both `[:rate 200]` and
//     `\rate 200\` are spoken aloud as text rather than interpreted, measured
//     by duration against the same sentence without them. So no client text
//     can be mistaken for a command, and nothing has to be neutralised.
//
// What is left is real but small: the engine takes a single-byte string, so
// wide text has to be converted, and SAPI's spell-out has to be expanded here
// rather than in the engine.
//
// The offset map is the reason this is not a one-line WideCharToMultiByte
// call. Word-boundary events report an offset into the bytes the engine was
// given, and a screen reader highlights the caller's original string, so every
// output byte records which source character produced it.

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace fv2 {
namespace text {

enum Mode {
    Prose,   // spoken as written
    Spell,   // one character at a time, punctuation named
};

struct Normalized {
    std::string           text;     // bytes for the engine
    std::vector<uint32_t> srcMap;   // one entry per byte: index into the source
};

// `len` is a character count, not a byte count, and never -1: passing -1 to
// WideCharToMultiByte makes it count the terminating NUL as part of the string
// and write it, which overruns a buffer sized from the returned count.
Normalized normalize(const wchar_t* text, uint32_t len, Mode mode);

// The spoken name of a punctuation character, or nullptr if it has none.
// Exposed for the tests.
const char* punctuation_name(wchar_t c);

}  // namespace text
}  // namespace fv2
