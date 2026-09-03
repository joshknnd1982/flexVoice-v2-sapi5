// client_test.cpp -- exercise the pipe protocol without a SAPI host.
//
// Built for both architectures on purpose: the 64-bit build is the only thing
// that proves the 64 -> 32 bridge works before the SAPI DLL is involved.
//
//   client_test ping
//   client_test voices
//   client_test speak <voiceIndex> <out.wav> [text]
//   client_test cancel <voiceIndex>       measure how fast a cancel takes effect
//   client_test bench  <voiceIndex>       first-audio latency over 20 utterances

#ifdef _MSC_VER
#  pragma warning(disable : 4996)
#endif

#include <windows.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "engine_client.h"
#include "flexvoice_log.h"
#include "settings.h"
#include "voice_data.hpp"

using namespace FlexVoice;

namespace {

// GetTickCount has a 15.6 ms granularity, which is the same order as the
// numbers being measured here -- it reported a flat "15 ms" for a latency that
// is actually a couple of milliseconds. Everything timed in this file uses the
// performance counter instead.
double now_ms()
{
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (1000.0 * t.QuadPart) / freq.QuadPart;
}

const char* kText =
    "The quick brown fox jumps over the lazy dog. "
    "FlexVoice, speaking through the pipe from a "
#ifdef _WIN64
    "sixty four bit"
#else
    "thirty two bit"
#endif
    " client.";

bool write_wav(const char* path, const std::vector<unsigned char>& pcm, int rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const unsigned dataLen = static_cast<unsigned>(pcm.size());
    const unsigned riff = 36 + dataLen, fmtLen = 16, byteRate = rate * 2;
    const unsigned short tag = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataLen, 4, 1, f);
    if (dataLen) fwrite(pcm.data(), 1, dataLen, f);
    fclose(f);
    return true;
}

SpeakParams params_for(int voiceIndex)
{
    SpeakParams p;
    const int vi = (voiceIndex >= 0 && voiceIndex < voices::kVoiceCount) ? voiceIndex : 0;
    const voices::Voice& v = voices::kVoices[vi];
    p.voiceIndex = static_cast<uint32_t>(vi);
    p.language = voices::kLanguages[v.languageIndex].id;
    p.sampleRate = 16000;
    p.wantWordEvents = true;
    p.wantSentenceEvents = true;

    static const char* kNames[FVP_COUNT] = {
        "speechRate", "volume", "defaultPitch", "pitchRate", "pitchMin", "pitchMax",
        "intonationLevel", "headsize", "tilt", "richness", "breathiness",
        "smoothness", "fricationRate", "plosiveRate" };
    for (int k = 0; k < v.overrideCount; ++k) {
        for (int i = 0; i < FVP_COUNT; ++i) {
            if (strcmp(v.overrides[k].name, kNames[i]) == 0) {
                p.set(static_cast<FlexVoiceParam>(i), v.overrides[k].value);
                break;
            }
        }
    }
    if (v.isCustom) {
        for (int i = 0; i < FVP_COUNT; ++i) {
            const auto q = static_cast<FlexVoiceParam>(i);
            p.set(q, voices::percent_to_value(voices::param(q),
                                              voices::param(q).defaultPercent));
        }
    }
    p.set(FVP_SPEECH_RATE, 1.0);
    p.set(FVP_VOLUME, v.levelTrim);   // the voice's own measured level
    p.set(FVP_PITCH_RATE, 1.0);
    return p;
}

int cmd_ping()
{
    FlexVoicePong pong = {};
    std::string error;
    if (!sharedClient().ping(pong, error)) {
        printf("ping FAILED: %s\n", error.c_str());
        return 1;
    }
    printf("pong: protocol %u, engine %s, %u voices\n", pong.protocolVersion,
           pong.engineReady ? "ready" : "not loaded", pong.voiceCount);
    return 0;
}

int cmd_voices()
{
    std::string out, error;
    if (!sharedClient().getVoices(out, error)) {
        printf("getVoices FAILED: %s\n", error.c_str());
        return 1;
    }
    printf("%s", out.c_str());
    return 0;
}

