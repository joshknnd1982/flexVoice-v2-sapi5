// fv2_probe -- first contact with FlexVoice 2.0.
//
// This exists to answer, by running the engine rather than by reading it:
//   1. Does the reconstructed header in src/ttsapi/fv2.hpp actually bind?
//   2. What path does EngineFactory want?
//   3. What are the values of v2's Language enum? (Nothing in the DLL says.)
//   4. Does a .tav load, and does the engine render audio through a
//      client-supplied IWaveOutputSite?
//   5. How many bytes of our over-allocated Speaker does the engine touch?
//
// Usage:
//   fv2_probe langsweep <dataRoot> <tav>
//   fv2_probe speak     <dataRoot> <tav> <out.wav> <language> "text"
//
// Built 32-bit: FlexVoice_2_00_010.dll is x86 and there is no 64-bit build of
// it anywhere, which is why the shipping wrapper needs a 32-bit host process.

#include "../src/ttsapi/fv2.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace MM_TTSAPI;

namespace {

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

// Set by --trace: log every engine->site callback, so a crash inside
// createEngine can be pinned to the last call that got through.
bool g_trace = false;
#define TRACE(...) do { if (g_trace) logf(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// A minimal wave sink. Everything the engine needs and nothing else.
// ---------------------------------------------------------------------------

class CaptureSite : public IWaveOutputSite
{
public:
    CaptureSite(int sampleRate, int bits)
        : fmt_(sampleRate, bits, WaveOutputFormat::WC_PCM_SIGNED)
    {
        InitializeCriticalSection(&cs_);
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~CaptureSite()
    {
        if (done_) CloseHandle(done_);
        DeleteCriticalSection(&cs_);
    }

    // IWaveOutputSite
    void put(unsigned char* buffer, unsigned int size) override
    {
        TRACE("    [site] put(%p, %u)", (void*)buffer, size);
        EnterCriticalSection(&cs_);
        audio_.insert(audio_.end(), buffer, buffer + size);
        ++putCalls_;
        LeaveCriticalSection(&cs_);
    }

    // IOutputSite
    const IOutputFormat& getOutputFormat() const override
    {
        TRACE("    [site] getOutputFormat()");
        return fmt_;
    }
    void pause() override { TRACE("    [site] pause()"); }
    void play()  override { TRACE("    [site] play()"); }
    void clear() override
    {
        EnterCriticalSection(&cs_);
        audio_.clear();
        LeaveCriticalSection(&cs_);
    }

    void setBookmark(Bookmark* bookmark) override
    {
        TRACE("    [site] setBookmark(%p)", (void*)bookmark);
        if (!bookmark) return;
        noteBookmark(*bookmark);
        delete bookmark;              // ownership passes to the site
    }
    void sendBookmark(Bookmark& bookmark) override
    {
        TRACE("    [site] sendBookmark()");
        noteBookmark(bookmark);
    }

    // INotifyDispatcher
    void registerNotify(INotify*, const BookmarkTypeList&) override
    { TRACE("    [site] registerNotify()"); }
    void unregisterNotify(INotify*) override
    { TRACE("    [site] unregisterNotify()"); }
    void addBookmarkTypes(INotify*, const BookmarkTypeList&) override
    { TRACE("    [site] addBookmarkTypes()"); }
    void removeBookmarkTypes(INotify*, const BookmarkTypeList&) override
    { TRACE("    [site] removeBookmarkTypes()"); }

    bool waitDone(DWORD ms) const { return WaitForSingleObject(done_, ms) == WAIT_OBJECT_0; }

    std::vector<unsigned char> audio_;
    std::vector<int>           marks_;
    int                        putCalls_ = 0;
    bool                       sawEnd_   = false;
    bool                       badType_  = false;

private:
    void noteBookmark(const Bookmark& b)
    {
        const int t = static_cast<int>(b.type);
        // If the reconstructed Bookmark layout were wrong this is where it
        // would show: a type outside the enum means we are reading the wrong
        // offset, and nothing downstream should be believed.
        if (t < 0 || t >= BM_NO_OF_BOOKMARKS) badType_ = true;
        marks_.push_back(t);
        if (b.type == BM_TEXT_END) {
            sawEnd_ = true;
            SetEvent(done_);
        }
    }

    WaveOutputFormat fmt_;
    CRITICAL_SECTION cs_;
    HANDLE           done_ = nullptr;
};

bool writeWav(const char* path, const std::vector<unsigned char>& pcm, int rate, int bits)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    const unsigned int dataBytes = static_cast<unsigned int>(pcm.size());
    const unsigned short channels = 1;
    const unsigned int byteRate = static_cast<unsigned int>(rate) * channels * (bits / 8);
    const unsigned short blockAlign = static_cast<unsigned short>(channels * (bits / 8));
    const unsigned int riffSize = 36 + dataBytes;
    const unsigned int fmtSize = 16;
    const unsigned short pcmTag = 1;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&pcmTag, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    if (dataBytes) fwrite(pcm.data(), 1, dataBytes, f);
    fclose(f);
    return true;
}

// Construct a Speaker into a poisoned buffer so we can see how far the engine
// writes, and check the pad in fv2.hpp is generous rather than lucky.
struct PoisonedSpeaker {
    static const size_t SLACK = 8192;
    std::vector<unsigned char> raw;
    Speaker* sp = nullptr;

