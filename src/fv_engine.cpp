#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#  pragma warning(disable : 4996)
#  pragma warning(disable : 4250)   // dominance through virtual inheritance
#endif

#include "fv_engine.hpp"
#include "flexvoice_log.h"
#include "text_normalize.h"
#include "voice_data.hpp"

#include <ttsapi/Engine.h>
#include <ttsapi/Speaker.h>
#include <ttsapi/Bookmark.h>
#include <ttsapi/FVLanguage.h>
#include <ttsapi/FVVersion.h>

#include <algorithm>
#include <memory>

using namespace MM_TTSAPI;

namespace FlexVoice {

namespace {

// Every clamp here has a measured reason; see the table in voice_data.hpp and
// the crash hunt in probe/fv_stream2.cpp.
struct Limit { double lo, hi; };

const Limit kLimits[FVP_COUNT] = {
    /* FVP_SPEECH_RATE   */ { 0.05,   12.0   },  // engine-level multiplier
    /* FVP_VOLUME        */ { 0.0,    8.0    },  // engine-level gain, 1.0 = unity
    /* FVP_DEFAULT_PITCH */ { 20.0,   600.0  },
    /* FVP_PITCH_RATE    */ { 0.1,    4.0    },
    /* FVP_PITCH_MIN     */ { 1.0,    400.0  },  // 0 throws
    /* FVP_PITCH_MAX     */ { 60.0,   2000.0 },
    /* FVP_INTONATION    */ { 0.0,    12.0   },  // negative throws
    /* FVP_HEADSIZE      */ { 0.1,    4.0    },
    /* FVP_TILT          */ { 0.0,    8.0    },  // negative throws
    /* FVP_RICHNESS      */ { 0.0,    4.0    },
    /* FVP_BREATHINESS   */ { 0.0,    3.0    },
    /* FVP_SMOOTHNESS    */ { 0.0,    3.0    },  // negative throws
    /* FVP_FRICATION     */ { 0.0,    6.0    },  // negative throws
    /* FVP_PLOSIVE       */ { 0.0,    6.0    },  // negative throws
};

// The three multipliers the Engine exposes through attribute(); everything
// else is a Speaker attribute. Setting an engine-level value on the Speaker
// would be silently wrong: Speaker "volume" is an absolute amplitude around
// 25..45, so a 1.0 there is near silence rather than unity gain.
const bool kEngineLevel[FVP_COUNT] = {
    true,   /* speechRate */
    true,   /* volume     */
    false,  /* defaultPitch */
    true,   /* pitchRate  */
    false, false, false, false, false, false, false, false, false, false,
};

const char* kAttrName[FVP_COUNT] = {
    "speechRate", "volume", "defaultPitch", "pitchRate", "pitchMin", "pitchMax",
    "intonationLevel", "headsize", "tilt", "richness", "breathiness",
    "smoothness", "fricationRate", "plosiveRate",
};

const bool kAttrIsInt[FVP_COUNT] = {
    false, false, true, false, true, true, false, false, false, false, false,
    false, false, false,
};

// Last line of defence. Clients are expected to have normalized already --
// the SAPI wrapper has to, because it needs the offset map for word events --
// but nothing that reaches the engine may be trusted to have done so. A single
// numeral getting through faults or wedges the engine, and a wedged engine
// cannot even be destroyed. Normalization is idempotent, so running it twice
// costs a little time and nothing else.
std::string sanitize(const std::string& in)
{
    return text::normalize_bytes(in);
}

std::string joinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    std::string r = a;
    if (r.back() != '\\' && r.back() != '/') r += '\\';
    return r + b;
}

}  // namespace

double clampParam(FlexVoiceParam p, double v)
{
    if (p < 0 || p >= FVP_COUNT) return v;
    const Limit& l = kLimits[p];
    if (!(v == v)) return l.lo;               // NaN
    return v < l.lo ? l.lo : (v > l.hi ? l.hi : v);
}

// ---------------------------------------------------------------------------
// Output site
// ---------------------------------------------------------------------------

// A bookmark we allocate, so we can tell ours apart from the engine's with
// dynamic_cast. Ownership passes to the site, which deletes it -- safe across
// the module boundary because Bookmark has a virtual destructor, so the
// deleting destructor in the vtable is the one the allocating module compiled.
class IndexBookmark : public Bookmark {
public:
    explicit IndexBookmark(uint32_t gen, uint32_t idx)
        : generation(gen), index(idx), magic(0x46565831u) { type = BM_USER; }
    virtual Bookmark* clone() const { return new IndexBookmark(*this); }
    uint32_t generation;
    uint32_t index;
    uint32_t magic;
};

class Engine::Site : public IWaveOutputSite {
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

