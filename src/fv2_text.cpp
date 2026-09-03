#include "fv2_text.hpp"

#include <windows.h>

namespace fv2 {
namespace text {

namespace {

// The engine is a 2001 Windows program and takes a single-byte string.
// CP1252 with best-fit mapping is the right target: it keeps the accented
// letters it can represent and folds the rest towards ASCII, so a curly
// apostrophe becomes a straight one and an em dash becomes a hyphen instead
// of vanishing.
const UINT kCodepage = 1252;

// A run of at most this many source characters is converted at a time. One
// character at a time would be correct but slow; a whole string at once gives
// no way to build the offset map. Converting per code point is the compromise,
// and it is what makes surrogate pairs work.
struct Appender {
    std::string& out;
    std::vector<uint32_t>& map;

    void put(const char* bytes, int n, uint32_t src)
    {
        for (int i = 0; i < n; ++i) {
            out.push_back(bytes[i]);
            map.push_back(src);
        }
    }
    void put(const char* s, uint32_t src)
    {
        for (const char* p = s; *p; ++p) {
            out.push_back(*p);
            map.push_back(src);
        }
    }
};

// Convert one code point (one or two wchar_t) to CP1252.
int convert(const wchar_t* w, int n, char* buf, int bufSize)
{
    // Explicit lengths throughout: -1 would make this count and emit the
    // terminating NUL, and a buffer sized from the returned count would then
    // be one byte short. That corrupts the heap somewhere else entirely.
    return WideCharToMultiByte(kCodepage, 0, w, n, buf, bufSize, nullptr, nullptr);
}

bool isSurrogatePair(const wchar_t* w, uint32_t i, uint32_t len)
{
    return w[i] >= 0xD800 && w[i] <= 0xDBFF && i + 1 < len &&
           w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF;
}

}  // namespace

const char* punctuation_name(wchar_t c)
{
    switch (c) {
    case L' ':  return "space";
    case L'!':  return "exclamation";
    case L'"':  return "quote";
    case L'#':  return "number";
    case L'$':  return "dollar";
    case L'%':  return "percent";
    case L'&':  return "and";
    case L'\'': return "apostrophe";
    case L'(':  return "left paren";
    case L')':  return "right paren";
    case L'*':  return "star";
    case L'+':  return "plus";
    case L',':  return "comma";
    case L'-':  return "dash";
    case L'.':  return "dot";
    case L'/':  return "slash";
    case L':':  return "colon";
    case L';':  return "semicolon";
    case L'<':  return "less than";
    case L'=':  return "equals";
    case L'>':  return "greater than";
    case L'?':  return "question";
    case L'@':  return "at";
    case L'[':  return "left bracket";
    case L'\\': return "backslash";
    case L']':  return "right bracket";
    case L'^':  return "caret";
    case L'_':  return "underscore";
    case L'`':  return "backtick";
    case L'{':  return "left brace";
    case L'|':  return "bar";
    case L'}':  return "right brace";
    case L'~':  return "tilde";
    default:    return nullptr;
    }
}

Normalized normalize(const wchar_t* text, uint32_t len, Mode mode)
{
    Normalized out;
    if (!text || len == 0) return out;

    out.text.reserve(len + 16);
    out.srcMap.reserve(len + 16);
    Appender app{ out.text, out.srcMap };

    char buf[16];

    for (uint32_t i = 0; i < len; ) {
        const wchar_t c = text[i];
        const uint32_t src = i;
        const int units = isSurrogatePair(text, i, len) ? 2 : 1;

        if (mode == Spell) {
            // SAPI produces SPVA_SpellOut for the <spell> tag, which is how a
            // screen reader's "spell this word" command arrives. Each item is
            // separated by a comma so the engine puts a real gap between them
            // rather than running the letters into one word.
            if (const char* name = punctuation_name(c)) {
                app.put(name, src);
            } else if (c == L'\r' || c == L'\n' || c == L'\t') {
                app.put("space", src);
            } else {
                const int n = convert(text + i, units, buf, (int)sizeof(buf));
                if (n > 0) app.put(buf, n, src);
            }
            app.put(", ", src);
            i += units;
            continue;
        }

        // Prose. Control characters would reach the engine as raw bytes; turn
        // the ones that carry meaning into a space and drop the rest.
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            app.put(" ", src);
            i += units;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            i += units;
            continue;
        }

        const int n = convert(text + i, units, buf, (int)sizeof(buf));
        if (n > 0) {
            app.put(buf, n, src);
        } else {
            // Nothing in CP1252 represents it, not even approximately. A space
            // keeps the words apart, which is better than gluing them together.
            app.put(" ", src);
        }
        i += units;
    }

    return out;
}

}  // namespace text
}  // namespace fv2
