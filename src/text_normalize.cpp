#include "text_normalize.h"

#include <windows.h>
#include <cctype>
#include <cstring>

namespace FlexVoice {
namespace text {

namespace {

const char* const kOnes[] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"
};
const char* const kTeens[] = {
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"
};
const char* const kTens[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
};
const char* const kScales[] = { "", " thousand", " million", " billion", " trillion" };

// Letter names, used for acronyms and lone consonants. The three-letter forms
// for s, r and l are deliberate: the prior FlexVoice wrapper found "ess",
// "arr" and "ell" survive where "es" and "el" get dropped.
const char* const kLetterName[26] = {
    "ay", "bee", "see", "dee", "ee", "eff", "gee", "aitch", "eye", "jay", "kay",
    "ell", "em", "en", "oh", "pee", "cue", "arr", "ess", "tee", "you", "vee",
    "double you", "ex", "why", "zee"
};

std::string words_below_1000(unsigned v)
{
    std::string out;
    if (v >= 100) {
        out += kOnes[v / 100];
        out += " hundred";
        v %= 100;
        if (v) out += " ";
    }
    if (v >= 20) {
        out += kTens[v / 10];
        if (v % 10) { out += " "; out += kOnes[v % 10]; }
    } else if (v >= 10) {
        out += kTeens[v - 10];
    } else if (v > 0 || out.empty()) {
        out += kOnes[v];
    }
    return out;
}

bool is_vowel_like(char c)
{
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u' || l == 'y';
}

bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

// A single writer, so every rule appends through the same offset bookkeeping.
class Writer {
public:
    Writer(std::string& out, std::vector<uint32_t>* map) : out_(out), map_(map) {}

    void put(char c, uint32_t src)
    {
        out_ += c;
        if (map_) map_->push_back(src);
    }
    void put(const char* s, uint32_t src)
    {
        for (; *s; ++s) put(*s, src);
    }
    void put(const std::string& s, uint32_t src) { put(s.c_str(), src); }

    // Collapse runs of whitespace rather than emitting them: the engine turns a
    // hard line break into a long pause, which makes say-all lurch.
    void space(uint32_t src)
    {
        if (!out_.empty() && out_.back() != ' ') put(' ', src);
    }

