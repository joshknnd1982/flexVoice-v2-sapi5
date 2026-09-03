// flexvoice_protocol.h -- wire protocol between the SAPI5 DLLs (32- and
// 64-bit) / the configuration utility and the 32-bit flexvoice_host.exe.
//
// The FlexVoice engine is a 32-bit DLL, so the 64-bit SAPI DLL cannot load it.
// It is also willing to call abort() on malformed input -- feeding a
// FlexVoice 2.0 .tav to the 3.01 engine does exactly that -- and an abort()
// inside a screen reader's process would take the screen reader down with it.
// So *both* bitnesses talk to a separate host process. The host is cheap to
// restart; NVDA is not.
//
// Framing: an 8-byte header followed by `size` payload bytes, over a byte-mode
// named pipe. One request is in flight at a time; the client cancels by
// closing the handle, which fails the host's next write.

#pragma once

#include <stdint.h>

#define FLEXVOICE_PIPE_NAME      L"\\\\.\\pipe\\FlexVoiceTTS"
#define FLEXVOICE_SERVER_MUTEX   L"Local\\FlexVoiceTTSHostMutex"
#define FLEXVOICE_LAUNCH_MUTEX   L"Local\\FlexVoiceTTSLaunchMutex"

// Bumped whenever the structs below change. The host echoes it in FV_RESP_PONG;
// a client that sees a different number shuts the stale host down and relaunches,
// so an upgrade over a running host is not a silent field failure.
#define FLEXVOICE_PROTOCOL_VERSION 2u

// A single message may never be larger than this. Anything bigger is treated as
// a desynchronised stream and drops the connection rather than allocating it.
#define FLEXVOICE_MAX_MESSAGE (32u * 1024u * 1024u)

enum FlexVoiceCommand : uint32_t {
    FV_CMD_PING        = 0,
    FV_CMD_SPEAK       = 1,
    FV_CMD_GET_VOICES  = 2,   // returns the roster the host discovered on disk
    FV_CMD_SHUTDOWN    = 3,
};

enum FlexVoiceResponse : uint32_t {
    FV_RESP_PONG       = 0,   // payload: FlexVoicePong
    FV_RESP_AUDIO      = 1,   // payload: raw PCM
    FV_RESP_BOOKMARK   = 2,   // payload: uint32 bookmark id
    FV_RESP_WORD       = 3,   // payload: FlexVoiceBoundary
    FV_RESP_SENTENCE   = 4,   // payload: FlexVoiceBoundary
    FV_RESP_END        = 5,   // utterance complete
    FV_RESP_ERROR      = 6,   // payload: UTF-8 message
    FV_RESP_VOICES     = 7,   // payload: UTF-8, one "name\tlanguage\tgender\tage" per line
};

// Segment kinds. Note what is *not* here: there is no way for a client to send
// the engine a raw embedded command. The engine's escape character is a
// backslash with no way to quote it, so any text carrying one would be parsed
// as a command and swallowed; the normalizer therefore turns every backslash
// into the word "backslash". Mid-utterance rate, pitch and volume changes are
// sent as numbers instead, and the host formats the command itself.
enum FlexVoiceSegmentKind : uint32_t {
    FV_SEG_TEXT     = 0,   // value = byte count of text in the engine's codepage
    FV_SEG_BOOKMARK = 1,   // value = bookmark id, no payload
    FV_SEG_SILENCE  = 2,   // value = milliseconds, no payload
    FV_SEG_SPELL    = 3,   // value = byte count; spelled out letter by letter
    FV_SEG_RATE     = 4,   // value = speed relative to the voice's own, percent
    FV_SEG_PITCH    = 5,   // value = pitch relative to the voice's own, percent
    FV_SEG_VOLUME   = 6,   // value = volume, percent
};

// Parameters the wrapper can override per utterance. Indices into
// FlexVoiceSpeakRequest::params; `paramMask` says which ones are meaningful.
//
// FlexVoice has two levels and they are not interchangeable:
//
//   * The Speaker carries the voice's character in absolute units -- volume on
//     a roughly 0..45 amplitude scale, defaultPitch in hertz, and so on.
//   * The Engine's own attribute() exposes exactly three multipliers over that
//     -- speechRate, volume and pitchRate, all 1.0 by default -- and they can
//     be changed mid-utterance in under a millisecond.
//
// So the client's rate/volume/pitch scaling belongs on the Engine, and the
// voice's identity belongs on the Speaker. The three marked ENGINE below are
// applied through attribute(); everything else is set on the Speaker.
//
// Only parameters that were *measured* to change the audio are here. The
// engine also accepts speedWPM, creakiness, singingPitchRate and
// volumeSmoothWindow, but sweeping each of them across its whole range left
// the rendered waveform bit-identical, so they are deliberately omitted.
enum FlexVoiceParam : uint32_t {
    FVP_SPEECH_RATE = 0,   // ENGINE  double, duration scale; 1.0 is unchanged
    FVP_VOLUME,            // ENGINE  double, gain; 1.0 is unchanged, 0.0 silent
    FVP_DEFAULT_PITCH,     // speaker int-valued, base F0 in Hz
    FVP_PITCH_RATE,        // ENGINE  double, multiplies the whole F0 contour
    FVP_PITCH_MIN,         // int-valued, F0 floor in Hz  (must be > 0)
    FVP_PITCH_MAX,         // int-valued, F0 ceiling in Hz
    FVP_INTONATION,        // double, F0 variance; 0 is monotone
    FVP_HEADSIZE,          // double, vocal tract scale
    FVP_TILT,              // double, spectral tilt      (must be >= 0)
    FVP_RICHNESS,          // double
    FVP_BREATHINESS,       // double
    FVP_SMOOTHNESS,        // double                     (must be >= 0)
    FVP_FRICATION,         // double, fricative energy   (must be >= 0)
    FVP_PLOSIVE,           // double, plosive burst energy (must be >= 0)
    FVP_COUNT
};

#pragma pack(push, 1)

struct FlexVoiceMessageHeader {
    uint32_t type;
    uint32_t size;          // payload bytes following this header
};

struct FlexVoicePong {
    uint32_t protocolVersion;
    uint32_t engineReady;    // 1 once the EngineFactory and language are loaded
    uint32_t voiceCount;
};

struct FlexVoiceBoundary {
    uint32_t position;      // character offset into the request's own text
    uint32_t length;        // characters
};

struct FlexVoiceSpeakRequest {
    uint32_t voiceIndex;    // index into the host's discovered roster
    uint32_t language;      // MM_TTSAPI::Language, e.g. 0x0409
    uint32_t sampleRate;    // 8000 / 11025 / 16000 / 22050 / 32000 / 44100
    uint32_t paramMask;     // bit i set => params[i] is meaningful
    double   params[FVP_COUNT];
    uint32_t wantWordEvents;
    uint32_t wantSentenceEvents;
    uint32_t segmentCount;
    // followed by segmentCount x FlexVoiceSegmentHeader (+ payload)
};

struct FlexVoiceSegmentHeader {
    uint32_t kind;
    uint32_t value;
};

#pragma pack(pop)
