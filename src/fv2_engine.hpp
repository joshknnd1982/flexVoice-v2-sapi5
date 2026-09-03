// fv2_engine.hpp -- FlexVoice 2.0, wrapped in something safe to drive from a
// pipe server.
//
// 32-bit only: FlexVoice_2_00_010.dll is x86 and no 64-bit build of it exists,
// which is why the SAPI wrapper needs a host process at all.
//
// What this file exists to hide, all of it measured rather than documented:
//
//   * Engine::wait() does not return with a client-supplied IWaveOutputSite.
//     The reliable completion signal is the engine's own BM_TEXT_END bookmark.
//   * Engine::stop() must be called before the Engine is deleted. Without it
//     the destructor blocks forever on the engine's worker thread -- and it
//     presents as a hang *after* a completely successful render, which is a
//     memorably unhelpful way to fail.
//   * EngineFactory is lazy. It accepts a path that does not exist, and every
//     load happens inside createEngine, which faults on a null pointer rather
//     than throwing when a file is missing. So open() checks the data
//     directory itself before handing it over.
//   * FlexVoice 2.0 has no IAttribute interface. A synthesis parameter can
//     only be changed by loading a Speaker from a .tav file, so selectVoice()
//     writes one. That is not a workaround for a missing feature; it is the
//     interface the engine has.
//   * Two engine-level multipliers escape that: setSpeechRate and setVolume
//     take effect immediately and cost nothing, so rate and volume never
//     rewrite a .tav. Pitch does.

#pragma once

#include <windows.h>

#include <deque>
#include <string>
#include <vector>

#include "fv2_protocol.h"
#include "fv2_speaker.hpp"

namespace fv2 {

// One item out of the engine, in the order the engine produced it.
struct StreamItem {
    enum Kind { AUDIO, BOOKMARK, WORD, SENTENCE, DONE, FAILED };
    Kind                       kind = AUDIO;
    std::vector<unsigned char> audio;
    uint32_t                   value = 0;     // bookmark id
    uint32_t                   position = 0;  // word/sentence: character offset
    uint32_t                   length = 0;    // word/sentence: character count
    std::string                message;       // FAILED
};

// What to speak: text interleaved with bookmarks and silences.
struct Segment {
    Fv2SegmentKind kind = FV2_SEG_TEXT;
    uint32_t       value = 0;
    std::string    text;    // in the engine's codepage, already normalized
};

class Engine {
public:
    Engine();
    ~Engine();

    // `dataDir` is the directory holding the engine's data files -- the one
    // that contains RHL2.dat and Julie.bin, not its parent. Fails rather than
    // deferring to createEngine if the files the engine needs are absent.
    bool open(const std::string& dataDir, std::string& error);
    bool isOpen() const { return factory_ != nullptr; }

    // Which of the engine's required files are missing, if any. Used by open()
    // and by the host's diagnostics.
    static std::vector<std::string> missingDataFiles(const std::string& dataDir);

    // Install a voice. The definition is serialised to a .tav and loaded;
    // an identical definition is a no-op, so repeating this per utterance is
    // free. Changing the voice does not recreate the engine.
    bool selectVoice(const SpeakerDef& def, int sampleRate, std::string& error);

    // Queue an utterance. Returns immediately; results arrive through poll().
    // `rate` and `volume` are the engine-level multipliers, 1.0 for unchanged.
    bool speak(const std::vector<Segment>& segments,
               bool wantWords, bool wantSentences,
               double rate, double volume, std::string& error);

    // Pop the next stream item, or return false if none is ready yet.
    bool poll(StreamItem& out);

    // Block until poll() would return something, or the timeout expires.
    // Polling with Sleep(1) instead costs a full 15.6 ms timer tick per
    // utterance, which is most of a keystroke's latency budget.
    void waitForItem(uint32_t timeoutMs);

    // True once DONE or FAILED has been produced and drained.
    bool finished() const;

    // Abandon the current utterance. Safe to call when nothing is speaking.
    void stop();

    int sampleRate() const { return sampleRate_; }

private:
    class Site;
    friend class Site;

    void destroyEngine();
    bool createEngine(std::string& error);

    void* factory_ = nullptr;   // MM_TTSAPI::EngineFactory*
    void* engine_  = nullptr;   // MM_TTSAPI::Engine*
    void* speaker_ = nullptr;   // MM_TTSAPI::Speaker*
    Site* site_    = nullptr;

    std::string dataDir_;
    std::string tavPath_;       // scratch file the Speaker is loaded from
    std::string currentTav_;    // its contents, to skip identical reloads
    int         sampleRate_ = 16000;
    uint32_t    generation_ = 0;

    double      lastRate_ = 1.0;
    double      lastVolume_ = 1.0;
};

}  // namespace fv2
