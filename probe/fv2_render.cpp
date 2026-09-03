// fv2_render -- render text through FlexVoice 2.0 with arbitrary parameter
// overrides, and write a WAV.
//
// This is the workhorse behind the sample set and the parameter sweeps. It also
// proves the two things everything else depends on:
//
//   * that src/fv2_speaker.cpp can read Mindmaker's .tav files and write files
//     the engine accepts (--roundtrip renders once from the original and once
//     from a re-serialised copy and compares the PCM), and
//   * that a parameter written into a generated .tav actually changes the audio.
//
// Because FlexVoice 2.0 has no attribute interface, generating a .tav is the
// only way to set a synthesis parameter, so this path is not a test convenience
// -- it is how the shipping wrapper works.

#include "../src/ttsapi/fv2.hpp"
#include "../src/fv2_speaker.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace MM_TTSAPI;

namespace {

const int kRate = 16000;
const int kBits = 16;

class Site : public IWaveOutputSite
{
public:
    Site() : fmt_(kRate, kBits, WaveOutputFormat::WC_PCM_SIGNED)
    {
        InitializeCriticalSection(&cs_);
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~Site() { CloseHandle(done_); DeleteCriticalSection(&cs_); }

    void put(unsigned char* b, unsigned int n) override
    {
        EnterCriticalSection(&cs_);
        audio.insert(audio.end(), b, b + n);
        LeaveCriticalSection(&cs_);
    }
    const IOutputFormat& getOutputFormat() const override { return fmt_; }
    void pause() override {}
    void play() override {}
    void clear() override {}
    void setBookmark(Bookmark* b) override { if (b) { note(*b); delete b; } }
    void sendBookmark(Bookmark& b) override { note(b); }
    void registerNotify(INotify*, const BookmarkTypeList&) override {}
    void unregisterNotify(INotify*) override {}
    void addBookmarkTypes(INotify*, const BookmarkTypeList&) override {}
    void removeBookmarkTypes(INotify*, const BookmarkTypeList&) override {}

    void reset() { audio.clear(); ResetEvent(done_); }
    bool wait(DWORD ms) { return WaitForSingleObject(done_, ms) == WAIT_OBJECT_0; }

    std::vector<unsigned char> audio;

private:
    void note(const Bookmark& b)
    {
        if (b.type == BM_TEXT_END) SetEvent(done_);
    }
    WaveOutputFormat fmt_;
    CRITICAL_SECTION cs_;
    HANDLE done_;
};

bool writeWav(const std::string& path, const std::vector<unsigned char>& pcm)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned int n = (unsigned)pcm.size();
    const unsigned short ch = 1, tag = 1, align = (unsigned short)(kBits / 8);
    const unsigned int riff = 36 + n, fmtsz = 16, rate = kRate;
    const unsigned int brate = kRate * (kBits / 8);
    const unsigned short bits = kBits;
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

std::string tempTav()
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "fv2", 0, path);
    return path;
}

// Speaker lives in an over-allocated buffer; see fv2.hpp.
struct SpeakerHolder {
    std::vector<unsigned char> raw;
    Speaker* sp;
    SpeakerHolder() : raw(sizeof(Speaker) + 4096, 0) { sp = new (raw.data()) Speaker(); }
    ~SpeakerHolder() { sp->~Speaker(); }
};

bool renderWith(const std::string& dataDir, const std::string& tavPath,
                const std::string& text, std::vector<unsigned char>& out,
                double engineRate, double engineVolume, unsigned* renderMs)
{
    EngineFactory factory(dataDir.c_str());
    SpeakerHolder holder;
    try {
        holder.sp->load(tavPath.c_str());
    } catch (...) {
        fprintf(stderr, "Speaker::load failed for %s\n", tavPath.c_str());
        return false;
    }

    Site site;
    // English is 0 in FlexVoice 2.0's Language enum -- read off the push at
    // FVWrapper.dll's own createEngine call site.
    std::auto_ptr<Engine> eng = factory.createEngine(&site, *holder.sp, (Language)0);
    if (!eng.get()) return false;

    if (engineRate != 1.0) eng->setSpeechRate(engineRate, false);
    if (engineVolume != 1.0) eng->setVolume(engineVolume, false);

    const DWORD t0 = GetTickCount();
    eng->addFragment(text.c_str());
    eng->speakRequest(1);
    // Engine::wait() never returns with a client-supplied site; the engine's
    // own BM_TEXT_END bookmark is the completion signal.
    const bool ok = site.wait(60000);
    if (renderMs) *renderMs = GetTickCount() - t0;
    if (!ok) fprintf(stderr, "timed out waiting for BM_TEXT_END\n");
    out = site.audio;

    // BM_TEXT_END means the last sample has been handed to the site, not that
    // the engine's worker thread has finished with it. Deleting the Engine
    // without stopping it first blocks forever waiting on that thread.
    try { eng->stop(); } catch (...) {}
    try { delete eng.release(); } catch (...) {}
    return ok;
}

