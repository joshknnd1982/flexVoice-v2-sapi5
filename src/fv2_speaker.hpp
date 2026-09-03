// fv2_speaker.hpp -- the FlexVoice 2.0 .tav speaker file, as data.
//
// FlexVoice 3.01 let a client set synthesis parameters by name through an
// IAttribute interface. **FlexVoice 2.0 has no such interface** -- no
// IAttribute class is exported at all -- and Speaker exports only load() and
// save(). So the .tav file is not merely how a voice is stored, it is the only
// way to reach a parameter. Everything the configuration utility offers ends up
// here, is written to a file, and is handed to Speaker::load().
//
// The format is plain text and reads like this (Julie.tav, abridged):
//
//     str gender="female";
//     dbl headsize=1.0;
//     int defaultPitch=170;
//     obj:Filter vsHarmFilter
//     {
//         dbl d1=0.1;
//         ...
//     }
//     obj:VSNoiseParams vsNoiseParams
//     {
//         dbl noisePeakAmpl = 0.3;
//         ...
//     }
//     obj:Equalizer equalizer[24]
//     <
//         obj { 31.000000  ; 0.500000; 0.67 ; },
//         ...
//     >
//
// Note the three bracket styles -- `=...;` scalars, `{...}` sub-objects and
// `<...>` arrays -- and that equalizer bands are positional triples rather than
// named fields. Round-tripping preserves all of it, because a field this code
// does not understand is still a field the engine does.

#pragma once

#include <string>
#include <vector>

namespace fv2 {

// The parameters the configuration utility exposes. These are exactly the
// scalar keys that appear in Mindmaker's own .tav files -- there is no wider
// set to discover, because a key the engine does not know is ignored and a
// value it dislikes is what makes it abort.
//
// Ordering here is the order of the controls in the utility.
// This order is both the array order of kParams and the order the controls
// appear in the configuration utility, so the working parameters come first
// and the three the engine ignores come last.
enum ParamId {
    P_VOLUME = 0,        // dbl  the voice's own amplitude, not a 0..1 gain
    P_SPEECH_RATE,       // dbl  duration scale
    P_DEFAULT_PITCH,     // int  base F0, Hz
    P_PITCH_MIN,         // int  F0 floor, Hz
    P_PITCH_MAX,         // int  F0 ceiling, Hz
    P_PITCH_RATE,        // dbl  multiplies the whole F0 contour
    P_INTONATION,        // dbl  F0 variance; 0 is monotone
    P_HEADSIZE,          // dbl  vocal tract scale
    P_RICHNESS,          // dbl  voice source richness
    P_SMOOTHNESS,        // dbl
    P_FRICATION,         // dbl  fricative energy
    P_PLOSIVE,           // dbl  plosive burst energy
    P_VOLUME_SMOOTH,     // int  smoothing window

    // Parsed and written back, but the engine ignores them. See fv2_params.inc.
    P_TILT,
    P_SINGING_PITCH_RATE,
    P_SPEED_WPM,

    P_COUNT,
    // Everything below P_ACTIVE_COUNT is a control the utility offers.
    P_ACTIVE_COUNT = P_TILT
};

enum ParamType { PT_INT, PT_DOUBLE };

struct ParamInfo {
    ParamId     id;
    const char* tavKey;     // the name used in the .tav file
    const char* label;      // what the configuration utility calls it
    ParamType   type;
    double      lo;         // 0% on the utility's scale
    double      hi;         // 100% on the utility's scale
    double      fallback;   // used when a .tav omits the key
    bool        geometric;  // map the percentage logarithmically
    // False for keys the engine reads and then ignores. They are still parsed
    // and written back so a voice file round-trips, but the configuration
    // utility does not offer a control for them -- a slider that provably does
    // nothing is worse than no slider, especially read aloud.
    bool        active;
    const char* help;       // spoken by a screen reader as the control's description
};

// Indexed by ParamId.
extern const ParamInfo kParams[P_COUNT];

const ParamInfo& param(ParamId id);

// Percent (0..100) <-> engine value, honouring `geometric`. Out-of-range
// percentages are clamped, because every caller is a UI control.
double percentToValue(ParamId id, double percent);
double valueToPercent(ParamId id, double value);

// A biquad-ish filter description; four doubles, whatever the engine does
// with them.
struct Filter {
    bool   present = false;
    double d1 = 0.0, d2 = 0.0, f1 = 0.0, f2 = 0.0;
};

struct NoiseParams {
    bool   present = false;
    double peakAmpl = 0.0, startPos = 0.0, floorLevel = 0.0;
};

// One equalizer band: centre frequency, bandwidth, gain in dB. Written back in
// the same positional order they were read.
struct EqBand {
    double f0 = 0.0, bw = 0.0, gain = 0.0;
};

// A whole speaker definition, parsed from a .tav and writable back to one.
class SpeakerDef
{
public:
    // Parses `text`. Returns false and fills `error` if the file is not a .tav
    // at all; unknown keys are preserved rather than rejected.
    bool parse(const std::string& text, std::string& error);
    bool loadFile(const std::string& path, std::string& error);

    // Renders a .tav. Byte-for-byte round-tripping is not attempted --
    // whitespace and number formatting are normalised -- but every field that
    // was read is written back.
    std::string serialize() const;
    bool saveFile(const std::string& path, std::string& error) const;

    // Scalar parameters, by id. has() distinguishes "the file set this" from
    // "we are returning the fallback".
    bool   has(ParamId id) const;
    double get(ParamId id) const;
    void   set(ParamId id, double value);
    void   clear(ParamId id);

    // Descriptive strings. gender is "male"/"female"; age is "adult"/"child".
    std::string gender = "female";
    std::string age = "adult";
    std::string voiceDescr;          // selects the diphone database: "Julie" -> Julie.bin
    std::string durationDescr;       // "" or "_T21" -> Dur.net / Dur_T21.net
    std::string pitchDescr;
    std::string pitchQDescr;
    std::string volumeDescr;         // "" or "_L" -> Volume.net / Volume_L.net
    std::string vsShapeFileName;     // "JulieVsShape" -> JulieVsShape.txt

    Filter      vsNoiseFilter;
    Filter      vsHarmFilter;
    Filter      shfilter;
    NoiseParams vsNoiseParams;
    std::vector<EqBand> equalizer;

    // A flat gain in dB applied to every equalizer band, so the utility can
    // offer one loudness control without disturbing the voice's spectral shape.
    void applyEqualizerTrim(double db);

private:
    double values_[P_COUNT] = {};
    bool   present_[P_COUNT] = {};
};

}  // namespace fv2
