// fv2_voices.hpp -- the voice roster.
//
// FlexVoice 2.0's Julie voice pack ships six .tav speaker files, but only five
// voices: Default.tav and Julie.tav are byte-identical, so shipping both would
// put the same voice in the list twice under two names.
//
// All five share one diphone database (Julie.bin, generated from Julie.cod at
// install time). They differ in the prosody models they select and in their
// voice-source parameters -- which is how one recorded voice becomes a male
// adult, a female child and so on.
//
// **Four of the five clip at Mindmaker's own shipped volume.** Rendering a
// demanding sentence at each voice's shipped `volume` gives peaks from 31285
// to 54590 against a 32767 ceiling: up to 4.5 dB of hard clipping, out of the
// box. Each voice therefore carries a measured trim, chosen to put its peak at
// about -2 dBFS, which also loudness-matches the roster so switching voices
// does not jump the level. `volume` is exactly linear in peak amplitude, so
// the trim is just target/peak-per-unit; see probe/sweep.py.

#pragma once

#include <string>
#include <vector>

namespace fv2 {

struct VoiceInfo {
    const char* name;        // shown in the SAPI voice list, without the prefix
    const char* tavFile;     // under Data\Voices
    const char* gender;      // "Male" / "Female"
    const char* age;         // "Adult" / "Child"
    double      shippedVolume;   // what Mindmaker's file says
    double      trimmedVolume;   // what we use instead; 0 means "leave alone"
    const char* description;
};

// Measured on "The quick brown fox jumps over the lazy dog. She sells sea
// shells by the sea shore. Peter Piper picked a peck of pickled peppers."
inline const std::vector<VoiceInfo>& builtinVoices()
{
    static const std::vector<VoiceInfo> v = {
        // name      tav            gender    age      shipped trimmed
        { "Julie",  "Julie.tav",   "Female", "Adult",  5.8, 4.82,
          "The voice the diphone database was recorded from." },
        { "Bill",   "Bill.tav",    "Male",   "Adult",  3.8, 2.39,
          "A male adult built from Julie's recordings by lowering the pitch "
          "to 90 Hz and enlarging the vocal tract." },
        { "Jill",   "Jill.tav",    "Female", "Child",  3.0, 1.59,
          "A female child: 275 Hz, a small vocal tract and a slower speaking "
          "rate." },
        { "Julius", "Julius.tav",  "Male",   "Adult",  4.8, 2.29,
          "A second male adult, softer than Bill in its plosives." },
        { "Kit",    "Kit.tav",     "Male",   "Child",  4.7, 2.61,
          "A male child at 115 Hz with the most frication of the five." },
    };
    return v;
}

// The configurable voice. Its parameters come from the configuration utility
// rather than from a .tav on disk, which is the whole point of it: a screen
// reader can only offer rate, pitch and volume, and FlexVoice has thirteen
// working parameters.
inline const char* customVoiceName() { return "Custom Voice"; }

// Every registered voice is prefixed, so a token sweep can find them all and
// so they sort together in a voice list.
inline const char* voicePrefix() { return "FlexVoice2 "; }

// Token ids are the prefix plus the name with spaces removed, e.g.
// FlexVoice2_Julie, FlexVoice2_CustomVoice.
inline std::string voiceTokenId(const std::string& name)
{
    std::string id = "FlexVoice2_";
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != ' ') id += name[i];
    }
    return id;
}

}  // namespace fv2
