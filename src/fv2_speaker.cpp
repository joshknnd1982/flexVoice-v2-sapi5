#include "fv2_speaker.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fv2 {

// The parameter table lives in its own file so the measurements and their
// justifications stay next to the numbers. See fv2_params.inc.
const ParamInfo kParams[P_COUNT] = {
#include "fv2_params.inc"
};

const ParamInfo& param(ParamId id)
{
    return kParams[id];
}

double percentToValue(ParamId id, double percent)
{
    const ParamInfo& p = kParams[id];
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    const double t = percent / 100.0;

    double v;
    if (p.geometric && p.lo > 0.0) {
        v = p.lo * std::pow(p.hi / p.lo, t);
    } else {
        v = p.lo + (p.hi - p.lo) * t;
    }
    if (p.type == PT_INT) v = std::floor(v + 0.5);
    return v;
}

double valueToPercent(ParamId id, double value)
{
    const ParamInfo& p = kParams[id];
    if (value <= p.lo) return 0.0;
    if (value >= p.hi) return 100.0;

    double t;
    if (p.geometric && p.lo > 0.0) {
        t = std::log(value / p.lo) / std::log(p.hi / p.lo);
    } else {
        t = (value - p.lo) / (p.hi - p.lo);
    }
    return t * 100.0;
}

// ---------------------------------------------------------------------------
// A very small scanner for the .tav grammar
// ---------------------------------------------------------------------------

namespace {

struct Scanner {
    const std::string& s;
    size_t i = 0;
    explicit Scanner(const std::string& text) : s(text) {}

    bool eof() const { return i >= s.size(); }

    void skipSpace()
    {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
            // Mindmaker's files carry no comments, but tolerate them anyway.
            if (c == ';' && i + 1 < s.size() && s[i + 1] == ';') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }

    bool peekIs(char c) { skipSpace(); return !eof() && s[i] == c; }
    bool accept(char c) { skipSpace(); if (!eof() && s[i] == c) { ++i; return true; } return false; }

    std::string ident()
    {
        skipSpace();
        const size_t start = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
        return s.substr(start, i - start);
    }

    // Reads up to `stop` (not consumed), trimmed.
    std::string until(char stop)
    {
        skipSpace();
        const size_t start = i;
        while (i < s.size() && s[i] != stop) ++i;
        std::string v = s.substr(start, i - start);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' ||
                              v.back() == '\r' || v.back() == '\n')) {
            v.pop_back();
        }
        return v;
    }

    std::string quoted()
    {
        skipSpace();
        if (eof() || s[i] != '"') return std::string();
        ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != '"') ++i;
        std::string v = s.substr(start, i - start);
        if (!eof()) ++i;
        return v;
    }
};

double toDouble(const std::string& t)
{
    return std::strtod(t.c_str(), nullptr);
}

// Formats without a trailing ".000000" but keeps enough precision to
// round-trip the values Mindmaker shipped.
std::string num(double v, bool asInt)
{
    char buf[64];
    if (asInt) {
        std::snprintf(buf, sizeof(buf), "%d", (int)std::floor(v + 0.5));
        return buf;
    }
    // %.6g rounds 39.018975 to 39.019, which silently retunes an
    // equalizer band every time a voice is saved. 12 digits round-trips
    // every value Mindmaker shipped.
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    std::string out = buf;
    if (out.find('.') == std::string::npos &&
        out.find('e') == std::string::npos &&
        out.find("inf") == std::string::npos &&
        out.find("nan") == std::string::npos) {
        out += ".0";
    }
    return out;
}

ParamId keyToParam(const std::string& key, bool* found)
{
    for (int k = 0; k < P_COUNT; ++k) {
        if (key == kParams[k].tavKey) { *found = true; return (ParamId)k; }
    }
    *found = false;
    return P_COUNT;
}

}  // namespace

// ---------------------------------------------------------------------------
// SpeakerDef
// ---------------------------------------------------------------------------

bool SpeakerDef::has(ParamId id) const { return present_[id]; }

double SpeakerDef::get(ParamId id) const
{
    return present_[id] ? values_[id] : kParams[id].fallback;
}

void SpeakerDef::set(ParamId id, double value)
{
    values_[id] = value;
    present_[id] = true;
}