    bool empty() const { return out_.empty(); }
    char back() const { return out_.empty() ? '\0' : out_.back(); }

private:
    std::string&           out_;
    std::vector<uint32_t>* map_;
};

// Naming every character is the whole job for spell-out, and it is also the
// fallback when an utterance would otherwise be silent -- which is what
// arrowing character by character produces, one punctuation mark at a time.
void spell_one(Writer& w, char c, uint32_t src)
{
    if (is_digit(c)) {
        w.put(kOnes[c - '0'], src);
        w.space(src);
        return;
    }
    if (is_alpha(c)) {
        const int idx = std::tolower(static_cast<unsigned char>(c)) - 'a';
        if (idx >= 0 && idx <= 25) {
            w.put(kLetterName[idx], src);
            w.space(src);
        }
        return;
    }
    if (const char* name = symbol_name(c)) {
        w.put(name, src);
        w.space(src);
    }
}

void spell_letters(Writer& w, const std::string& token, uint32_t src)
{
    for (size_t i = 0; i < token.size(); ++i) {
        if (!is_alpha(token[i])) continue;
        const int idx = std::tolower(static_cast<unsigned char>(token[i])) - 'a';
        if (idx < 0 || idx > 25) continue;
        w.put(kLetterName[idx], src);
        w.put(' ', src);
    }
}

// Fold anything the engine's single-byte world cannot carry, and everything
// above ASCII, to a conservative set. Bytes >= 0x80 are a known source of
// stalls and dropped words even when the codepage nominally supports them.
char fold_byte(unsigned char c)
{
    if (c == '\r' || c == '\n' || c == '\t') return ' ';
    if (c < 0x20 || c == 0x7F) return ' ';
    if (c < 0x80) return static_cast<char>(c);

    // Latin-1 / CP1252 letters to their unaccented ASCII forms.
    static const struct { unsigned char from; const char* to; } kFold[] = {
        {0xC0,"A"},{0xC1,"A"},{0xC2,"A"},{0xC3,"A"},{0xC4,"A"},{0xC5,"A"},
        {0xC7,"C"},{0xC8,"E"},{0xC9,"E"},{0xCA,"E"},{0xCB,"E"},
        {0xCC,"I"},{0xCD,"I"},{0xCE,"I"},{0xCF,"I"},{0xD1,"N"},
        {0xD2,"O"},{0xD3,"O"},{0xD4,"O"},{0xD5,"O"},{0xD6,"O"},{0xD8,"O"},
        {0xD9,"U"},{0xDA,"U"},{0xDB,"U"},{0xDC,"U"},{0xDD,"Y"},
        {0xE0,"a"},{0xE1,"a"},{0xE2,"a"},{0xE3,"a"},{0xE4,"a"},{0xE5,"a"},
        {0xE7,"c"},{0xE8,"e"},{0xE9,"e"},{0xEA,"e"},{0xEB,"e"},
        {0xEC,"i"},{0xED,"i"},{0xEE,"i"},{0xEF,"i"},{0xF1,"n"},
        {0xF2,"o"},{0xF3,"o"},{0xF4,"o"},{0xF5,"o"},{0xF6,"o"},{0xF8,"o"},
        {0xF9,"u"},{0xFA,"u"},{0xFB,"u"},{0xFC,"u"},{0xFD,"y"},{0xFF,"y"},
        {0, nullptr}
    };
    for (int i = 0; kFold[i].to; ++i) {
        if (kFold[i].from == c) return kFold[i].to[0];
    }
    return ' ';
}

// Everything after the codepage conversion works on single bytes, with an
// index back into the original wide string for each one.
struct Byte { char c; uint32_t src; };

std::vector<Byte> to_bytes(const wchar_t* src, size_t len, unsigned codepage)
{
    std::vector<Byte> out;
    out.reserve(len);
    char buf[8];
    for (size_t i = 0; i < len; ++i) {
        const int n = WideCharToMultiByte(codepage, 0, src + i, 1, buf, sizeof(buf),
                                          nullptr, nullptr);
        if (n <= 0) {
            out.push_back({' ', static_cast<uint32_t>(i)});
            continue;
        }
        for (int k = 0; k < n; ++k) {
            const char folded = fold_byte(static_cast<unsigned char>(buf[k]));
            out.push_back({folded, static_cast<uint32_t>(i)});
        }
    }
    return out;
}

void spell_all(const std::vector<Byte>& in, std::string& out, std::vector<uint32_t>* map)
{
    Writer w(out, map);
    for (size_t i = 0; i < in.size(); ++i) {
        spell_one(w, in[i].c, in[i].src);
    }
    while (!out.empty() && out.back() == ' ') {
        out.erase(out.size() - 1);
        if (map) map->pop_back();
    }
}

const char* three_letter_fix(const std::string& tok)
{
    // Per-token bug reports inherited from the prior FlexVoice wrapper: these
    // specific spellings come out wrong or not at all.
    struct Fix { const char* from; const char* to; bool onlyWhenNotAllCaps; };
    static const Fix kFixes[] = {
        { "etc", "etcetera", false },
        { "nul", "null",     false },
        { "col", "cole",     true  },
        { "bor", "bore",     true  },
        { "pur", "purr",     true  },
        { "kik", "kick",     true  },
        { nullptr, nullptr,  false }
    };
    if (tok.size() != 3) return nullptr;
    bool allCaps = true;
    std::string lower;
    for (char c : tok) {
        if (!is_alpha(c)) return nullptr;
        if (std::islower(static_cast<unsigned char>(c))) allCaps = false;
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (int i = 0; kFixes[i].from; ++i) {
        if (lower == kFixes[i].from && !(kFixes[i].onlyWhenNotAllCaps && allCaps)) {
            return kFixes[i].to;
        }
    }
    return nullptr;
}

void run(const std::vector<Byte>& in, std::string& out, std::vector<uint32_t>* map)
{
    Writer w(out, map);
    const size_t n = in.size();
    size_t i = 0;

    while (i < n) {
        const char c = in[i].c;
        const uint32_t src = in[i].src;

        if (c == ' ') { w.space(src); ++i; continue; }

        // --- digit runs: the crash fix ------------------------------------
        if (is_digit(c)) {
            size_t j = i;
            std::string digits;
            while (j < n && is_digit(in[j].c)) { digits += in[j].c; ++j; }
            w.space(src);
            w.put(number_to_words(digits), src);
            w.space(src);
            i = j;
            continue;
        }

        // --- alphabetic tokens ---------------------------------------------
        if (is_alpha(c)) {
            size_t j = i;
            std::string tok;
            while (j < n && is_alpha(in[j].c)) { tok += in[j].c; ++j; }

            // "https://" and friends: the engine drops the scheme and
            // sometimes the rest of the token with it.
            if (j + 2 < n && in[j].c == ':' && in[j + 1].c == '/' && in[j + 2].c == '/' &&
                tok.size() >= 2 && tok.size() <= 10) {
                w.space(src);
                spell_letters(w, tok, src);
                w.put("colon slash slash ", src);
                i = j + 3;
                continue;
            }

            // Contractions, before the single-letter rule can see the tail.
            if (j < n && in[j].c == '\'' && j + 1 < n && is_alpha(in[j + 1].c)) {
                size_t k = j + 1;
                std::string tail;
                while (k < n && is_alpha(in[k].c)) { tail += in[k].c; ++k; }
                std::string lower;
                for (char t : tail) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(t)));

                const char* expand = nullptr;
                bool join = false;
                if (lower == "re") expand = " are";
                else if (lower == "ve") expand = " have";
                else if (lower == "ll") expand = " will";
                else if (lower == "m")  expand = " am";
                else if (lower == "d")  expand = " would";
                else if (lower == "s" || lower == "t") join = true;

                w.space(src);
                w.put(tok, src);
                if (expand) {
                    w.put(expand, src);
                } else if (join) {
                    // "it's" -> "its", "can't" -> "cant": leaving the lone
                    // letter would trip the single-letter rule below.
                    w.put(tail, src);
                }
                i = k;
                continue;
            }

            if (const char* fixed = three_letter_fix(tok)) {
                w.space(src);
                w.put(fixed, src);
                w.space(src);
                i = j;
                continue;
            }

            bool allCaps = true, anyVowel = false;
            for (char t : tok) {
                if (!std::isupper(static_cast<unsigned char>(t))) allCaps = false;
                if (is_vowel_like(t)) anyVowel = true;
            }

            if (tok.size() == 1) {
                // Consonants only: "a" and "I" are real words.
                if (!is_vowel_like(tok[0])) {
                    w.space(src);
                    spell_letters(w, tok, src);
                    i = j;
                    continue;
                }
            } else if (allCaps && tok.size() <= 6) {
                // NVDA -> en vee dee ay, or the acronym vanishes.
                w.space(src);
                spell_letters(w, tok, src);
                i = j;
                continue;
            } else if (!anyVowel && tok.size() <= 128) {
                // msgs, src, http: vowel-less tokens get dropped.
                w.space(src);
                spell_letters(w, tok, src);
                i = j;
                continue;
            }

            w.put(tok, src);
            i = j;
            continue;
        }

        // --- punctuation ----------------------------------------------------
        const bool prevAlnum = (i > 0) && is_alnum(in[i - 1].c);
        const bool nextAlnum = (i + 1 < n) && is_alnum(in[i + 1].c);

        switch (c) {
        case '-':
            // Glued hyphens truncate the token; a leading one before a number
            // is a minus sign.
            if (nextAlnum && is_digit(in[i + 1].c) &&
                (i == 0 || in[i - 1].c == ' ' || in[i - 1].c == '(' ||
                 in[i - 1].c == '[' || in[i - 1].c == '{' || in[i - 1].c == ',' ||
                 in[i - 1].c == ':')) {
                w.space(src);
                w.put("minus", src);
                w.space(src);
            } else {
                w.space(src);
            }
            break;
        case '+':
            if (prevAlnum || nextAlnum || (i + 1 < n && in[i + 1].c == '+')) {
                w.space(src); w.put("plus", src); w.space(src);
            } else {
                w.space(src);
            }
            break;
        case '.':
            if (prevAlnum && nextAlnum) { w.space(src); w.put("dot", src); w.space(src); }
            else { w.put('.', src); }
            break;
        case '/':
            if (prevAlnum && nextAlnum) { w.space(src); w.put("slash", src); w.space(src); }
            else { w.space(src); }
            break;
        case '\\':
            // The engine's embedded-command escape. It must never reach the
            // engine as data or the rest of the line is parsed as a command.
            w.space(src); w.put("backslash", src); w.space(src);
            break;
        case '@':
            w.space(src); w.put("at", src); w.space(src);
            break;
        case '#':
            w.space(src); w.put("hash", src); w.space(src);
            break;
        case '_':
            w.space(src);
            break;
        case ',': case ';': case ':': case '?': case '!':
            w.put(c, src);
            break;
        case '\'':
            // A lone apostrophe makes the engine split oddly.
            w.space(src);
            break;
        default:
            // Everything else is a word boundary rather than a mystery to the
            // engine's text normalizer.
            w.space(src);
            break;
        }
        ++i;
    }

