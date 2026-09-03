// client_test -- exercise the whole path a screen reader takes: client, pipe,
// host, engine, and back.
//
// The number that matters is not throughput. It is how long after speak()
// the first audio byte arrives, because that is the gap a user hears between
// pressing an arrow key and the machine starting to talk. It is measured with
// QueryPerformanceCounter: GetTickCount's 15.6 ms granularity reports a flat
// "15 ms" for anything faster, which is exactly the range of interest.

#include "../src/fv2_client.h"
#include "../src/fv2_log.h"
#include "../src/fv2_voices.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    ++g_checks;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

double now()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

std::vector<fv2::SpeakSegment> text(const std::string& s)
{
    fv2::SpeakSegment seg;
    seg.kind = FV2_SEG_TEXT;
    seg.text = s;
    return std::vector<fv2::SpeakSegment>(1, seg);
}

struct Result {
    size_t bytes = 0;
    double firstAudioMs = -1.0;
    int    words = 0;
    std::vector<uint32_t> bookmarks;
};

// Speak and collect. `cancelAfter` > 0 cancels once that many bytes arrived.
Result speak(const std::string& s, const std::string& voice, double rate,
             std::string& err, size_t cancelAfter = 0, bool words = false)
{
    Result r;
    fv2::SpeakParams p;
    p.baseVoice = voice;
    p.rate = rate;
    p.wantWordEvents = words;

    const double t0 = now();
    fv2::SpeakSink sink;
    sink.audio = [&](const unsigned char*, uint32_t n) -> bool {
        if (r.firstAudioMs < 0) r.firstAudioMs = now() - t0;
        r.bytes += n;
        return !(cancelAfter && r.bytes >= cancelAfter);
    };
    sink.word = [&](uint32_t, uint32_t) -> bool { ++r.words; return true; };
    sink.bookmark = [&](uint32_t id) -> bool { r.bookmarks.push_back(id); return true; };

    fv2::sharedClient().speak(p, text(s), sink, err);
    return r;
}

}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    fv2::log::component() = L"clienttest";

    std::string err;

    std::printf("ping\n");
    {
        // The first ping pays for launching the host and loading the engine.
        const double t0 = now();
        Result warm = speak("Warming up.", "Julie", 1.0, err);
        std::printf("  cold start (launch + engine load + first utterance): %.0f ms\n",
                    now() - t0);
        check(warm.bytes > 0, "the host started and spoke");
        if (warm.bytes == 0) {
            std::printf("  error: %s\n", err.c_str());
            return 1;
        }
    }

    std::printf("\nvoice list\n");
    {
        std::string list;
        check(fv2::sharedClient().getVoices(list, err), "getVoices");
        int lines = 0;
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == '\n') ++lines;
        std::printf("  %d voices\n", lines);
        check(lines == (int)fv2::builtinVoices().size(), "every built-in voice is listed");
    }

    std::printf("\nevery voice speaks through the pipe\n");
    for (size_t i = 0; i < fv2::builtinVoices().size(); ++i) {
        const char* name = fv2::builtinVoices()[i].name;
        Result r = speak("The quick brown fox jumps over the lazy dog.", name, 1.0,
                         err, 0, true);
        std::printf("  %-8s %6.2fs  first audio %5.1f ms  %d words\n",
                    name, r.bytes / 32000.0, r.firstAudioMs, r.words);
        check(r.bytes > 20000, "produced audio");
        check(r.words > 5, "reported word boundaries");
    }

    std::printf("\nkeystroke latency (what arrowing through a document feels like)\n");
    {
        // Short utterances, each cancelled almost immediately, exactly as a
        // screen reader does when the user keeps moving.
        std::vector<double> latencies;
        for (int i = 0; i < 25; ++i) {
            Result r = speak("Line one of the document.", "Julie", 1.0, err, 4000);
            if (r.firstAudioMs >= 0) latencies.push_back(r.firstAudioMs);
        }
        std::sort(latencies.begin(), latencies.end());
        if (!latencies.empty()) {
            const double median = latencies[latencies.size() / 2];
            const double worst = latencies.back();
            std::printf("  %u utterances: median %.1f ms, worst %.1f ms\n",
                        (unsigned)latencies.size(), median, worst);
            check(median < 60.0, "median first-audio latency is under 60 ms");
            check(worst < 250.0, "worst first-audio latency is under 250 ms");
        }
        check(latencies.size() >= 24, "every cancelled utterance still produced audio");
    }

    std::printf("\nrate multiplier over the wire\n");
    {
        Result slow = speak("The quick brown fox jumps over the lazy dog.", "Julie", 0.5, err);
        Result fast = speak("The quick brown fox jumps over the lazy dog.", "Julie", 2.0, err);
        std::printf("  half speed %u bytes, double speed %u bytes\n",
                    (unsigned)slow.bytes, (unsigned)fast.bytes);
        check(slow.bytes > fast.bytes * 3, "rate reaches the engine");
    }

    std::printf("\nbookmarks over the wire\n");
    {
        std::vector<fv2::SpeakSegment> segs;
        for (int i = 0; i < 3; ++i) {
            fv2::SpeakSegment t;
            t.kind = FV2_SEG_TEXT;
            t.text = "Part. ";
            segs.push_back(t);
            fv2::SpeakSegment b;
            b.kind = FV2_SEG_BOOKMARK;
            b.value = 7000 + i;
            segs.push_back(b);
        }
        std::vector<uint32_t> got;
        fv2::SpeakParams p;
        fv2::SpeakSink sink;
        sink.audio = [](const unsigned char*, uint32_t) { return true; };
        sink.bookmark = [&](uint32_t id) { got.push_back(id); return true; };
        check(fv2::sharedClient().speak(p, segs, sink, err), "speak with bookmarks");
        std::printf("  got %u bookmarks\n", (unsigned)got.size());
        check(got.size() == 3, "all three came back");
    }

    std::printf("\nthe host survives a cancelled utterance\n");
    {
        speak("A very long sentence that will certainly be interrupted well before "
              "it finishes, several times over, to be sure.", "Julie", 1.0, err, 2000);
        Result after = speak("Still here.", "Julie", 1.0, err);
        check(after.bytes > 4000, "speaks again after a cancel");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