void SpeakerDef::clear(ParamId id) { present_[id] = false; }

void SpeakerDef::applyEqualizerTrim(double db)
{
    for (EqBand& b : equalizer) b.gain += db;
}

bool SpeakerDef::parse(const std::string& text, std::string& error)
{
    Scanner sc(text);
    bool sawAnything = false;

    // A .tav is user-editable and reaches this parser from the configuration
    // utility as well as from disk, so a malformed one must fail rather than
    // spin. Every pass through the loop has to consume at least one character.
    size_t guard = 0;

    while (true) {
        sc.skipSpace();
        if (sc.eof()) break;

        const size_t before = sc.i;
        if (++guard > text.size() + 16) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%u", (unsigned)sc.i);
            error = std::string("malformed .tav: no progress at offset ") + buf;
            return false;
        }

        const std::string kind = sc.ident();
        if (kind.empty()) {
            // Not something we recognise; skip a character and carry on rather
            // than failing a file the engine itself would accept.
            ++sc.i;
            continue;
        }

        if (kind == "str" || kind == "int" || kind == "dbl") {
            const std::string name = sc.ident();
            if (!sc.accept('=')) { error = "expected = after " + name; return false; }

            if (kind == "str") {
                const std::string v = sc.quoted();
                sc.accept(';');
                if (name == "gender") gender = v;
                else if (name == "age") age = v;
                else if (name == "voiceDescr") voiceDescr = v;
                else if (name == "durationDescr") durationDescr = v;
                else if (name == "pitchDescr") pitchDescr = v;
                else if (name == "pitchQDescr") pitchQDescr = v;
                else if (name == "volumeDescr") volumeDescr = v;
                else if (name == "vsShapeFileName") vsShapeFileName = v;
            } else {
                const std::string v = sc.until(';');
                sc.accept(';');
                bool found = false;
                const ParamId id = keyToParam(name, &found);
                if (found) set(id, toDouble(v));
            }
            sawAnything = true;
            continue;
        }

        if (kind == "obj") {
            // obj:Type name ...
            std::string type;
            if (sc.accept(':')) type = sc.ident();
            const std::string name = sc.ident();

            // An array: name[N] < obj { a; b; c; }, ... >
            if (sc.accept('[')) {
                sc.until(']');
                sc.accept(']');
                if (sc.accept('<')) {
                    equalizer.clear();
                    while (!sc.eof() && !sc.peekIs('>')) {
                        if (!sc.accept('o')) { ++sc.i; continue; }
                        // we just consumed 'o' of "obj"
                        sc.ident();                 // "bj"
                        if (!sc.accept('{')) continue;
                        EqBand band;
                        band.f0   = toDouble(sc.until(';')); sc.accept(';');
                        band.bw   = toDouble(sc.until(';')); sc.accept(';');
                        band.gain = toDouble(sc.until(';')); sc.accept(';');
                        sc.accept('}');
                        sc.accept(',');
                        equalizer.push_back(band);
                    }
                    sc.accept('>');
                }
                sawAnything = true;
                continue;
            }

            // A sub-object: name { dbl f=...; ... }
            if (sc.accept('{')) {
                Filter f;
                NoiseParams np;
                f.present = true;
                np.present = true;
                while (!sc.eof() && !sc.peekIs('}')) {
                    const std::string fkind = sc.ident();
                    if (fkind.empty()) { ++sc.i; continue; }
                    const std::string fname = sc.ident();
                    if (!sc.accept('=')) { ++sc.i; continue; }
                    const double v = toDouble(sc.until(';'));
                    sc.accept(';');

                    if (fname == "d1") f.d1 = v;
                    else if (fname == "d2") f.d2 = v;
                    else if (fname == "f1") f.f1 = v;
                    else if (fname == "f2") f.f2 = v;
                    else if (fname == "noisePeakAmpl") np.peakAmpl = v;
                    else if (fname == "noiseStartPos") np.startPos = v;
                    else if (fname == "noiseFloor") np.floorLevel = v;
                }
                sc.accept('}');

                if (type == "VSNoiseParams" || name == "vsNoiseParams") {
                    vsNoiseParams = np;
                } else if (name == "vsNoiseFilter") {
                    vsNoiseFilter = f;
                } else if (name == "vsHarmFilter") {
                    vsHarmFilter = f;
                } else if (name == "shfilter") {
                    shfilter = f;
                }
                sawAnything = true;
                continue;
            }
        }

        // Unrecognised token: step over it.
        sc.skipSpace();
        if (!sc.eof()) ++sc.i;
        if (sc.i == before) ++sc.i;   // never leave the cursor where it was
    }

    if (!sawAnything) {
        error = "not a FlexVoice .tav file";
        return false;
    }
    return true;
}

