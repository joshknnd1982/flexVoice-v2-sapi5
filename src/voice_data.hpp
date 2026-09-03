// voice_data.hpp -- the FlexVoice voice roster and the speech-parameter model.
//
// Both tables are the product of measurement, not of the vendor documentation.
// The SDK documents 33 Speaker attributes but gives a range for none of them,
// and Speaker::set() accepts every value you hand it -- including values that
// make the engine abort(). So each parameter below was swept across its range
// with the rendered audio measured (duration, median F0, F0 spread, RMS, peak,
// clipping) and the limits here are where the audio stops being usable.
//
// Four documented parameters are deliberately absent: speedWPM, creakiness,
// singingPitchRate and volumeSmoothWindow. Sweeping each across its full range
// produced bit-identical output every time -- they are inert in this build of
// the engine, and exposing a control that does nothing is worse than omitting it.

#pragma once

#include <stdint.h>
#include <cmath>
#include <cstring>

#include "flexvoice_protocol.h"

namespace FlexVoice {
namespace voices {

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------
//
// FVLanguage.h declares only LNG_ENGLISH and LNG_HUNGARIAN, but asking the
// engine itself (getLangName over the 0x0400..0x0440 range) returns four:
//   0x0405 Czech, 0x0409 English:US, 0x040e Hungarian, 0x043e Malay.
// Only the languages whose data directory is present can actually be loaded;
// loadLanguage() throws "Cannot load language!" for the others.

struct Language {
    const char*    code;      // three-letter code used in settings.ini
    uint32_t       id;        // MM_TTSAPI::Language
    const wchar_t* name;      // display name
    const wchar_t* lcid;      // SAPI token Language attribute (hex, no 0x)
    const char*    dir;       // data subdirectory under the engine root
    unsigned       codepage;  // the engine takes single-byte text, not UTF-8
};

inline const Language kLanguages[] = {
    { "eng", 0x0409, L"English (United States)", L"409", "English",   1252 },
    { "hun", 0x040e, L"Hungarian",               L"40E", "Hungarian", 1250 },
    { "cze", 0x0405, L"Czech",                   L"405", "Czech",     1250 },
    { "msa", 0x043e, L"Malay",                   L"43E", "Malay",     1252 },
};
inline const int kLanguageCount = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));

inline int language_index_from_code(const char* code)
{
    for (int i = 0; i < kLanguageCount; ++i) {
        if (_stricmp(kLanguages[i].code, code) == 0) return i;
    }
    return -1;
}

