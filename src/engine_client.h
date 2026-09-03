#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

#include "flexvoice_protocol.h"

// Client side of the pipe to flexvoice_host.exe. Used by both SAPI DLLs and by
// the configuration utility, so it must not assume a COM apartment or a window.

namespace FlexVoice {

struct SpeakParams {
    uint32_t voiceIndex = 0;
    uint32_t language = 0x0409;
    uint32_t sampleRate = 16000;
    uint32_t paramMask = 0;
    double   params[FVP_COUNT] = {};
    bool     wantWordEvents = false;
    bool     wantSentenceEvents = false;

    void set(FlexVoiceParam p, double v)
    {
        params[p] = v;
        paramMask |= (1u << p);
    }
};

struct SpeakSegment {
    FlexVoiceSegmentKind kind = FV_SEG_TEXT;
    uint32_t             value = 0;
    std::string          text;   // UTF-8
};

// Callbacks run on the calling thread, inside speak(). Returning false from
// any of them cancels the utterance: the client drops the pipe, which fails
// the host's next write and makes it stop the engine.
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

    // "index\tdisplayName\tlangCode\tgender\tage" lines for the voices whose
    // language data is actually installed.
    bool getVoices(std::string& out, std::string& error);

    bool ping(FlexVoicePong& pong, std::string& error);

    // Ask a running host to exit. Used by the installer and the uninstaller.
    static bool shutdownServer();
    static bool isServerRunning();

private:
    bool ensureConnected(std::string& error);
    void disconnect();
    bool launchServer(std::string& error);
    bool sendMessage(uint32_t type, const void* payload, uint32_t size);
    bool readHeader(FlexVoiceMessageHeader& h);
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

}  // namespace FlexVoice