    PoisonedSpeaker() : raw(sizeof(Speaker) + SLACK, 0xCD)
    {
        sp = new (raw.data()) Speaker();
    }
    ~PoisonedSpeaker() { if (sp) sp->~Speaker(); }

    size_t touched() const
    {
        for (size_t i = raw.size(); i-- > 0; ) {
            if (raw[i] != 0xCD) return i + 1;
        }
        return 0;
    }
    bool overflowed() const { return touched() > sizeof(Speaker); }
};

}  // namespace

int main(int argc, char** argv)
{
    // The engine calls abort() on some malformed input. Without this a probe
    // run stops on a modal dialog instead of returning a exit code.
    setvbuf(stdout, nullptr, _IONBF, 0);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--trace") == 0) { g_trace = true; }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    if (argc < 4) {
        logf("usage: fv2_probe langsweep <dataRoot> <tav>");
        logf("       fv2_probe speak <dataRoot> <tav> <out.wav> <language> \"text\"");
        return 2;
    }

    const std::string mode = argv[1];
    const std::string root = argv[2];
    const std::string tav  = argv[3];

    logf("fv2_probe: mode=%s", mode.c_str());
    logf("  dataRoot = %s", root.c_str());
    logf("  tav      = %s", tav.c_str());
    logf("  sizeof(Speaker)       = %u", (unsigned)sizeof(Speaker));
    logf("  sizeof(EngineFactory) = %u", (unsigned)sizeof(EngineFactory));
    logf("  sizeof(WaveOutputFormat) = %u", (unsigned)sizeof(WaveOutputFormat));

    try {
        logf("  engine version = %s", getVersion());
    } catch (...) {
        logf("  getVersion() threw");
    }

    // ---- factory -----------------------------------------------------------
    EngineFactory* factory = nullptr;
    try {
        factory = new EngineFactory(root.c_str());
        logf("OK: EngineFactory constructed");
    } catch (...) {
        logf("FAIL: EngineFactory(\"%s\") threw", root.c_str());
        return 1;
    }

    // ---- speaker -----------------------------------------------------------
    PoisonedSpeaker speaker;
    try {
        speaker.sp->load(tav.c_str());
        logf("OK: Speaker::load  (engine touched %u bytes of %u declared%s)",
             (unsigned)speaker.touched(), (unsigned)sizeof(Speaker),
             speaker.overflowed() ? "  *** OVERFLOW ***" : "");
    } catch (...) {
        logf("FAIL: Speaker::load(\"%s\") threw", tav.c_str());
        return 1;
    }

    // ---- language sweep ----------------------------------------------------
    if (mode == "langsweep") {
        // Two hypotheses: v2 uses Windows LCIDs like 3.01 does, or it uses a
        // plain 0,1,2... enum. Try both ranges and report every value the
        // factory will actually build an engine for.
        std::vector<int> candidates;
        for (int i = 0; i <= 32; ++i) candidates.push_back(i);
        const int lcids[] = { 0x0405, 0x0407, 0x0409, 0x040c, 0x040e, 0x0410,
                              0x0413, 0x0415, 0x0419, 0x041d, 0x043e, 0x0809,
                              0x0c09, 0x1009 };
        for (int l : lcids) candidates.push_back(l);

        int found = 0;
        for (int cand : candidates) {
            CaptureSite site(16000, 16);
            try {
                std::auto_ptr<Engine> eng =
                    factory->createEngine(&site, *speaker.sp, (Language)cand);
                if (eng.get()) {
                    logf("  LANGUAGE 0x%04x (%d): engine CREATED", cand, cand);
                    ++found;
                }
            } catch (...) {
                // Expected for every language whose data is not present.
            }
        }
        logf("langsweep: %d language value(s) accepted", found);
        delete factory;
        return found > 0 ? 0 : 1;
    }

    // ---- speak -------------------------------------------------------------
    if (mode != "speak" || argc < 7) {
        logf("FAIL: 'speak' needs <out.wav> <language> \"text\"");
        delete factory;
        return 2;
    }

    const std::string outWav = argv[4];
    const int language = (int)strtol(argv[5], nullptr, 0);
    const std::string text = argv[6];

    const int rate = 16000, bits = 16;
    CaptureSite site(rate, bits);

    try {
        std::auto_ptr<Engine> eng =
            factory->createEngine(&site, *speaker.sp, (Language)language);
        if (!eng.get()) {
            logf("FAIL: createEngine returned null");
            delete factory;
            return 1;
        }
        logf("OK: createEngine (language 0x%04x)", language);

        const DWORD t0 = GetTickCount();
        eng->addFragment(text.c_str());
        eng->speakRequest(1);
        logf("OK: speakRequest issued");

        // Prefer the engine's own completion bookmark; fall back to wait().
        if (!site.waitDone(15000)) {
            logf("  no BM_TEXT_END within 15 s -- calling wait()");
            eng->wait();
        }
        const DWORD ms = GetTickCount() - t0;

        logf("RESULT: %u bytes PCM in %u put() calls, %u bookmarks, %s, %u ms",
             (unsigned)site.audio_.size(), site.putCalls_,
             (unsigned)site.marks_.size(),
             site.sawEnd_ ? "saw BM_TEXT_END" : "NO BM_TEXT_END",
             (unsigned)ms);
        if (site.badType_) {
            logf("WARNING: a bookmark type was outside the enum -- the "
                 "reconstructed Bookmark layout is wrong");
        }
        if (!site.audio_.empty()) {
            const double seconds = double(site.audio_.size()) / (rate * (bits / 8));
            logf("        %.3f s of audio, real-time factor %.1fx",
                 seconds, ms ? seconds * 1000.0 / ms : 0.0);
            if (writeWav(outWav.c_str(), site.audio_, rate, bits)) {
                logf("OK: wrote %s", outWav.c_str());
            } else {
                logf("FAIL: could not write %s", outWav.c_str());
            }
        }
        logf("  Speaker touched %u bytes%s",
             (unsigned)speaker.touched(), speaker.overflowed() ? "  *** OVERFLOW ***" : "");
    } catch (...) {
        logf("FAIL: speaking threw");
        delete factory;
        return 1;
    }

    delete factory;
    logf("done");
    return 0;
}
