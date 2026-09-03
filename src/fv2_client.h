// fv2_client.h -- client side of the pipe to fv2_host.exe.
//
// Used by both SAPI DLLs and by the configuration utility, so it must not
// assume a COM apartment or a window.

#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "fv2_protocol.h"
#include "fv2_speaker.hpp"

namespace fv2 {

struct SpeakParams {
    // One of the built-in voice names ("Julie", "Bill", ...). The Custom Voice
    // is not a name the host knows: the wrapper resolves it to whichever base
    // voice the user chose, plus the overrides below.
    std::string baseVoice = "Julie";

    // Engine-level multipliers. These are the fast path -- they apply
    // immediately and never rewrite a voice file, which is what keeps a rate
    // change off the critical path of a keystroke.
    double rate = 1.0;
    double volume = 1.0;

    bool wantWordEvents = false;
    bool wantSentenceEvents = false;

    // .tav parameters that differ from the base voice, in engine units.
    std::vector<Fv2ParamOverride> overrides;

    void set(ParamId p, double v)
    {
        for (size_t i = 0; i < overrides.size(); ++i) {
            if (overrides[i].id == (uint32_t)p) { overrides[i].value = v; return; }
        }
        Fv2ParamOverride o;
        o.id = (uint32_t)p;
        o.value = v;
        overrides.push_back(o);
    }
};

struct SpeakSegment {
    Fv2SegmentKind kind = FV2_SEG_TEXT;
    uint32_t       value = 0;
    std::string    text;
};

// Callbacks run on the calling thread, inside speak(). Returning false from any
// of them cancels the utterance: the client drops the pipe, which fails the
// host's next write and makes it stop the engine.
struct SpeakSink {
    std::function<bool(const unsigned char*, uint32_t)> audio;
    std::function<bool(uint32_t)>                       bookmark;
    std::function<bool(uint32_t, uint32_t)>             word;
    std::function<bool(uint32_t, uint32_t)>             sentence;
};

class EngineClient {
public:
    EngineClient();
    ~EngineClient();

    // Speak one utterance, streaming results into `sink`. Returns false and
    // fills `error` on a real failure; a caller-requested cancel returns true.
    bool speak(const SpeakParams& params, const std::vector<SpeakSegment>& segments,
               const SpeakSink& sink, std::string& error);

    // "name\tlanguage\tgender\tage\ttavFile" lines.
    bool getVoices(std::string& out, std::string& error);

    bool ping(Fv2Pong& pong, std::string& error);

    // Ask a running host to exit. Used by the installer and the uninstaller.
    static bool shutdownServer();
    static bool isServerRunning();

private:
    bool ensureConnected(std::string& error);
    void disconnect();
    bool launchServer(std::string& error);
    bool sendMessage(uint32_t type, const void* payload, uint32_t size);
    bool readHeader(Fv2MessageHeader& h);
    bool readPayload(void* data, uint32_t size);
    bool skipPayload(uint32_t size);
    bool speakOnce(const SpeakParams& params, const std::vector<SpeakSegment>& segments,
                   const SpeakSink& sink, std::string& error, bool& cancelled);

    HANDLE           pipe_ = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION lock_;
    bool             versionChecked_ = false;
};

// One client per process, created on first use.
EngineClient& sharedClient();

}  // namespace fv2