int cmd_speak(int voiceIndex, const char* outPath, const char* text, double gain)
{
    SpeakParams p = params_for(voiceIndex);
    // An explicit engine-level gain, so level calibration can measure a voice
    // below the host's limiter knee where the response is still linear.
    if (gain > 0) p.set(FVP_VOLUME, gain);
    std::vector<SpeakSegment> segs;
    SpeakSegment s;
    s.kind = FV_SEG_TEXT;
    s.text = text ? text : kText;
    segs.push_back(s);
    SpeakSegment b;
    b.kind = FV_SEG_BOOKMARK;
    b.value = 42;
    segs.push_back(b);

    std::vector<unsigned char> pcm;
    int words = 0, sentences = 0, marks = 0;
    SpeakSink sink;
    sink.audio = [&](const unsigned char* d, uint32_t n) {
        pcm.insert(pcm.end(), d, d + n); return true; };
    sink.word = [&](uint32_t pos, uint32_t len) {
        if (words < 6) printf("  word at %u len %u\n", pos, len);
        ++words; return true; };
    sink.sentence = [&](uint32_t, uint32_t) { ++sentences; return true; };
    sink.bookmark = [&](uint32_t id) { ++marks; printf("  bookmark %u\n", id); return true; };

    const DWORD t0 = GetTickCount();
    std::string error;
    if (!sharedClient().speak(p, segs, sink, error)) {
        printf("speak FAILED: %s\n", error.c_str());
        return 1;
    }
    const DWORD ms = GetTickCount() - t0;

    printf("voice %d (%S): %u bytes (%.2f s) in %lu ms, %d words, %d sentences, %d bookmarks\n",
           voiceIndex, voices::kVoices[voiceIndex].displayName,
           static_cast<unsigned>(pcm.size()), pcm.size() / 32000.0,
           static_cast<unsigned long>(ms), words, sentences, marks);

    if (outPath && !pcm.empty()) {
        if (write_wav(outPath, pcm, static_cast<int>(p.sampleRate))) {
            printf("wrote %s\n", outPath);
        }
    }
    return pcm.empty() ? 1 : 0;
}

// Send one utterance split across several text segments, which is what the
// SAPI wrapper does whenever a stream carries per-fragment rate or pitch.
int cmd_frags(int voiceIndex, int count)
{
    SpeakParams p = params_for(voiceIndex);
    std::vector<SpeakSegment> segs;
    static const char* kPieces[] = { "Alpha bravo. ", "Charlie delta. ",
                                     "Echo foxtrot. ", "Golf hotel. ",
                                     "India juliet. ", "Kilo lima." };
    for (int i = 0; i < count; ++i) {
        SpeakSegment s;
        s.kind = FV_SEG_TEXT;
        s.text = kPieces[i % 6];
        segs.push_back(s);
    }
    size_t bytes = 0;
    SpeakSink sink;
    sink.audio = [&](const unsigned char*, uint32_t n) { bytes += n; return true; };
    const DWORD t0 = GetTickCount();
    std::string error;
    const bool ok = sharedClient().speak(p, segs, sink, error);
    printf("%d fragment(s): %s, %u bytes in %lu ms%s%s\n", count,
           ok ? "ok" : "FAILED", static_cast<unsigned>(bytes),
           static_cast<unsigned long>(GetTickCount() - t0),
           error.empty() ? "" : " - ", error.c_str());
    return (ok && bytes) ? 0 : 1;
}

// Send text as a spell-out segment, which is what SAPI does for <spell> and
// what NVDA uses for its spell-word command.
int cmd_spell(int voiceIndex, const char* outPath, const char* text)
{
    SpeakParams p = params_for(voiceIndex);
    std::vector<SpeakSegment> segs(1);
    segs[0].kind = FV_SEG_SPELL;
    segs[0].text = text ? text : "NVDA";

    std::vector<unsigned char> pcm;
    SpeakSink sink;
    sink.audio = [&](const unsigned char* d, uint32_t n) {
        pcm.insert(pcm.end(), d, d + n); return true; };
    std::string error;
    const bool ok = sharedClient().speak(p, segs, sink, error);
    printf("spell \"%s\": %s, %u bytes (%.2f s)%s%s\n", segs[0].text.c_str(),
           ok ? "ok" : "FAILED", static_cast<unsigned>(pcm.size()),
           pcm.size() / 32000.0, error.empty() ? "" : " - ", error.c_str());
    if (outPath && !pcm.empty()) write_wav(outPath, pcm, static_cast<int>(p.sampleRate));
    return pcm.empty() ? 1 : 0;
}