inline int language_index_from_id(uint32_t id)
{
    for (int i = 0; i < kLanguageCount; ++i) {
        if (kLanguages[i].id == id) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------
//
// FlexVoice 3.01's English runtime ships two diphone databases -- Julie
// (female) and Tom (male) -- and three .tav speaker files, two of which
// (default.tav and Voices\Kim.tav) are byte-identical.
//
// The remaining five are MindMaker's own FlexVoice 2.0 speaker presets with
// their parameters re-expressed in the 3.01 schema. The 2.0 files cannot be
// loaded directly: they carry the obsolete durationDescr / pitchDescr /
// volumeDescr selectors and none of the model filenames 3.01 requires, and
// feeding one to the 3.01 engine makes it print "Undefined of improper
// featurevalue type in featValVal" and call abort(). Ported across are
// gender, age, and every prosody and voice-source parameter 3.01 still
// understands; not ported are the 2.0 volume scale (different units) and its
// 24-band equaliser (the 3.01 Julie base is tuned for 12).
//
// Each voice also carries a levelTrim, which is a measurement rather than a
// MindMaker value; see the comment on Voice::levelTrim below.

struct VoiceParamOverride {
    const char* name;    // FlexVoice Speaker attribute name
    bool        isInt;
    double      value;
};

struct Voice {
    const wchar_t*            displayName;
    const char*               tavRelative;   // relative to the language data dir
    const wchar_t*            gender;        // SAPI attribute
    const wchar_t*            age;           // SAPI attribute
    int                       languageIndex;
    const char*               origin;        // provenance, for the README and logs
    const VoiceParamOverride* overrides;
    int                       overrideCount;
    bool                      isCustom;      // the configurable voice
    // Engine-level gain that puts this voice at the same peak as the rest.
    // Measured, not guessed: each voice was rendered at a fixed low gain, its
    // peak read off the waveform, and the trim solved for a common target.
    // Without it the ported FlexVoice 2.0 presets run 8 dB hot -- they were
    // written against a different volume scale -- and clip, while the shipped
    // voices sit well under. See probe/calibrate.py.
    double                    levelTrim;
};

// --- override tables -------------------------------------------------------

// The three voices FlexVoice 3.01 actually ships need no parameter changes:
// their .tav files already are the voice. Only their level trim differs.
inline const VoiceParamOverride kBillOv[] = {
    { "defaultPitch", true, 90 },
    { "headsize", false, 1.2 }, { "richness", false, 0.6 }, { "smoothness", false, 0.61 },
    { "fricationRate", false, 0.81 }, { "plosiveRate", false, 1.0 },
    { "intonationLevel", false, 1.0 }, { "speechRate", false, 1.0 },
};
inline const VoiceParamOverride kJillOv[] = {
    { "defaultPitch", true, 275 },
    { "headsize", false, 0.74 }, { "richness", false, 0.99 }, { "smoothness", false, 0.55 },
    { "fricationRate", false, 0.81 }, { "plosiveRate", false, 1.0 },
    { "intonationLevel", false, 1.0 }, { "speechRate", false, 0.81 },
};
inline const VoiceParamOverride kJuliusOv[] = {
    { "defaultPitch", true, 90 },
    { "headsize", false, 1.2 }, { "richness", false, 0.6 }, { "smoothness", false, 0.74 },
    { "fricationRate", false, 0.57 }, { "plosiveRate", false, 0.74 },
    { "intonationLevel", false, 1.0 }, { "speechRate", false, 1.0 },
};
inline const VoiceParamOverride kKitOv[] = {
    { "defaultPitch", true, 115 },
    { "headsize", false, 0.92 }, { "richness", false, 0.76 }, { "smoothness", false, 0.77 },
    { "fricationRate", false, 0.96 }, { "plosiveRate", false, 1.0 },
    { "intonationLevel", false, 1.02 }, { "speechRate", false, 1.0 },
};
inline const VoiceParamOverride kJuliaOv[] = {
    { "defaultPitch", true, 170 },
    { "headsize", false, 1.0 }, { "richness", false, 0.99 }, { "smoothness", false, 0.78 },
    { "fricationRate", false, 0.2 }, { "plosiveRate", false, 1.2 },
    { "intonationLevel", false, 1.0 }, { "speechRate", false, 1.0 },
};

#define FV_OV(a) a, static_cast<int>(sizeof(a) / sizeof(a[0]))
#define FV_NO_OV nullptr, 0

// Index 0 is the configurable voice; everything else is a fixed preset whose
// character parameters the utility must not touch, or every preset would sound
// alike.
inline const Voice kVoices[] = {
    { L"FlexVoice Custom Voice", "default.tav",       L"Female", L"Adult", 0,
      "configurable; base voice chosen in the FlexVoice configuration utility",
      FV_NO_OV, true, 1.270 },

    { L"FlexVoice Julie",  "default.tav",       L"Female", L"Adult", 0,
      "FlexVoice 3.01 English/default.tav (Julie diphone database)", FV_NO_OV, false, 1.254 },
    { L"FlexVoice Kim",    "Voices\\Kim.tav",   L"Female", L"Adult", 0,
      "FlexVoice 3.01 English/Voices/Kim.tav (byte-identical to default.tav)", FV_NO_OV, false, 1.336 },
    { L"FlexVoice Tim",    "Voices\\Tim.tav",   L"Male",   L"Adult", 0,
      "FlexVoice 3.01 English/Voices/Tim.tav (Tom diphone database)", FV_NO_OV, false, 1.130 },

    { L"FlexVoice Bill",   "default.tav",       L"Male",   L"Adult", 0,
      "FlexVoice 2.0 Bill.tav, parameters ported to the 3.01 schema", FV_OV(kBillOv), false, 0.449 },
    { L"FlexVoice Julius", "default.tav",       L"Male",   L"Adult", 0,
      "FlexVoice 2.0 Julius.tav, parameters ported to the 3.01 schema", FV_OV(kJuliusOv), false, 0.680 },
    { L"FlexVoice Julia",  "default.tav",       L"Female", L"Adult", 0,
      "FlexVoice 2.0 Julie.tav, parameters ported to the 3.01 schema", FV_OV(kJuliaOv), false, 0.506 },
    { L"FlexVoice Jill",   "default.tav",       L"Female", L"Child", 0,
      "FlexVoice 2.0 Jill.tav, parameters ported to the 3.01 schema", FV_OV(kJillOv), false, 0.644 },
    { L"FlexVoice Kit",    "default.tav",       L"Male",   L"Child", 0,
      "FlexVoice 2.0 Kit.tav, parameters ported to the 3.01 schema", FV_OV(kKitOv), false, 0.552 },
};
inline const int kVoiceCount = static_cast<int>(sizeof(kVoices) / sizeof(kVoices[0]));

#undef FV_OV

// The two diphone databases, offered as the Custom voice's starting point.
struct BaseVoice {
    const wchar_t* name;
    const char*    tavRelative;
    const wchar_t* gender;
};
inline const BaseVoice kBaseVoices[] = {
    { L"Julie (female)", "default.tav",     L"Female" },
    { L"Tom (male)",     "Voices\\Tim.tav", L"Male"   },
};
inline const int kBaseVoiceCount = 2;

// ---------------------------------------------------------------------------
// Speech parameters
// ---------------------------------------------------------------------------
//
// Every control in the configuration utility is a whole percentage: 0 is the
// parameter's minimum usable value and 100 is its maximum, as Josh asked for.
// `scale` decides how the percentage is spread across that interval:
//
//   LINEAR    value = lo + (hi - lo) * p/100
//             for parameters that are additive and include a true zero
//             (intonation, breathiness, tilt, frication, plosive, volume).
//   GEOMETRIC value = lo * (hi/lo)^(p/100)
//             for ratio-like parameters, where a fixed number of percent
//             should mean a fixed *proportional* change -- rate, pitch in
//             hertz, head size. A linear rate control spends most of its
//             travel in the fast half and is unusable at the slow end.

enum ParamScale { SCALE_LINEAR, SCALE_GEOMETRIC };

struct ParamDef {
    FlexVoiceParam index;
    const char*    iniKey;
    const wchar_t* label;         // configuration utility label, with an access key
    const wchar_t* accName;       // MSAA name, spelling out the range in words
    double         lo;
    double         hi;
    ParamScale     scale;
    int            defaultPercent;
    bool           isInt;         // engine wants an int, not a double
    const wchar_t* unit;          // shown next to the value, may be empty
};

// Ranges below are where the sweep showed the audio still usable. Notes on the
// non-obvious ones:
//   speechRate  0.25 renders a sentence in 11 s, 6.0 in 0.54 s; below 0.25 the
//               engine still works but a screen reader is unusable.
//   volume      linear to a true zero, because 0 % must be silence. The engine
//               throws on volume == 0.0, so the host substitutes a floor.
//   pitchMin    the engine throws on pitchMin == 0, hence a floor of 20 Hz.
//   pitchMax    acts as a hard clamp on the contour: at 150 Hz the F0 standard
//               deviation collapses from 21 Hz to 1.9 Hz and the voice goes
//               monotone. Worth exposing, easy to misuse.
//   smoothness  0.0 renders 11 dB hotter than 0.78 and clips; the floor of 0.3
//               keeps the limiter from having to work hard.
//   tilt        negative values throw.
inline const ParamDef kParams[] = {
    { FVP_SPEECH_RATE,   "rate",        L"R&ate",
      L"Rate, 0 slowest to 100 fastest",                 0.25,   6.0,  SCALE_GEOMETRIC, 44, false, L"%" },
    { FVP_VOLUME,        "volume",      L"&Volume",
      L"Volume, 0 silent to 100 loudest",                0.0,    1.0,  SCALE_LINEAR,   100, false, L"%" },
    { FVP_DEFAULT_PITCH, "pitch",       L"&Pitch",
      L"Pitch, 0 is 50 hertz to 100 is 450 hertz",       50.0,  450.0, SCALE_GEOMETRIC, 64,  true, L"Hz" },
    { FVP_PITCH_RATE,    "pitchScale",  L"Pitch s&cale",
      L"Pitch scale, 0 halves the pitch range to 100 doubles it",
                                                          0.5,    2.0,  SCALE_GEOMETRIC, 50, false, L"" },
    { FVP_PITCH_MIN,     "pitchFloor",  L"Pitch &floor",
      L"Pitch floor, 0 is 20 hertz to 100 is 300 hertz", 20.0,  300.0, SCALE_GEOMETRIC, 33,  true, L"Hz" },
    { FVP_PITCH_MAX,     "pitchCeil",   L"Pitch ceilin&g",
      L"Pitch ceiling, 0 is 150 hertz to 100 is 1000 hertz",
                                                        150.0, 1000.0, SCALE_GEOMETRIC, 64,  true, L"Hz" },
    { FVP_INTONATION,    "intonation",  L"&Intonation",
      L"Intonation, 0 monotone to 100 highly expressive", 0.0,   5.0,  SCALE_LINEAR,    34, false, L"%" },
    { FVP_HEADSIZE,      "headSize",    L"Hea&d size",
      L"Head size, 0 smallest to 100 largest",            0.5,   2.0,  SCALE_GEOMETRIC, 46, false, L"%" },
    { FVP_TILT,          "tilt",        L"&Tilt",
      L"Tilt, 0 none to 100 maximum spectral tilt",       0.0,   4.0,  SCALE_LINEAR,     0, false, L"%" },
    { FVP_RICHNESS,      "richness",    L"Ric&hness",
      L"Richness, 0 thinnest to 100 richest",             0.0,   2.0,  SCALE_LINEAR,    50, false, L"%" },
    { FVP_BREATHINESS,   "breathiness", L"B&reathiness",
      L"Breathiness, 0 none to 100 most breathy",         0.0,   1.5,  SCALE_LINEAR,     0, false, L"%" },
    { FVP_SMOOTHNESS,    "smoothness",  L"Smooth&ness",
      L"Smoothness, 0 roughest to 100 smoothest",         0.3,   1.2,  SCALE_LINEAR,    53, false, L"%" },
    { FVP_FRICATION,     "frication",   L"Fricati&on",
      L"Frication, 0 none to 100 strongest",              0.0,   1.5,  SCALE_LINEAR,     8, false, L"%" },
    { FVP_PLOSIVE,       "plosive",     L"Plosive&s",
      L"Plosives, 0 softest to 100 hardest",              0.0,   2.0,  SCALE_LINEAR,    20, false, L"%" },
};
inline const int kParamCount = static_cast<int>(sizeof(kParams) / sizeof(kParams[0]));

inline const ParamDef& param(FlexVoiceParam p)
{
    for (int i = 0; i < kParamCount; ++i) {
        if (kParams[i].index == p) return kParams[i];
    }
    return kParams[0];
}

inline int clamp_percent(int p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

// percentage -> engine value
inline double percent_to_value(const ParamDef& d, int percent)
{
    const double t = clamp_percent(percent) / 100.0;
    double v;
    if (d.scale == SCALE_GEOMETRIC) {
        v = d.lo * std::pow(d.hi / d.lo, t);
    } else {
        v = d.lo + (d.hi - d.lo) * t;
    }
    return d.isInt ? std::floor(v + 0.5) : v;
}

// engine value -> percentage, for showing a preset's value in the utility
inline int value_to_percent(const ParamDef& d, double value)
{
    double t;
    if (d.scale == SCALE_GEOMETRIC) {
        if (value <= 0 || d.lo <= 0) return d.defaultPercent;
        t = std::log(value / d.lo) / std::log(d.hi / d.lo);
    } else {
        t = (d.hi == d.lo) ? 0.0 : (value - d.lo) / (d.hi - d.lo);
    }
    return clamp_percent(static_cast<int>(std::floor(t * 100.0 + 0.5)));
}

}  // namespace voices
}  // namespace FlexVoice