    // Trim a trailing space so the caller's offsets end where the text does.
    while (!out.empty() && out.back() == ' ') {
        out.erase(out.size() - 1);
        if (map) map->pop_back();
    }
}

}  // namespace

const char* symbol_name(char c)
{
    // Plain, unambiguous names. A screen reader normally substitutes its own
    // before the text ever reaches a synthesiser, so these are the fallback for
    // when a raw character arrives -- which is exactly what happens when the
    // user arrows onto one.
    switch (c) {
    case ' ':  return "space";
    case '\t': return "tab";
    case '!':  return "exclamation";
    case '"':  return "quote";
    case '#':  return "number sign";
    case '$':  return "dollar";
    case '%':  return "percent";
    case '&':  return "ampersand";
    case '\'': return "apostrophe";
    case '(':  return "left paren";
    case ')':  return "right paren";
    case '*':  return "asterisk";
    case '+':  return "plus";
    case ',':  return "comma";
    case '-':  return "dash";
    case '.':  return "period";
    case '/':  return "slash";
    case ':':  return "colon";
    case ';':  return "semicolon";
    case '<':  return "less than";
    case '=':  return "equals";
    case '>':  return "greater than";
    case '?':  return "question mark";
    case '@':  return "at";
    case '[':  return "left bracket";
    case '\\': return "backslash";
    case ']':  return "right bracket";
    case '^':  return "caret";
    case '_':  return "underscore";
    case '`':  return "back tick";
    case '{':  return "left brace";
    case '|':  return "vertical bar";
    case '}':  return "right brace";
    case '~':  return "tilde";
    default:   return nullptr;
    }
}

