// engine_test -- drive fv2::Engine the way the host will.
//
// Checks the things the SAPI wrapper depends on and that nothing else can
// check without the real engine present:
//
//   * every voice loads and produces audio at a sane level
//   * switching voices does not require a new engine, and is fast
//   * word boundaries and bookmarks come back, in order
//   * stop() interrupts an utterance promptly and leaves the engine usable
//   * the engine-level rate and volume multipliers work without touching a .tav
//
// 32-bit, like everything that touches FlexVoice_2_00_010.dll.

#include "../src/fv2_engine.hpp"
#include "../src/fv2_voices.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
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

double nowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

struct Drained {
    std::vector<unsigned char> audio;
    std::vector<uint32_t> bookmarks;
    int words = 0, sentences = 0;
    bool done = false, failed = false;
    std::string error;
    double firstAudioMs = -1.0;
};

Drained drain(fv2::Engine& eng, double startMs, unsigned budgetMs = 30000)
{
    Drained d;
    const double deadline = startMs + budgetMs;
    while (!d.done && !d.failed && nowMs() < deadline) {
        fv2::StreamItem it;
        if (!eng.poll(it)) {
            eng.waitForItem(50);
            continue;
        }
        switch (it.kind) {
        case fv2::StreamItem::AUDIO:
            if (d.firstAudioMs < 0) d.firstAudioMs = nowMs() - startMs;
            d.audio.insert(d.audio.end(), it.audio.begin(), it.audio.end());
            break;
        case fv2::StreamItem::BOOKMARK: d.bookmarks.push_back(it.value); break;
        case fv2::StreamItem::WORD: ++d.words; break;
        case fv2::StreamItem::SENTENCE: ++d.sentences; break;
        case fv2::StreamItem::DONE: d.done = true; break;
        case fv2::StreamItem::FAILED: d.failed = true; d.error = it.message; break;
        }
    }
    return d;
}

void level(const std::vector<unsigned char>& pcm, int* peak, double* dbfs)
{
    const short* s = (const short*)pcm.data();
    const size_t n = pcm.size() / 2;
    double sum = 0.0;
    int p = 0;
    for (size_t i = 0; i < n; ++i) {
        const int v = s[i] < 0 ? -s[i] : s[i];
        if (v > p) p = v;
        sum += (double)s[i] * s[i];
    }
    *peak = p;
    const double rms = n ? std::sqrt(sum / n) : 0.0;
    *dbfs = rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -999.0;
}

bool writeWav(const std::string& path, const std::vector<unsigned char>& pcm, int rate)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned n = (unsigned)pcm.size();
    const unsigned short ch = 1, tag = 1, align = 2, bits = 16;
    const unsigned riff = 36 + n, fmtsz = 16, brate = (unsigned)rate * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f);
    if (n) fwrite(pcm.data(), 1, n, f);
    fclose(f);
    return true;
}