bool SpeakerDef::loadFile(const std::string& path, std::string& error)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), error);
}

std::string SpeakerDef::serialize() const
{
    std::ostringstream o;

    auto str = [&o](const char* k, const std::string& v) {
        o << "str " << k << "=\"" << v << "\";\r\n";
    };
    auto scalar = [&](ParamId id) {
        const ParamInfo& p = kParams[id];
        o << (p.type == PT_INT ? "int " : "dbl ") << p.tavKey << "="
          << num(get(id), p.type == PT_INT) << ";\r\n";
    };

    str("gender", gender);
    str("age", age);
    str("voiceDescr", voiceDescr);
    str("durationDescr", durationDescr);
    str("pitchDescr", pitchDescr);
    str("pitchQDescr", pitchQDescr);
    str("volumeDescr", volumeDescr);

    // Mindmaker's own ordering, so a generated file diffs cleanly against a
    // shipped one.
    scalar(P_VOLUME_SMOOTH);
    scalar(P_VOLUME);
    scalar(P_SPEECH_RATE);
    scalar(P_SPEED_WPM);
    scalar(P_DEFAULT_PITCH);
    scalar(P_PITCH_MIN);
    scalar(P_PITCH_MAX);
    scalar(P_PITCH_RATE);
    scalar(P_SINGING_PITCH_RATE);
    scalar(P_INTONATION);
    scalar(P_HEADSIZE);
    scalar(P_TILT);
    scalar(P_RICHNESS);
    scalar(P_SMOOTHNESS);
    scalar(P_FRICATION);
    scalar(P_PLOSIVE);

    auto filter = [&o](const char* name, const Filter& f) {
        if (!f.present) return;
        o << "obj:Filter " << name << "\r\n{\r\n"
          << "    dbl d1=" << num(f.d1, false) << ";\r\n"
          << "    dbl d2=" << num(f.d2, false) << ";\r\n"
          << "    dbl f1=" << num(f.f1, true) << ";\r\n"
          << "    dbl f2=" << num(f.f2, true) << ";\r\n"
          << "}\r\n";
    };

    filter("vsNoiseFilter", vsNoiseFilter);
    filter("vsHarmFilter", vsHarmFilter);
    filter("shfilter", shfilter);

    if (!vsShapeFileName.empty()) str("vsShapeFileName", vsShapeFileName);

    if (vsNoiseParams.present) {
        o << "obj:VSNoiseParams vsNoiseParams\r\n{\r\n"
          << "    dbl noisePeakAmpl = " << num(vsNoiseParams.peakAmpl, false) << ";\r\n"
          << "    dbl noiseStartPos = " << num(vsNoiseParams.startPos, false) << ";\r\n"
          << "    dbl noiseFloor = " << num(vsNoiseParams.floorLevel, false) << ";\r\n"
          << "}\r\n";
    }

    if (!equalizer.empty()) {
        o << "obj:Equalizer equalizer[" << equalizer.size() << "]\r\n<\r\n";
        for (size_t i = 0; i < equalizer.size(); ++i) {
            const EqBand& b = equalizer[i];
            o << "    obj { " << num(b.f0, false) << " ; "
              << num(b.bw, false) << "; " << num(b.gain, false) << " ; }";
            if (i + 1 < equalizer.size()) o << ",";
            o << "\r\n";
        }
        o << ">\r\n";
    }

    return o.str();
}

bool SpeakerDef::saveFile(const std::string& path, std::string& error) const
{
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) { error = "cannot write " + path; return false; }
    const std::string text = serialize();
    out.write(text.data(), (std::streamsize)text.size());
    if (!out) { error = "write failed for " + path; return false; }
    return true;
}

}  // namespace fv2
