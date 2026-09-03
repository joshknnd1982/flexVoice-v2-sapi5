// fv2_protocol.h -- wire protocol between the SAPI 5 DLLs (32- and 64-bit) and
// the configuration utility on one side, and the 32-bit fv2_host.exe on the
// other.
//
// FlexVoice_2_00_010.dll is a 32-bit DLL, so the 64-bit SAPI DLL cannot load
// it. It is also willing to fault on malformed input, and a fault inside a
// screen reader's process would take the screen reader down with it. So *both*
// bitnesses talk to a separate host process. The host is cheap to restart;
// NVDA is not.
//
// Framing: an 8-byte header followed by `size` payload bytes, over a byte-mode
// named pipe. One request is in flight at a time; a client cancels by closing
// its handle, which fails the host's next write.

#pragma once

#include <stdint.h>

#define FV2_PIPE_NAME       L"\\\\.\\pipe\\FlexVoice2TTS"
#define FV2_SERVER_MUTEX    L"Local\\FlexVoice2TTSHostMutex"
#define FV2_LAUNCH_MUTEX    L"Local\\FlexVoice2TTSLaunchMutex"

// Bumped whenever anything below changes. The host echoes it in FV2_RESP_PONG;
// a client that sees a different number shuts the stale host down and relaunches,
// so upgrading over a running host is not a silent field failure.
#define FV2_PROTOCOL_VERSION 1u

// A single message may never be larger than this. Anything bigger is treated as
// a desynchronised stream and drops the connection rather than allocating it.
#define FV2_MAX_MESSAGE (32u * 1024u * 1024u)

enum Fv2Command : uint32_t {
    FV2_CMD_PING       = 0,
    FV2_CMD_SPEAK      = 1,
    FV2_CMD_GET_VOICES = 2,   // the roster the host found on disk
    FV2_CMD_SHUTDOWN   = 3,
};

enum Fv2Response : uint32_t {
    FV2_RESP_PONG      = 0,   // payload: Fv2Pong
    FV2_RESP_AUDIO     = 1,   // payload: raw PCM
    FV2_RESP_BOOKMARK  = 2,   // payload: uint32 bookmark id
    FV2_RESP_WORD      = 3,   // payload: Fv2Boundary
    FV2_RESP_SENTENCE  = 4,   // payload: Fv2Boundary
    FV2_RESP_END       = 5,   // utterance complete
    FV2_RESP_ERROR     = 6,   // payload: UTF-8 message
    FV2_RESP_VOICES    = 7,   // payload: UTF-8, one voice per line (see below)
};

// Segment kinds. Note what is *not* here: there is no way for a client to send
// the engine a raw embedded command. Mid-utterance rate, pitch and volume
// changes are sent as numbers and the host applies them, so no client text can
// ever be mistaken for a command.
enum Fv2SegmentKind : uint32_t {
    FV2_SEG_TEXT     = 0,   // spoken as written
    FV2_SEG_BOOKMARK = 1,   // value = bookmark id, no text
    FV2_SEG_SILENCE  = 2,   // value = milliseconds, no text
    FV2_SEG_SPELL    = 3,   // spelled out letter by letter
    FV2_SEG_RATE     = 4,   // value = percent of the voice's own rate
    FV2_SEG_PITCH    = 5,   // value = percent of the voice's own pitch
    FV2_SEG_VOLUME   = 6,   // value = percent
};

// Flags in Fv2SpeakRequest::flags.
#define FV2_SPEAK_WORDS      0x0001u   // report word boundaries
#define FV2_SPEAK_SENTENCES  0x0002u   // report sentence boundaries

#pragma pack(push, 1)

struct Fv2MessageHeader {
    uint32_t type;
    uint32_t size;          // payload bytes following this header
};

struct Fv2Pong {
    uint32_t protocolVersion;
    uint32_t engineReady;   // 1 once the factory and a voice have loaded
    uint32_t voiceCount;
    uint32_t sampleRate;
};

struct Fv2Boundary {
    uint32_t position;      // character offset into the request's own text
    uint32_t length;        // characters
};

// A speech request is this header, then the voice name, then `paramCount`
// Fv2ParamOverride records, then `segmentCount` segments (each an Fv2Segment
// followed by its text).
//
// The client names a voice and sends only the parameters it wants to differ
// from that voice's own .tav; the host owns the voice files. rate and volume
// are engine-level multipliers applied with setSpeechRate/setVolume, which
// take effect immediately and never rewrite a .tav -- that is what keeps a
// rate change off the critical path.
struct Fv2SpeakRequest {
    uint32_t flags;
    double   rate;          // 1.0 = the voice's own speed
    double   volume;        // 1.0 = the voice's own loudness
    uint32_t voiceBytes;    // UTF-8 voice name, follows this struct
    uint32_t paramCount;
    uint32_t segmentCount;
};

// An override of one .tav parameter, identified by fv2::ParamId. Values are in
// the parameter's own engine units, not percent: the percentage scale belongs
// to the user interface, and converting it once at the edge means the host
// never has to know what a control looked like.
struct Fv2ParamOverride {
    uint32_t id;
    double   value;
};

struct Fv2Segment {
    uint32_t kind;
    uint32_t value;
    uint32_t textBytes;     // text follows, no terminator
};

#pragma pack(pop)

// FV2_RESP_VOICES payload format: one line per voice,
//     name \t language \t gender \t age \t tavFileName
// so a client can build its token list without reading the engine directory.
