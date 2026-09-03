// fv_engine.hpp -- the FlexVoice 3.01 C++ SDK, wrapped in something safe to
// drive from a pipe server. 32-bit only: the engine DLL is x86.
//
// What this file exists to hide, all of it measured rather than documented:
//
//   * Engine::wait() does not work with a client-supplied IWaveOutputSite.
//     Ten fresh engines, ten times zero bytes of audio produced before wait()
//     returned. The reliable completion signal is the engine's own
//     BM_TEXT_END bookmark, which arrived 10/10.
//   * A bookmark added after the last text fragment never comes back -- the
//     engine emits BM_TEXT_END instead. Trailing bookmarks must be flushed by
//     hand when the utterance ends.
//   * Speaker::set() accepts every value including ones that abort the
//     process. Negative tilt, smoothness, intonationLevel, fricationRate and
//     plosiveRate throw, as do volume == 0.0 and pitchMin == 0.
//   * The engine calls abort() outright on some malformed input. Nothing here
//     can prevent that, which is why this code lives in a host process rather
//     than in the screen reader.
//
// The good news, also measured: speakRequest() is asynchronous, put() runs on
// an engine-owned worker thread, first audio arrives ~15 ms after the request,
// a whole second of speech renders in ~15 ms, and Engine::stop() returns in
// 0 ms with not one further put() afterwards.

#pragma once

#include <windows.h>
#include <deque>
#include <string>
#include <vector>

#include "flexvoice_protocol.h"

namespace FlexVoice {

// One item out of the engine, in the order the engine produced it.
struct StreamItem {
    enum Kind { AUDIO, BOOKMARK, WORD, SENTENCE, DONE, FAILED };
    Kind                       kind = AUDIO;
    std::vector<unsigned char> audio;
    uint32_t                   value = 0;    // bookmark id
    uint32_t                   position = 0; // word/sentence: char offset
    uint32_t                   length = 0;   // word/sentence: char count
    std::string                message;      // FAILED
};

// What to speak: text interleaved with bookmarks and silences.
struct Segment {
    FlexVoiceSegmentKind kind = FV_SEG_TEXT;
    uint32_t             value = 0;
    std::string          text;   // UTF-8
};

class Engine {
public:
    Engine();
    ~Engine();

    // dataRoot is the directory holding the per-language data folders.
    bool open(const std::string& dataRoot, uint32_t language, std::string& error);
    bool isOpen() const { return factory_ != nullptr; }
    uint32_t language() const { return language_; }

    // Which languages actually have loadable data under dataRoot.
    static std::vector<uint32_t> probeLanguages(const std::string& dataRoot);

    // Load a speaker file and apply overrides. `tavPath` is absolute.
    bool selectVoice(const std::string& tavPath,
                     const double* params, uint32_t paramMask,
                     int sampleRate, std::string& error);

    // Queue an utterance. Returns immediately; results arrive through poll().
    bool speak(const std::vector<Segment>& segments,
               bool wantWords, bool wantSentences, std::string& error);

    // Pop the next stream item, or return false if none is ready yet.
    bool poll(StreamItem& out);

    // Block until poll() would return something, or until the utterance ends.
    // Polling with Sleep(1) instead costs a full 15.6 ms timer tick per
    // utterance -- which was most of the remaining keystroke latency once the
    // pipe reconnect was fixed.
    void waitForItem(uint32_t timeoutMs);
    // True once DONE or FAILED has been produced for the current utterance.
    bool finished() const;

    void stop();

private:
    class Site;
    friend class Site;

    void* factory_ = nullptr;    // MM_TTSAPI::EngineFactory*
    void* engine_  = nullptr;    // MM_TTSAPI::Engine*
    void* speaker_ = nullptr;    // MM_TTSAPI::Speaker*
    Site* site_    = nullptr;

    std::string dataRoot_;
    std::string currentTav_;
    uint32_t    language_ = 0;
    int         sampleRate_ = 16000;

    // What the current Speaker was built from, so an utterance that changes
    // nothing but rate or volume skips reloading the .tav entirely.
    uint32_t    lastMask_ = 0;
    double      lastParams_[FVP_COUNT] = {};

    void destroyEngine();
};

// Clamp a parameter to a value the engine will not throw on, and to the range
// the sweep showed still produces usable audio.
double clampParam(FlexVoiceParam p, double v);

}  // namespace FlexVoice