std::vector<fv2::Segment> textOf(const std::string& s)
{
    fv2::Segment seg;
    seg.kind = FV2_SEG_TEXT;
    seg.text = s;
    return std::vector<fv2::Segment>(1, seg);
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (argc < 2) {
        std::printf("usage: engine_test <dataDir> [outDir]\n");
        return 2;
    }
    const std::string dataDir = argv[1];
    const std::string outDir = argc > 2 ? argv[2] : "";

    fv2::Engine eng;
    std::string err;

    std::printf("open\n");
    check(eng.open(dataDir, err), err.empty() ? "engine opens" : err.c_str());
    if (!eng.isOpen()) return 1;

    const std::string sentence =
        "The quick brown fox jumps over the lazy dog. "
        "She sells sea shells by the sea shore.";

    // ---- every voice renders, at a sane level ------------------------------
    std::printf("\nvoices\n");
    const std::vector<fv2::VoiceInfo>& voices = fv2::builtinVoices();
    for (size_t i = 0; i < voices.size(); ++i) {
        fv2::SpeakerDef def;
        const std::string tav = dataDir + "\\Voices\\" + voices[i].tavFile;
        if (!def.loadFile(tav, err)) { check(false, err.c_str()); continue; }
        def.set(fv2::P_VOLUME, voices[i].trimmedVolume);

        const double t0 = nowMs();
        if (!eng.selectVoice(def, 16000, err)) { check(false, err.c_str()); continue; }
        const double selectMs = nowMs() - t0;

        const double t1 = nowMs();
        if (!eng.speak(textOf(sentence), true, true, 1.0, 1.0, err)) {
            check(false, err.c_str());
            continue;
        }
        Drained d = drain(eng, t1);

        int peak = 0; double dbfs = 0;
        level(d.audio, &peak, &dbfs);
        std::printf("  %-8s %6.2fs  select %5.1f ms  first audio %5.1f ms  "
                    "peak %5d  %6.1f dBFS  %d words\n",
                    voices[i].name,
                    d.audio.size() / 32000.0, selectMs, d.firstAudioMs, peak, dbfs, d.words);

        check(d.done && !d.failed, "utterance completed");
        check(d.audio.size() > 16000, "produced a reasonable amount of audio");
        check(peak > 4000, "not silent");
        check(peak <= 32700, "does not clip with the measured trim");
        check(d.words > 10, "reported word boundaries");

        if (!outDir.empty()) {
            writeWav(outDir + "\\" + voices[i].name + ".wav", d.audio, eng.sampleRate());
        }
    }

    // ---- switching voices is cheap after the first ------------------------
    std::printf("\nvoice switching\n");
    {
        fv2::SpeakerDef a, b;
        a.loadFile(dataDir + "\\Voices\\Julie.tav", err);
        b.loadFile(dataDir + "\\Voices\\Bill.tav", err);
        double worst = 0.0;
        for (int i = 0; i < 6; ++i) {
            const double t = nowMs();
            eng.selectVoice((i % 2) ? b : a, 16000, err);
            const double ms = nowMs() - t;
            if (ms > worst) worst = ms;
        }
        std::printf("  worst switch %.1f ms\n", worst);
        check(worst < 250.0, "switching voices stays under 250 ms");

        // And an identical re-selection must cost nothing at all.
        const double t = nowMs();
        eng.selectVoice(a, 16000, err);
        eng.selectVoice(a, 16000, err);
        const double repeat = nowMs() - t;
        std::printf("  two identical re-selections %.3f ms\n", repeat);
        check(repeat < 5.0, "re-selecting the same voice is free");
    }

    // ---- bookmarks come back, in order ------------------------------------
    std::printf("\nbookmarks\n");
    {
        std::vector<fv2::Segment> segs;
        for (int i = 0; i < 3; ++i) {
            fv2::Segment t;
            t.kind = FV2_SEG_TEXT;
            t.text = (i == 0) ? "First part. " : (i == 1) ? "Second part. " : "Third part.";
            segs.push_back(t);
            fv2::Segment b;
            b.kind = FV2_SEG_BOOKMARK;
            b.value = 100 + i;
            segs.push_back(b);
        }
        const double t0 = nowMs();
        check(eng.speak(segs, false, false, 1.0, 1.0, err), "speak with bookmarks");
        Drained d = drain(eng, t0);
        std::printf("  got %u bookmarks\n", (unsigned)d.bookmarks.size());
        check(d.bookmarks.size() == 3, "all three bookmarks came back");
        bool ordered = d.bookmarks.size() == 3;
        for (size_t i = 0; i < d.bookmarks.size(); ++i) {
            if (d.bookmarks[i] != 100 + i) ordered = false;
        }
        check(ordered, "bookmarks arrived in order, including a trailing one");
    }

    // ---- stop() interrupts, and the engine survives ------------------------
    std::printf("\nstop\n");
    {
        std::string longText;
        for (int i = 0; i < 12; ++i) longText += sentence + " ";
        const double t0 = nowMs();
        check(eng.speak(textOf(longText), false, false, 1.0, 1.0, err), "speak a long utterance");
        // Let it get going, then interrupt.
        fv2::StreamItem it;
        while (nowMs() - t0 < 200 && !eng.poll(it)) eng.waitForItem(10);
        const double t1 = nowMs();
        eng.stop();
        const double stopMs = nowMs() - t1;
        std::printf("  stop() returned in %.1f ms\n", stopMs);
        check(stopMs < 250.0, "stop() returns promptly");

        // And the engine must still speak afterwards.
        const double t2 = nowMs();
        check(eng.speak(textOf("Still here."), false, false, 1.0, 1.0, err),
              "speaks again after stop()");
        Drained d = drain(eng, t2, 10000);
        check(d.done && d.audio.size() > 4000, "the utterance after stop() produced audio");
    }

    // ---- engine-level rate and volume --------------------------------------
    std::printf("\nrate and volume multipliers\n");
    {
        size_t slow = 0, fast = 0, quiet = 0, loud = 0;
        int peak = 0; double dbfs = 0;

        double t = nowMs();
        eng.speak(textOf(sentence), false, false, 0.5, 1.0, err);
        slow = drain(eng, t).audio.size();

        t = nowMs();
        eng.speak(textOf(sentence), false, false, 2.0, 1.0, err);
        fast = drain(eng, t).audio.size();

        std::printf("  half speed %u bytes, double speed %u bytes\n",
                    (unsigned)slow, (unsigned)fast);
        check(slow > fast * 3, "halving the rate roughly quadruples the duration vs doubling");

        t = nowMs();
        Drained q = drain((eng.speak(textOf(sentence), false, false, 1.0, 0.25, err), eng), t);
        level(q.audio, &peak, &dbfs);
        quiet = (size_t)peak;

        t = nowMs();
        Drained l = drain((eng.speak(textOf(sentence), false, false, 1.0, 1.0, err), eng), t);
        level(l.audio, &peak, &dbfs);
        loud = (size_t)peak;

        std::printf("  quarter volume peak %u, full volume peak %u\n",
                    (unsigned)quiet, (unsigned)loud);
        check(loud > quiet * 3, "volume multiplier scales the amplitude");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