std::string number_to_words(const std::string& digits)
{
    if (digits.empty()) return std::string();

    // A leading zero is significant -- 007, area codes, version numbers -- and
    // anything absurdly long would overflow, so both are spoken digit by digit.
    if (digits[0] == '0' || digits.size() > 15) {
        std::string out;
        for (size_t i = 0; i < digits.size(); ++i) {
            if (i) out += " ";
            out += kOnes[digits[i] - '0'];
        }
        return out;
    }

    unsigned long long v = 0;
    for (char c : digits) {
        const unsigned d = static_cast<unsigned>(c - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10ull) {
            std::string out;
            for (size_t i = 0; i < digits.size(); ++i) {
                if (i) out += " ";
                out += kOnes[digits[i] - '0'];
            }
            return out;
        }
        v = v * 10 + d;
    }
    if (v == 0) return "zero";

    // Split into groups of three, most significant first.
    unsigned groups[5] = {};
    int count = 0;
    while (v && count < 5) { groups[count++] = static_cast<unsigned>(v % 1000); v /= 1000; }

    std::string out;
    for (int g = count - 1; g >= 0; --g) {
        if (!groups[g]) continue;
        if (!out.empty()) out += " ";
        out += words_below_1000(groups[g]);
        out += kScales[g];
    }
    return out;
}

namespace {

bool has_speakable(const std::string& s)
{
    for (char c : s) {
        if (is_alnum(c)) return true;
    }
    return false;
}

// Prose rules first; if they leave nothing a voice could say, name the
// characters instead. Arrowing through a document one character at a time
// sends exactly that -- a lone "," or "(" -- and the prose rules quite
// correctly reduce those to a word boundary, which is silence.
void run_with_fallback(const std::vector<Byte>& bytes, Mode mode,
                       std::string& text, std::vector<uint32_t>* map)
{
    if (mode == Spell) {
        spell_all(bytes, text, map);
        return;
    }
    run(bytes, text, map);
    if (!has_speakable(text)) {
        text.clear();
        if (map) map->clear();
        spell_all(bytes, text, map);
    }
}

}  // namespace

Normalized normalize(const wchar_t* src, size_t len, unsigned codepage, Mode mode)
{
    Normalized out;
    if (!src || !len) return out;
    const std::vector<Byte> bytes = to_bytes(src, len, codepage);
    out.srcMap.reserve(bytes.size());
    run_with_fallback(bytes, mode, out.text, &out.srcMap);
    return out;
}

std::string normalize_bytes(const std::string& in, Mode mode)
{
    std::vector<Byte> bytes;
    bytes.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        bytes.push_back({fold_byte(static_cast<unsigned char>(in[i])),
                         static_cast<uint32_t>(i)});
    }
    std::string out;
    run_with_fallback(bytes, mode, out, nullptr);
    return out;
}

}  // namespace text
}  // namespace FlexVoice
