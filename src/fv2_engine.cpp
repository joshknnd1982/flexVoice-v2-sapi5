#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#  pragma warning(disable : 4996)
#  pragma warning(disable : 4250)   // dominance through virtual inheritance
#endif

#include "fv2_engine.hpp"
#include "fv2_log.h"
#include "ttsapi/fv2.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

using namespace MM_TTSAPI;

namespace fv2 {

namespace {

// The engine will not start without these. EngineFactory does not check --
// it accepts any path and lets createEngine fault on the first null -- so the
// check has to happen here, where it can say which file is missing.
//
// RHL2.dat and Julie.bin are absent from a plain file copy of FlexVoice 2.0:
// the original installer generates them from CHL2.dat and Julie.cod with
// FVZip.exe. If they are missing, the installer did not finish.
const char* const kRequiredFiles[] = {
    "RHL2.dat",            // generated: FVZip.exe chl2rhl { CHL2.dat RHL2.dat }
    "TTSHLTNData.dat",
    "POSset.dat",
    "POSStatistics.dat",
    "SpecialWordPOS.dat",
    "SimplePosDict.dat",
    "TN.dat",
    "PhonemeGroups",
    "L2P.id3",
    "NotPRTR.dat",
    "PRCS.dat",
};

// Needed the moment a voice is selected, rather than at factory time.
const char* const kRequiredVoiceFiles[] = {
    "Julie.bin",           // generated: FVZip.exe vq2bin { Julie.cod Julie.bin }
    "DurStat.dat",
    "VolumeStat.dat",
    "PhraseBoundSettings.dat",
    "PhraseBound.net",
};

bool fileExists(const std::string& path)
{
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string join(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    std::string r = dir;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    return r + name;
}

// A bookmark we allocate, so ours can be told from the engine's with
// dynamic_cast. Ownership passes to the site, which deletes it -- safe across
// the module boundary because Bookmark has a virtual destructor, so the
// deleting destructor in the vtable is the one our module compiled.
class IndexBookmark : public Bookmark
{
public:
    IndexBookmark(uint32_t gen, uint32_t idx)
        : generation(gen), index(idx), magic(0x46563258u)   // 'FV2X'
    {
        type = BM_USER;
    }
    virtual Bookmark* clone() const { return new IndexBookmark(*this); }
    uint32_t generation;
    uint32_t index;
    uint32_t magic;
};

}  // namespace

// ---------------------------------------------------------------------------
// Output site
// ---------------------------------------------------------------------------

class Engine::Site : public IWaveOutputSite
{
public:
    Site(int sampleRate, int bits)
        : fmt_(sampleRate, bits, WaveOutputFormat::WC_PCM_SIGNED)
    {
        InitializeCriticalSection(&cs_);
        // Auto-reset: one wake per item is exactly what the reader wants.
        ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Site()
    {
        if (ready_) CloseHandle(ready_);
        DeleteCriticalSection(&cs_);
    }

    HANDLE readyEvent() const { return ready_; }
    void signal() { if (ready_) SetEvent(ready_); }

    void beginUtterance(uint32_t gen, bool words, bool sentences,
                        const std::vector<uint32_t>& expectedBookmarks)
    {
        EnterCriticalSection(&cs_);
        generation_ = gen;
        wantWords_ = words;
        wantSentences_ = sentences;
        done_ = false;
        queue_.clear();
        pending_.assign(expectedBookmarks.begin(), expectedBookmarks.end());
        LeaveCriticalSection(&cs_);
    }

    // Stop accepting anything from the engine, used before Engine::stop() so a
    // chunk already in flight cannot land in the next utterance's queue.
    void gateOff()
    {
        EnterCriticalSection(&cs_);
        generation_ = 0;
        queue_.clear();
        pending_.clear();
        done_ = true;
        LeaveCriticalSection(&cs_);
        signal();
    }

    void push(const StreamItem& item)
    {
        EnterCriticalSection(&cs_);
        queue_.push_back(item);
        LeaveCriticalSection(&cs_);
        signal();
    }

    bool pop(StreamItem& out)
    {
        EnterCriticalSection(&cs_);
        const bool have = !queue_.empty();
        if (have) { out = queue_.front(); queue_.pop_front(); }
        LeaveCriticalSection(&cs_);
        return have;
    }

    bool finished()
    {
        EnterCriticalSection(&cs_);
        const bool d = done_ && queue_.empty();
        LeaveCriticalSection(&cs_);
        return d;
    }

    void fail(const std::string& msg)
    {
        StreamItem it;
        it.kind = StreamItem::FAILED;
        it.message = msg;
        EnterCriticalSection(&cs_);
        queue_.push_back(it);
        done_ = true;
        LeaveCriticalSection(&cs_);
        signal();
    }

    // Flush the queued bookmarks and end the utterance without asking the
    // engine for anything: used when a request carries no speakable text.
    void finishEmpty()
    {
        EnterCriticalSection(&cs_);
        for (size_t i = 0; i < pending_.size(); ++i) {
            StreamItem it;
            it.kind = StreamItem::BOOKMARK;
            it.value = pending_[i];
            queue_.push_back(it);
        }
        pending_.clear();
        StreamItem end;
        end.kind = StreamItem::DONE;
        queue_.push_back(end);
        done_ = true;
        LeaveCriticalSection(&cs_);
        signal();
    }

    // --- IWaveOutputSite ---------------------------------------------------
    virtual const IOutputFormat& getOutputFormat() const { return fmt_; }
    virtual void pause() {}
    virtual void play() {}
    virtual void clear() {}

    virtual void put(unsigned char* buffer, unsigned int size)
    {
        if (!buffer || !size) return;
        EnterCriticalSection(&cs_);
        if (generation_) {
            StreamItem it;
            it.kind = StreamItem::AUDIO;
            it.audio.assign(buffer, buffer + size);
            queue_.push_back(it);
        }
        LeaveCriticalSection(&cs_);
        signal();
    }

    virtual void setBookmark(Bookmark* bm)
    {
        if (!bm) return;
        handle(*bm);
        delete bm;                 // ownership was transferred to us
    }

    virtual void sendBookmark(Bookmark& bm) { handle(bm); }

    virtual void registerNotify(INotify*, const BookmarkTypeList&) {}
    virtual void unregisterNotify(INotify*) {}
    virtual void addBookmarkTypes(INotify*, const BookmarkTypeList&) {}
    virtual void removeBookmarkTypes(INotify*, const BookmarkTypeList&) {}

private:
    void handle(const Bookmark& bm)
    {
        EnterCriticalSection(&cs_);
        if (!generation_) { LeaveCriticalSection(&cs_); return; }

        if (const IndexBookmark* ib = dynamic_cast<const IndexBookmark*>(&bm)) {
            if (ib->magic == 0x46563258u && ib->generation == generation_) {
                StreamItem it;
                it.kind = StreamItem::BOOKMARK;
                it.value = ib->index;
                queue_.push_back(it);
                // Drop it from the pending list; whatever is left when the
                // utterance ends gets flushed, because a bookmark added after
                // the final text fragment never comes back on its own.
                for (size_t i = 0; i < pending_.size(); ++i) {
                    if (pending_[i] == ib->index) {
                        pending_.erase(pending_.begin() + i);
                        break;
                    }
                }
            }
            LeaveCriticalSection(&cs_);
            signal();
            return;
        }

        switch (bm.type) {
        case BM_WORD_BEGIN:
            if (wantWords_ && bm.pos >= 0 && bm.len > 0) {
                StreamItem it;
                it.kind = StreamItem::WORD;
                it.position = static_cast<uint32_t>(bm.pos);
                it.length = static_cast<uint32_t>(bm.len);
                queue_.push_back(it);
            }
            break;
        case BM_SENTENCE_BEGIN:
            if (wantSentences_ && bm.pos >= 0 && bm.len > 0) {
                StreamItem it;
                it.kind = StreamItem::SENTENCE;
                it.position = static_cast<uint32_t>(bm.pos);
                it.length = static_cast<uint32_t>(bm.len);
                queue_.push_back(it);
            }
            break;
        case BM_TEXT_END: {
            FV2_LOG("engine: BM_TEXT_END");
            // Any bookmark the engine never delivered is emitted here, in the
            // order it was queued, so a client waiting on the last bookmark of
            // an utterance is not left hanging.
            for (size_t i = 0; i < pending_.size(); ++i) {
                StreamItem it;
                it.kind = StreamItem::BOOKMARK;
                it.value = pending_[i];
                queue_.push_back(it);
            }
            pending_.clear();
            StreamItem it;
            it.kind = StreamItem::DONE;
            queue_.push_back(it);
            done_ = true;
            break;
        }
        default:
            break;
        }
        LeaveCriticalSection(&cs_);
        signal();
    }

    WaveOutputFormat      fmt_;
    CRITICAL_SECTION      cs_;
    HANDLE                ready_ = nullptr;
    std::deque<StreamItem> queue_;
    std::vector<uint32_t> pending_;
    uint32_t              generation_ = 0;
    bool                  wantWords_ = false;
    bool                  wantSentences_ = false;
    bool                  done_ = true;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() {}

Engine::~Engine()
{
    destroyEngine();

    if (speaker_) {
        static_cast<Speaker*>(speaker_)->~Speaker();
        ::operator delete(speaker_);
        speaker_ = nullptr;
    }
    if (factory_) {
        try { delete static_cast<EngineFactory*>(factory_); } catch (...) {}
        factory_ = nullptr;
    }
    delete site_;
    site_ = nullptr;

    if (!tavPath_.empty()) DeleteFileA(tavPath_.c_str());
}

std::vector<std::string> Engine::missingDataFiles(const std::string& dataDir)
{
    std::vector<std::string> missing;
    for (size_t i = 0; i < sizeof(kRequiredFiles) / sizeof(kRequiredFiles[0]); ++i) {
        if (!fileExists(join(dataDir, kRequiredFiles[i]))) missing.push_back(kRequiredFiles[i]);
    }
    for (size_t i = 0; i < sizeof(kRequiredVoiceFiles) / sizeof(kRequiredVoiceFiles[0]); ++i) {
        if (!fileExists(join(dataDir, kRequiredVoiceFiles[i]))) missing.push_back(kRequiredVoiceFiles[i]);
    }
    return missing;
}

bool Engine::open(const std::string& dataDir, std::string& error)
{
    if (factory_) return true;

    const std::vector<std::string> missing = missingDataFiles(dataDir);
    if (!missing.empty()) {
        error = "engine data is incomplete; missing: ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) error += ", ";
            error += missing[i];
        }
        if (std::find(missing.begin(), missing.end(), std::string("RHL2.dat")) != missing.end() ||
            std::find(missing.begin(), missing.end(), std::string("Julie.bin")) != missing.end()) {
            error += " (RHL2.dat and Julie.bin are generated by the installer "
                     "with FVZip.exe; reinstalling rebuilds them)";
        }
        FV2_LOG("open failed: %s", error.c_str());
        return false;
    }

    try {
        factory_ = new EngineFactory(dataDir.c_str());
    } catch (...) {
        error = "the FlexVoice 2.0 engine would not initialise from " + dataDir;
        FV2_LOG("open: EngineFactory threw");
        return false;
    }

    dataDir_ = dataDir;

    // One scratch file per host process; the Speaker is reloaded from it every
    // time the voice changes.
    char tmpDir[MAX_PATH] = {0};
    char tmpFile[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tmpDir);
    if (GetTempFileNameA(tmpDir, "fv2", 0, tmpFile)) tavPath_ = tmpFile;

    FV2_LOG("open: engine %s ready, data=%s", getVersion(), dataDir.c_str());
    return true;
}

bool Engine::selectVoice(const SpeakerDef& def, int sampleRate, std::string& error)
{
    if (!factory_) { error = "engine not open"; return false; }
    if (tavPath_.empty()) { error = "no scratch file for the speaker"; return false; }

    const std::string text = def.serialize();
    const bool sameVoice = (text == currentTav_);
    const bool sameRate = (sampleRate == sampleRate_);
    if (sameVoice && sameRate && engine_) return true;

    if (!sameVoice) {
        std::string werr;
        if (!def.saveFile(tavPath_, werr)) { error = werr; return false; }

        if (!speaker_) {
            // Over-allocated on purpose; see the note in ttsapi/fv2.hpp.
            speaker_ = ::operator new(sizeof(Speaker));
            new (speaker_) Speaker();
        }
        try {
            static_cast<Speaker*>(speaker_)->load(tavPath_.c_str());
        } catch (...) {
            error = "the engine rejected the generated voice file";
            FV2_LOG("selectVoice: Speaker::load threw");
            return false;
        }
        currentTav_ = text;
    }

    // The sample rate is baked into the output site, and the site is baked into
    // the engine, so changing it means a new engine. Changing only the voice
    // does not: setSpeaker takes effect without one.
    if (!engine_ || !sameRate) {
        sampleRate_ = sampleRate;
        destroyEngine();
        delete site_;
        site_ = new Site(sampleRate_, 16);
        if (!createEngine(error)) return false;
    } else {
        try {
            static_cast<MM_TTSAPI::Engine*>(engine_)->setSpeaker(
                *static_cast<Speaker*>(speaker_), true);
        } catch (...) {
            // Fall back to a fresh engine rather than speaking in the old voice.
            FV2_LOG("selectVoice: setSpeaker threw, recreating the engine");
            destroyEngine();
            if (!createEngine(error)) return false;
        }
    }
    return true;
}

bool Engine::createEngine(std::string& error)
{
    if (!factory_ || !speaker_ || !site_) { error = "engine not ready"; return false; }
    try {
        std::auto_ptr<MM_TTSAPI::Engine> e =
            static_cast<EngineFactory*>(factory_)->createEngine(
                site_, *static_cast<Speaker*>(speaker_), (Language)0);
        engine_ = e.release();
    } catch (...) {
        error = "the engine could not be created for this voice";
        FV2_LOG("createEngine threw");
        return false;
    }
    if (!engine_) { error = "the engine could not be created for this voice"; return false; }

    lastRate_ = 1.0;
    lastVolume_ = 1.0;
    return true;
}

void Engine::destroyEngine()
{
    if (!engine_) return;
    MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);
    // stop() first, always. Deleting a running engine blocks forever on its
    // worker thread, and it looks exactly like a hang after a good render.
    try { e->stop(); } catch (...) {}
    try { delete e; } catch (...) {}
    engine_ = nullptr;
}

bool Engine::speak(const std::vector<Segment>& segments,
                   bool wantWords, bool wantSentences,
                   double rate, double volume, std::string& error)
{
    if (!engine_ || !site_) { error = "no voice selected"; return false; }
    MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);