int cmd_cancel(int voiceIndex)
{
    SpeakParams p = params_for(voiceIndex);
    std::vector<SpeakSegment> segs(1);
    segs[0].kind = FV_SEG_TEXT;
    segs[0].text =
        "This is a long passage that will be cut short. One two three four five "
        "six seven eight nine ten. Eleven twelve thirteen fourteen fifteen. "
        "Sixteen seventeen eighteen nineteen twenty. Twenty one twenty two.";

    for (int trial = 0; trial < 5; ++trial) {
        size_t got = 0;
        DWORD cancelAt = 0;
        DWORD lastAudio = 0;
        SpeakSink sink;
        sink.audio = [&](const unsigned char*, uint32_t n) -> bool {
            got += n;
            lastAudio = GetTickCount();
            if (got > 8000) {                 // ~0.25 s of audio, then cancel
                cancelAt = GetTickCount();
                return false;
            }
            return true;
        };
        std::string error;
        const DWORD t0 = GetTickCount();
        sharedClient().speak(p, segs, sink, error);
        const DWORD total = GetTickCount() - t0;
        printf("  trial %d: cancelled after %u bytes; speak() returned %lu ms "
               "after the cancel\n", trial, static_cast<unsigned>(got),
               static_cast<unsigned long>(cancelAt ? GetTickCount() - cancelAt : total));
    }
    return 0;
}

// What arrowing through a document actually looks like: speak, cancel almost
// immediately, speak again. The cancel drops the pipe, so this measures the
// reconnect as well as the render -- which cmd_bench, speaking one utterance
// after another to completion, never touches.
int cmd_arrow(int voiceIndex)
{
    SpeakParams p = params_for(voiceIndex);
    std::vector<SpeakSegment> segs(1);
    segs[0].kind = FV_SEG_TEXT;
    segs[0].text = "This is a line of text in a document that gets interrupted.";

    double worst = 0, total = 0;
    const int kRounds = 20;
    for (int i = 0; i < kRounds; ++i) {
        double firstAudio = 0;
        size_t got = 0;
        const double t0 = now_ms();
        SpeakSink sink;
        sink.audio = [&](const unsigned char*, uint32_t n) -> bool {
            if (firstAudio == 0) firstAudio = now_ms() - t0;
            got += n;
            return got < 6000;          // about 190 ms of audio, then arrow away
        };
        std::string error;
        sharedClient().speak(p, segs, sink, error);
        total += firstAudio;
        if (firstAudio > worst) worst = firstAudio;
        printf("  press %2d: first audio %6.2f ms\n", i, firstAudio);
    }
    printf("arrow-key latency over %d presses: mean %.2f ms, worst %.2f ms\n",
           kRounds, total / kRounds, worst);
    return 0;
}

int cmd_bench(int voiceIndex)
{
    SpeakParams p = params_for(voiceIndex);
    std::vector<SpeakSegment> segs(1);
    segs[0].kind = FV_SEG_TEXT;
    segs[0].text = "The quick brown fox jumps over the lazy dog.";

    DWORD worstFirst = 0, totalFirst = 0;
    for (int i = 0; i < 20; ++i) {
        DWORD firstAudio = 0;
        const DWORD t0 = GetTickCount();
        SpeakSink sink;
        sink.audio = [&](const unsigned char*, uint32_t) -> bool {
            if (!firstAudio) firstAudio = GetTickCount() - t0;
            return true;
        };
        std::string error;
        if (!sharedClient().speak(p, segs, sink, error)) {
            printf("  utterance %d FAILED: %s\n", i, error.c_str());
            return 1;
        }
        totalFirst += firstAudio;
        if (firstAudio > worstFirst) worstFirst = firstAudio;
    }
    printf("first-audio latency over 20 utterances: mean %.2f ms, worst %.2f ms\n",
           totalFirst / 20, worstFirst);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    FlexVoice::log::component() = L"clienttest";
    FlexVoice::log::enabled() = true;

    if (argc < 2) {
        printf("usage: client_test ping | voices | speak <n> <out.wav> [text] | "
               "cancel <n> | bench <n>\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "ping") return cmd_ping();
    if (cmd == "voices") return cmd_voices();
    if (cmd == "speak") {
        return cmd_speak(argc > 2 ? atoi(argv[2]) : 0,
                         argc > 3 ? argv[3] : nullptr,
                         argc > 4 ? argv[4] : nullptr,
                         argc > 5 ? atof(argv[5]) : 0.0);
    }
    if (cmd == "cancel") return cmd_cancel(argc > 2 ? atoi(argv[2]) : 0);
    if (cmd == "arrow") return cmd_arrow(argc > 2 ? atoi(argv[2]) : 0);
    if (cmd == "spell") {
        return cmd_spell(argc > 2 ? atoi(argv[2]) : 0,
                         argc > 3 ? argv[3] : nullptr,
                         argc > 4 ? argv[4] : nullptr);
    }
    if (cmd == "frags") {
        return cmd_frags(argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? atoi(argv[3]) : 4);
    }
    if (cmd == "bench") return cmd_bench(argc > 2 ? atoi(argv[2]) : 0);
    printf("unknown command \"%s\"\n", cmd.c_str());
    return 2;
}