    // Stop accepting anything from the engine; used before Engine::stop() so a
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
        EnterCriticalSection(&cs_);
        StreamItem it;
        it.kind = StreamItem::FAILED;
        it.message = msg;
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

    // --- IAttribute: we expose nothing ------------------------------------
    virtual bool set(const char*, bool, int) { return false; }
    virtual bool get(const char*, bool&, int) const { return false; }
    virtual bool set(const char*, char, int) { return false; }
    virtual bool get(const char*, char&, int) const { return false; }
    virtual bool set(const char*, int, int) { return false; }
    virtual bool get(const char*, int&, int) const { return false; }
    virtual bool set(const char*, double, int) { return false; }
    virtual bool get(const char*, double&, int) const { return false; }
    virtual bool set(const char*, const char*, int) { return false; }
    virtual bool get(const char*, char*, int, int*, int) const { return false; }
    virtual bool setArraySize(const char*, int) { return false; }
    virtual bool getArraySize(const char*, int&) const { return false; }
    virtual const IAttribute* getAttribute(const char*) const { return 0; }
    virtual IAttribute* getAttribute(const char*) { return 0; }

private:
    void handle(const Bookmark& bm)
    {
        EnterCriticalSection(&cs_);
        if (!generation_) { LeaveCriticalSection(&cs_); return; }

        if (const IndexBookmark* ib = dynamic_cast<const IndexBookmark*>(&bm)) {
            if (ib->magic == 0x46565831u && ib->generation == generation_) {
                StreamItem it;
                it.kind = StreamItem::BOOKMARK;
                it.value = ib->index;
                queue_.push_back(it);
                // Drop it from the pending list; whatever is left when the
                // utterance ends gets flushed, because the engine silently
                // swallows a bookmark added after the final text fragment.
                for (size_t i = 0; i < pending_.size(); ++i) {
                    if (pending_[i] == ib->index) {
                        pending_.erase(pending_.begin() + i);
                        break;
                    }
                }
            }
            LeaveCriticalSection(&cs_);
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
            FV_LOG("engine: BM_TEXT_END");
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

    WaveOutputFormat        fmt_;
    CRITICAL_SECTION        cs_;
    HANDLE                  ready_ = nullptr;
    std::deque<StreamItem>  queue_;
    std::vector<uint32_t>   pending_;
    uint32_t                generation_ = 0;
    bool                    wantWords_ = false;
    bool                    wantSentences_ = false;
    bool                    done_ = true;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() {}

Engine::~Engine()
{
    destroyEngine();
    delete static_cast<Speaker*>(speaker_);
    speaker_ = nullptr;
    delete static_cast<EngineFactory*>(factory_);
    factory_ = nullptr;
    delete site_;
    site_ = nullptr;
}

void Engine::destroyEngine()
{
    if (engine_) {
        MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);
        try { e->stop(); } catch (...) {}
        // Engine overrides operator delete to route back through the factory.
        try { delete e; } catch (...) {}
        engine_ = nullptr;
    }
}

std::vector<uint32_t> Engine::probeLanguages(const std::string& dataRoot)
{
    std::vector<uint32_t> found;
    for (int i = 0; i < voices::kLanguageCount; ++i) {
        const std::string dir = joinPath(dataRoot, voices::kLanguages[i].dir);
        const DWORD attr = GetFileAttributesA(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // A language directory without default.tav is not usable.
        if (GetFileAttributesA(joinPath(dir, "default.tav").c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(joinPath(dir, "Default.tav").c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        found.push_back(voices::kLanguages[i].id);
    }
    return found;
}

bool Engine::open(const std::string& dataRoot, uint32_t language, std::string& error)
{
    if (factory_ && language_ == language) return true;

    destroyEngine();
    delete static_cast<Speaker*>(speaker_);
    speaker_ = nullptr;
    currentTav_.clear();

    if (factory_ && language_ != language) {
        // Loading a second language into an existing factory is supported, but
        // a fresh factory is cheap (~30 ms) and avoids any cross-language state.
        delete static_cast<EngineFactory*>(factory_);
        factory_ = nullptr;
    }

    try {
        if (!factory_) {
            FV_LOG("engine: creating EngineFactory(\"%s\")", dataRoot.c_str());
            factory_ = new EngineFactory(dataRoot.c_str());
        }
        static_cast<EngineFactory*>(factory_)->loadLanguage(static_cast<Language>(language));
        dataRoot_ = dataRoot;
        language_ = language;
        FV_LOG("engine: language 0x%04x loaded, engine version %s", language, getVersion());
        return true;
    } catch (GenericException& e) {
        error = std::string(e.what()) + ": " + e.details();
        FV_LOG("engine: open failed: %s", error.c_str());
    } catch (...) {
        error = "unknown exception while opening the engine";
        FV_LOG("engine: open failed: %s", error.c_str());
    }
    delete static_cast<EngineFactory*>(factory_);
    factory_ = nullptr;
    return false;
}

bool Engine::selectVoice(const std::string& tavPath, const double* params,
                         uint32_t paramMask, int sampleRate, std::string& error)
{
    if (!factory_) { error = "engine not open"; return false; }

    // Rebuilding the engine costs ~16 ms and spawns a worker thread, so only do
    // it when the speaker file or the output format actually changed.
    const bool sameTav = (tavPath == currentTav_);
    const bool sameRate = (sampleRate == sampleRate_);

    // The speaker-level parameters decide the voice's identity and hardly ever
    // change; the engine-level three change on every utterance a screen reader
    // sends. Reloading the .tav from disk, re-applying the parameters and
    // calling addSpeaker/setSpeaker each time is pure cost on the path that
    // matters most -- every keystroke while arrowing. Do it only when something
    // it depends on actually moved.
    bool speakerUnchanged = engine_ && site_ && sameTav && sameRate &&
                            speaker_ && paramMask == lastMask_;
    if (speakerUnchanged) {
        for (int i = 0; i < FVP_COUNT; ++i) {
            if (kEngineLevel[i] || !(paramMask & (1u << i))) continue;
            if (params[i] != lastParams_[i]) { speakerUnchanged = false; break; }
        }
    }

    if (speakerUnchanged) {
        try {
            MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);
            for (int i = 0; i < FVP_COUNT; ++i) {
                if (!kEngineLevel[i]) continue;
                const double v = (paramMask & (1u << i))
                    ? clampParam(static_cast<FlexVoiceParam>(i), params[i])
                    : 1.0;
                e->attribute().set(kAttrName[i], v);
                lastParams_[i] = params[i];
            }
            return true;
        } catch (...) {
            // Fall through and rebuild properly.
        }
    }

    try {
        std::auto_ptr<Speaker> sp(new Speaker());
        sp->load(tavPath.c_str());

        for (int i = 0; i < FVP_COUNT; ++i) {
            if (!(paramMask & (1u << i))) continue;
            if (kEngineLevel[i]) continue;          // applied after createEngine
            const double v = clampParam(static_cast<FlexVoiceParam>(i), params[i]);
            const bool ok = kAttrIsInt[i]
                ? sp->set(kAttrName[i], static_cast<int>(v + (v < 0 ? -0.5 : 0.5)))
                : sp->set(kAttrName[i], v);
            if (!ok) FV_LOG("engine: speaker rejected %s", kAttrName[i]);
        }

        // pitchMin must stay below pitchMax or the contour collapses.
        int lo = 0, hi = 0;
        if (sp->get("pitchMin", lo) && sp->get("pitchMax", hi) && lo >= hi) {
            sp->set("pitchMin", hi > 2 ? hi - 1 : 1);
            FV_LOG("engine: pitchMin %d >= pitchMax %d, lowered", lo, hi);
        }

        EngineFactory* f = static_cast<EngineFactory*>(factory_);
        f->addSpeaker(static_cast<Language>(language_), *sp,
                      SCS_LOAD_IMMEDIATELY_DO_NOT_DELETE);

        if (!engine_ || !sameTav || !sameRate || !site_) {
            destroyEngine();
            delete site_;
            site_ = new Site(sampleRate, 16);
            std::auto_ptr<MM_TTSAPI::Engine> e =
                f->createEngine(site_, *sp, static_cast<Language>(language_));
            engine_ = e.release();
            sampleRate_ = sampleRate;
        } else {
            // Same engine, new parameters: cheapest path, applied immediately.
            static_cast<MM_TTSAPI::Engine*>(engine_)->setSpeaker(*sp, true);
        }

        // The engine-level multipliers, applied last. These are cheap: measured
        // at 0 ms even in the middle of an utterance.
        {
            MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);
            for (int i = 0; i < FVP_COUNT; ++i) {
                if (!kEngineLevel[i]) continue;
                const double v = (paramMask & (1u << i))
                    ? clampParam(static_cast<FlexVoiceParam>(i), params[i])
                    : 1.0;
                if (!e->attribute().set(kAttrName[i], v)) {
                    FV_LOG("engine: attribute() rejected %s", kAttrName[i]);
                }
            }
        }

        delete static_cast<Speaker*>(speaker_);
        speaker_ = sp.release();
        currentTav_ = tavPath;
        lastMask_ = paramMask;
        for (int i = 0; i < FVP_COUNT; ++i) lastParams_[i] = params[i];
        return true;
    } catch (GenericException& e) {
        error = std::string(e.what()) + ": " + e.details();
    } catch (...) {
        error = "unknown exception while selecting a voice";
    }
    FV_LOG("engine: selectVoice(\"%s\") failed: %s", tavPath.c_str(), error.c_str());
    destroyEngine();
    currentTav_.clear();
    return false;
}

bool Engine::speak(const std::vector<Segment>& segments, bool wantWords,
                   bool wantSentences, std::string& error)
{
    if (!engine_ || !site_) { error = "no voice selected"; return false; }

    static uint32_t s_generation = 0;
    const uint32_t gen = ++s_generation ? s_generation : ++s_generation;

    std::vector<uint32_t> expected;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].kind == FV_SEG_BOOKMARK) expected.push_back(segments[i].value);
    }

    MM_TTSAPI::Engine* e = static_cast<MM_TTSAPI::Engine*>(engine_);

    // A completed request stays loaded in the engine: without this, the next
    // speakRequest replays the previous utterance and ignores the fragments
    // just added. Measured -- reset() does not clear it, only stop() does.
    try { e->stop(); } catch (...) {}

    site_->beginUtterance(gen, wantWords, wantSentences, expected);

    try {
        bool anyText = false;
        for (size_t i = 0; i < segments.size(); ++i) {
            const Segment& s = segments[i];
            switch (s.kind) {
            case FV_SEG_TEXT: {
                const std::string t = sanitize(s.text);
                if (!t.empty()) {
                    e->addFragment(t.c_str());
                    anyText = true;
                }
                break;
            }
            case FV_SEG_SPELL: {
                // Spelled out here rather than with the engine's own \spell\
                // block, because that block does not work: measured, every
                // input produces the same 21210 bytes of near-silence
                // regardless of the word, so \spell\text\endspell\ swallows
                // the text and emits a trailing pause.
                const std::string t = text::normalize_bytes(s.text, text::Spell);
                if (!t.empty()) {
                    e->addFragment(t.c_str());
                    anyText = true;
                }
                break;
            }
            case FV_SEG_SILENCE: {
                char buf[48];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\pau=%u\\",
                            s.value > 60000u ? 60000u : s.value);
                e->addFragment(buf);
                anyText = true;
                break;
            }
            // Mid-utterance parameter changes. The command text is built here
            // from an integer the client sent, never from client text, so
            // there is nothing to escape and nothing to inject.
            case FV_SEG_RATE: {
                char buf[48];
                const uint32_t v = s.value < 10u ? 10u : (s.value > 1000u ? 1000u : s.value);
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\rspd=%u\\", v);
                e->addFragment(buf);
                break;
            }
            case FV_SEG_PITCH: {
                char buf[48];
                const uint32_t v = s.value < 25u ? 25u : (s.value > 400u ? 400u : s.value);
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\rpit=%u\\", v);
                e->addFragment(buf);
                break;
            }
            case FV_SEG_VOLUME: {
                char buf[48];
                const uint32_t v = s.value > 100u ? 100u : s.value;
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\vol=%u\\", v);
                e->addFragment(buf);
                break;
            }
            case FV_SEG_BOOKMARK:
                e->addBookmark(new IndexBookmark(gen, s.value));
                break;
            }
        }

        if (!anyText) {
            // Bookmarks but no text. The engine emits no BM_TEXT_END for an
            // empty request, so complete the utterance here instead of letting
            // the caller wait for a signal that will never come.
            site_->finishEmpty();
            return true;
        }

        e->speakRequest(1);
        FV_LOG("engine: speakRequest issued, gen %u, %d segment(s)", gen,
               (int)segments.size());
        return true;
    } catch (GenericException& ex) {
        error = std::string(ex.what()) + ": " + ex.details();
    } catch (...) {
        error = "unknown exception while speaking";
    }
    FV_LOG("engine: speak failed: %s", error.c_str());
    site_->fail(error);
    return false;
}

bool Engine::poll(StreamItem& out) { return site_ && site_->pop(out); }

void Engine::waitForItem(uint32_t timeoutMs)
{
    if (!site_ || !site_->readyEvent()) { Sleep(1); return; }
    WaitForSingleObject(site_->readyEvent(), timeoutMs);
}
bool Engine::finished() const { return !site_ || site_->finished(); }

void Engine::stop()
{
    if (!engine_) return;
    // Close the gate first: measured, no put() arrives after stop(), but a
    // chunk already inside put() must not land in the next utterance.
    if (site_) site_->gateOff();
    try { static_cast<MM_TTSAPI::Engine*>(engine_)->stop(); } catch (...) {}
}

}  // namespace FlexVoice