int usage()
{
    printf("usage:\n");
    printf("  fv2_render --data <dir> --tav <file> --text <s> --out <wav>\n");
    printf("             [--set key=value]...   raw .tav key\n");
    printf("             [--pct name=percent]... named parameter, 0-100\n");
    printf("             [--rate x] [--volume x] engine-level multipliers\n");
    printf("  fv2_render --data <dir> --tav <file> --roundtrip\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    std::string dataDir, tavPath, text = "Hello world.", outPath, saveTav;
    std::vector<std::pair<std::string, std::string> > sets;
    std::vector<std::pair<std::string, double> > pcts;
    double engineRate = 1.0, engineVolume = 1.0;
    bool roundtrip = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--data") dataDir = next();
        else if (a == "--tav") tavPath = next();
        else if (a == "--text") text = next();
        else if (a == "--out") outPath = next();
        else if (a == "--rate") engineRate = atof(next().c_str());
        else if (a == "--volume") engineVolume = atof(next().c_str());
        else if (a == "--roundtrip") roundtrip = true;
        else if (a == "--save-tav") saveTav = next();
        else if (a == "--set") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) sets.push_back(std::make_pair(kv.substr(0, eq), kv.substr(eq + 1)));
        } else if (a == "--pct") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) pcts.push_back(std::make_pair(kv.substr(0, eq), atof(kv.c_str() + eq + 1)));
        } else return usage();
    }
    if (dataDir.empty() || tavPath.empty()) return usage();

    // ---- read the base voice ----------------------------------------------
    fv2::SpeakerDef def;
    std::string err;
    if (!def.loadFile(tavPath, err)) {
        fprintf(stderr, "parse failed: %s\n", err.c_str());
        return 1;
    }

    if (roundtrip) {
        // Render from Mindmaker's file, then from ours, and compare the PCM.
        std::vector<unsigned char> a, b;
        unsigned ms = 0;
        if (!renderWith(dataDir, tavPath, text, a, 1.0, 1.0, &ms)) return 1;

        const std::string tmp = tempTav();
        if (!def.saveFile(tmp, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        if (!renderWith(dataDir, tmp, text, b, 1.0, 1.0, &ms)) return 1;
        DeleteFileA(tmp.c_str());

        printf("original  : %u bytes\n", (unsigned)a.size());
        printf("round-trip: %u bytes\n", (unsigned)b.size());
        if (a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0)) {
            printf("IDENTICAL - the parser and serialiser preserve the voice exactly\n");
            return 0;
        }
        printf("DIFFERENT - a re-serialised .tav does not reproduce the voice\n");
        return 1;
    }

    // ---- apply overrides ---------------------------------------------------
    for (size_t i = 0; i < pcts.size(); ++i) {
        bool found = false;
        for (int k = 0; k < fv2::P_COUNT; ++k) {
            if (pcts[i].first == fv2::kParams[k].tavKey) {
                def.set((fv2::ParamId)k, fv2::percentToValue((fv2::ParamId)k, pcts[i].second));
                found = true;
                break;
            }
        }
        if (!found) fprintf(stderr, "unknown parameter %s\n", pcts[i].first.c_str());
    }
    for (size_t i = 0; i < sets.size(); ++i) {
        const std::string& k = sets[i].first;
        const std::string& v = sets[i].second;
        if (k == "gender") def.gender = v;
        else if (k == "age") def.age = v;
        else if (k == "voiceDescr") def.voiceDescr = v;
        else if (k == "durationDescr") def.durationDescr = v;
        else if (k == "volumeDescr") def.volumeDescr = v;
        else if (k == "eqTrim") def.applyEqualizerTrim(atof(v.c_str()));
        else {
            bool found = false;
            for (int p = 0; p < fv2::P_COUNT; ++p) {
                if (k == fv2::kParams[p].tavKey) {
                    def.set((fv2::ParamId)p, atof(v.c_str()));
                    found = true;
                    break;
                }
            }
            if (!found) fprintf(stderr, "unknown key %s\n", k.c_str());
        }
    }

    if (!saveTav.empty()) {
        if (!def.saveFile(saveTav, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        printf("wrote %s\n", saveTav.c_str());
        return 0;
    }

    const std::string tmp = tempTav();
    if (!def.saveFile(tmp, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

    std::vector<unsigned char> pcm;
    unsigned ms = 0;
    const bool ok = renderWith(dataDir, tmp, text, pcm, engineRate, engineVolume, &ms);
    DeleteFileA(tmp.c_str());
    if (!ok) return 1;

    // Level report, so a sweep can spot clipping and silence without a
    // separate pass over the file.
    double sum = 0.0; int peak = 0;
    const short* s = (const short*)pcm.data();
    const size_t n = pcm.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const int v = s[i] < 0 ? -s[i] : s[i];
        if (v > peak) peak = v;
        sum += (double)s[i] * s[i];
    }
    const double rms = n ? std::sqrt(sum / n) : 0.0;
    printf("samples=%u seconds=%.3f ms=%u rtf=%.1f peak=%d rms=%.1f dbfs=%.1f\n",
           (unsigned)n, (double)n / kRate, ms,
           ms ? ((double)n / kRate) * 1000.0 / ms : 0.0,
           peak, rms, rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -999.0);

    if (!outPath.empty() && !writeWav(outPath, pcm)) {
        fprintf(stderr, "could not write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}