    // Collect the bookmark ids up front so any the engine swallows can be
    // flushed at BM_TEXT_END rather than silently lost.
    std::vector<uint32_t> bookmarks;
    bool haveText = false;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].kind == FV2_SEG_BOOKMARK) bookmarks.push_back(segments[i].value);
        else if ((segments[i].kind == FV2_SEG_TEXT || segments[i].kind == FV2_SEG_SPELL) &&
                 !segments[i].text.empty()) {
            haveText = true;
        }
    }

    ++generation_;
    if (generation_ == 0) ++generation_;
    site_->beginUtterance(generation_, wantWords, wantSentences, bookmarks);

    if (!haveText) {
        // Nothing to say. Report the bookmarks and finish without troubling
        // the engine, which would otherwise never produce BM_TEXT_END.
        site_->finishEmpty();
        return true;
    }

    try {
        if (rate != lastRate_) { e->setSpeechRate(rate, false); lastRate_ = rate; }
        if (volume != lastVolume_) { e->setVolume(volume, false); lastVolume_ = volume; }

        for (size_t i = 0; i < segments.size(); ++i) {
            const Segment& s = segments[i];
            switch (s.kind) {
            case FV2_SEG_TEXT:
            case FV2_SEG_SPELL:
                if (!s.text.empty()) e->addFragment(s.text.c_str());
                break;
            case FV2_SEG_BOOKMARK:
                e->addBookmark(new IndexBookmark(generation_, s.value));
                break;
            case FV2_SEG_SILENCE:
                // The engine has no silence primitive we can reach without
                // embedded commands, and embedded commands would make client
                // text ambiguous. Emitting the samples ourselves is exact.
                {
                    StreamItem it;
                    it.kind = StreamItem::AUDIO;
                    const size_t bytes =
                        (size_t)((double)sampleRate_ * (double)s.value / 1000.0) * 2u;
                    it.audio.assign(bytes, 0);
                    site_->push(it);
                }
                break;
            case FV2_SEG_RATE:
                {
                    const double r = rate * (s.value / 100.0);
                    e->setSpeechRate(r, false);
                    lastRate_ = r;
                }
                break;
            case FV2_SEG_VOLUME:
                {
                    const double v = volume * (s.value / 100.0);
                    e->setVolume(v, false);
                    lastVolume_ = v;
                }
                break;
            case FV2_SEG_PITCH:
                // Pitch lives on the Speaker, and swapping speakers mid-stream
                // would cut the utterance. The wrapper applies pitch before the
                // request instead; nothing to do here.
                break;
            }
        }
        e->speakRequest((int)generation_);
    } catch (...) {
        error = "the engine refused this utterance";
        FV2_LOG("speak: threw");
        site_->fail(error);
        return false;
    }
    return true;
}

bool Engine::poll(StreamItem& out)
{
    return site_ ? site_->pop(out) : false;
}

void Engine::waitForItem(uint32_t timeoutMs)
{
    if (!site_) return;
    WaitForSingleObject(site_->readyEvent(), timeoutMs);
}

bool Engine::finished() const
{
    return site_ ? site_->finished() : true;
}

void Engine::stop()
{
    if (!engine_ || !site_) return;
    // Gate the site first: a chunk already on its way from the engine's worker
    // thread must not land in the next utterance's queue.
    site_->gateOff();
    try { static_cast<MM_TTSAPI::Engine*>(engine_)->stop(); } catch (...) {}
}

}  // namespace fv2
