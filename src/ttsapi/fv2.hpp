// fv2.hpp -- the Mindmaker FlexVoice 2.0 C++ engine API, reconstructed.
//
// FlexVoice 2.0 shipped no headers. What it shipped was FlexVoice_2_00_010.dll:
// 381 MSVC-mangled C++ exports in namespace MM_TTSAPI, built with Visual C++ 6
// in April 2001, x86 only. A mangled name encodes the complete signature, so
// every declaration below was read back out of the export table rather than
// guessed, and sdk/fv2lib/FlexVoice_2_00_010.def lists the exact symbols this
// header must produce.
//
// The FlexVoice 3.01 SDK (sdk/ttsapi301/) documents a LATER version of the same
// API and 257 of the 381 v2 mangled names appear in it byte-for-byte, so it is
// a good starting point -- but it is not this API, and the differences bite:
//
//   * 3.01 has an IAttribute base and a named get/set attribute mechanism.
//     **v2 has no IAttribute at all** -- no such class is exported, and v2's
//     IOutputSite vftables list only INotifyDispatcher where 3.01's list
//     IAttribute too. Speaker parameters in v2 are not reachable by name; the
//     .tav speaker file is the interface. See fv2_speaker.hpp.
//   * 3.01: WaveOutputFormat(int,int,WaveCoding).  v2: (int,int,int) --
//     ??0WaveOutputFormat@MM_TTSAPI@@QAE@HHH@Z.
//   * 3.01: ~Speaker is virtual (??1Speaker@MM_TTSAPI@@UAE@XZ).
//     v2: non-virtual (QAE) -- v2's Speaker has no vtable and no base class.
//   * 3.01 exports getLangID/getLangName. v2 exports neither; the only free
//     function in the whole v2 DLL is getVersion().
//
// Layout rules this header lives by, because a wrong layout corrupts memory
// silently rather than failing to link:
//
//   * The interface classes (INotify, INotifyDispatcher, IOutputFormat,
//     IOutputSite, IWaveOutputSite) carry no data members, so their layout is
//     fully determined by the inheritance graph -- and that graph is not
//     guesswork either. Every vbtable/vftable symbol in the DLL was read:
//     IOutputSite has a vbtable and a vftable {for INotifyDispatcher}, so it
//     inherits INotifyDispatcher VIRTUALLY; IWaveOutputSite has vbtables
//     {for IOutputSite} and {for IWaveOutputSite}, so it inherits IOutputSite
//     virtually; and so on.
//   * Speaker has no bases and no virtuals, so trailing padding is harmless --
//     the engine's constructor writes only the bytes it knows about. It is
//     padded generously rather than sized exactly, because an under-sized
//     declaration would let the DLL's constructor write off the end of our
//     object. Over-allocating cannot. fv2_probe reports the highest byte the
//     engine actually touches so the pad can be checked rather than trusted.
//   * Classes with virtual bases (WaveOutputFormat) may NOT be padded: a
//     virtual base subobject sits after the derived data, so padding moves it
//     and the DLL's constructor would initialise the wrong address. Those are
//     declared to match exactly.

#pragma once

#include <bitset>

// std::auto_ptr appears in EngineFactory::createEngine's mangled return type,
// so the type must really be std::auto_ptr for the name to match. It is gone in
// C++17; the engine binding is compiled as C++14 for this one reason. MSVC's
// auto_ptr is a single raw pointer, as VC6's was, so it is layout-compatible.
#include <memory>

namespace MM_TTSAPI {

// ---------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------

// v2 exports no Bookmark symbols, so Bookmark is header-only and its layout is
// fixed by whatever Mindmaker's header said. The engine allocates these and
// hands them over, so a mismatch here reads garbage rather than crashing.
// fv2_probe checks that every type it receives is inside this enum.
enum BookmarkType {
    BM_INVALID,
    BM_TEXT_BEGIN,      BM_TEXT_END,
    BM_SECTION_BEGIN,   BM_SECTION_END,
    BM_PARAGRAPH_BEGIN, BM_PARAGRAPH_END,
    BM_SENTENCE_BEGIN,  BM_SENTENCE_END,
    BM_ITEM_BEGIN,      BM_ITEM_END,
    BM_WORD_BEGIN,      BM_WORD_END,
    BM_PHONEME_BEGIN,   BM_PHONEME_END,
    BM_IPAPHONEME_BEGIN, BM_IPAPHONEME_END,
    BM_SPEAKERCHANGED,
    BM_PARAMCHANGED,
    BM_USER,
    BM_EMBEDDED,
    BM_PAUSE,
    BM_RESUME,
    BM_START,
    BM_STOP,
    BM_REPEAT,
    BM_NO_OF_BOOKMARKS
};

class BookmarkTypeList : public std::bitset<BM_NO_OF_BOOKMARKS>
{
public:
    BookmarkTypeList() {}
    BookmarkTypeList(const BookmarkType& type) { set(type); }
};

inline BookmarkTypeList operator|(const BookmarkType& a, const BookmarkType& b)
{
    BookmarkTypeList l; l.set(a); l.set(b); return l;
}
inline BookmarkTypeList operator|(const BookmarkTypeList& a, const BookmarkType& b)
{
    BookmarkTypeList l(a); l.set(b); return l;
}

class Bookmark
{
public:
    Bookmark()
        : type(BM_INVALID), pos(0), time_sec(0), time_ms(0), len(0), dur(0), id(0) {}
    virtual ~Bookmark() {}
    virtual Bookmark* clone() const { return new Bookmark(*this); }

    BookmarkType   type;
    int            pos;
    unsigned long  time_sec;
    unsigned short time_ms;
    int            len;
    int            dur;
    int            id;
};

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

class INotify
{
public:
    virtual ~INotify() {}
    virtual void bookmark(const Bookmark& bookmark) = 0;
    virtual void prepare(const Bookmark& bookmark) = 0;
};

class INotifyDispatcher
{
public:
    virtual ~INotifyDispatcher() {}
    virtual void registerNotify(INotify* notify, const BookmarkTypeList& types) = 0;
    virtual void unregisterNotify(INotify* notify) = 0;
    virtual void addBookmarkTypes(INotify* notify, const BookmarkTypeList& types) = 0;
    virtual void removeBookmarkTypes(INotify* notify, const BookmarkTypeList& types) = 0;
};

// ---------------------------------------------------------------------------
// Output formats
// ---------------------------------------------------------------------------

class IOutputFormat
{
public:
    enum OutputType {
        NORMALIZED_TEXT,
        PHONEME_STRING,
        PHONEME_STRING_WITH_PROSODY,
        IPA_PHONEME_STRING_WITH_PROSODY,
        WAVE
    };

    virtual ~IOutputFormat() {}
    virtual OutputType outputType() const = 0;
};

// Note the constructor takes three plain ints in v2, not (int, int, WaveCoding)
// as 3.01 does. WaveCoding is still declared, to name the third argument's
// values, but it must be passed as an int or the mangled name changes.
//
// And note `public IOutputFormat`, NOT `public virtual` as 3.01 declares it.
// v2 exports ??_7WaveOutputFormat@MM_TTSAPI@@6B@ and no ??_8 vbtable, so the
// inheritance is plain; the object is vfptr + three ints = 16 bytes, which is
// exactly what the DLL's constructor writes. Declaring it virtual instead puts
// a vbptr at offset 0 where the engine puts a vfptr: the three ints still read
// correctly, so it looks fine right up until the first call to outputType(),
// which then dispatches through a vbtable that is really a vftable.
struct WaveOutputFormat : public IOutputFormat
{
    enum WaveCoding {
        WC_INVALID,
        WC_PCM_SIGNED,
        WC_PCM_UNSIGNED,
        WC_ULAW,
        WC_ALAW,
        WC_VOX,
        WC_VIS
    };

    WaveOutputFormat(int sf, int br, int wc);
    WaveOutputFormat(const WaveOutputFormat& from);
    virtual ~WaveOutputFormat();
    WaveOutputFormat& operator=(const WaveOutputFormat& from);

    virtual OutputType outputType() const;

    int samplingFrequency;
    int bitResolution;
    int waveCoding;
};

// ---------------------------------------------------------------------------
// Output sites
// ---------------------------------------------------------------------------

// Implemented by the client. The engine calls put() from its own worker thread
// as audio becomes available, and setBookmark() to interleave marks with it.
class IOutputSite : public virtual INotifyDispatcher
{
public:
    virtual const IOutputFormat& getOutputFormat() const = 0;

    virtual void pause() = 0;
    virtual void play()  = 0;
    virtual void clear() = 0;

    // Ownership of the bookmark passes to the site.
    virtual void setBookmark(Bookmark* bookmark) = 0;
    // Sent synchronously, bypassing the audio queue -- used for BM_STOP.
    virtual void sendBookmark(Bookmark& bookmark) = 0;
};

class IWaveOutputSite : public virtual IOutputSite
{
public:
    // The buffer is not owned by the site; copy it before returning.
    virtual void put(unsigned char* buffer, unsigned int size) = 0;
};

// ---------------------------------------------------------------------------
// Speakers
// ---------------------------------------------------------------------------

enum SpeakerCacheStrategy {
    SCS_LOAD_IMMEDIATELY_DO_NOT_DELETE,
    SCS_LOAD_IMMEDIATELY_DELETE_WHEN_NOT_USED,
    SCS_LOAD_IMMEDIATELY_DELETE_TIMEOUT,
    SCS_LOAD_WHEN_NEEDED_DO_NOT_DELETE,
    SCS_LOAD_WHEN_NEEDED_DELETE_WHEN_NOT_USED,
    SCS_LOAD_WHEN_NEEDED_DELETE_TIMEOUT
};

// A voice. load() parses a .tav file -- see fv2_speaker.hpp for that format,
// which in v2 is the only way to reach the synthesis parameters.
//
// No bases, no virtual functions: ~Speaker is exported as QAE (non-virtual),
// unlike 3.01's UAE. The pad is deliberate over-allocation; see the header
// comment. FV2_SPEAKER_CAPACITY is checked at runtime by fv2_probe.
class Speaker
{
public:
    enum { FV2_SPEAKER_CAPACITY = 65536 };

    Speaker();
    Speaker(const Speaker& from);
    ~Speaker();
    Speaker& operator=(const Speaker& from);

    bool operator<(const Speaker& other) const;
    bool operator==(const Speaker& other) const;

    // Throws (FileNotFoundException / a parse failure) on a bad file.
    void load(const char* filename);
    void save(const char* filename);

private:
    char storage_[FV2_SPEAKER_CAPACITY];
};

class SpeakerMap;

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

// v2's Language is a real enum (W4Language in the mangling), where 3.01 made it
// a typedef for int. The VALUES are not recoverable from the export table and
// the v2 DLL contains no language-name strings, so they are not asserted here:
// probe/fv2_probe.cpp sweeps candidate values and reports which ones the
// factory will actually build an engine for. LNG_ENGLISH below is the 3.01
// value and is a hypothesis until that sweep confirms it.
enum Language {
    LNG_INVALID   = 0x0000,
    LNG_ENGLISH   = 0x0409,
    LNG_HUNGARIAN = 0x040e
};

enum DocumentUnit {
    DU_TEXT,
    DU_SECTION,
    DU_PARAGRAPH,
    DU_SENTENCE,
    DU_ITEM,
    DU_WORD,
    DU_PHONEME
};

class IPhonemeCoder;
class UserDictionary;
class EngineFactory;

class Engine
{
public:
    ~Engine();
    static void operator delete(void* p);

    // Text in, in fragments; then speakRequest() to render what has been added.
    void addFragment(const char* text);
    void addBookmark(Bookmark* bookmark);

    void speakRequest(int id);
    void speakRequest(const char* text, int id);

    void wait();
    void stop();
    void cancel(int id);
    void reset();

    void pause(DocumentUnit unit, int count);
    void resume();
    int  skip(DocumentUnit unit, int count);

    // Engine-level multipliers, distinct from the Speaker's own values.
    void   setSpeechRate(double rate, bool relative);
    double getSpeechRate();
    void   setVolume(double volume, bool relative);
    double getVolume() const;

    void setSpeaker(const Speaker& speaker, bool immediately);
    const Speaker& getSpeaker() const;

    void addSpeaker(const char* name, const Speaker& speaker, SpeakerCacheStrategy strategy);
    void updateSpeaker(const char* name, const Speaker& speaker);
    void removeSpeaker(const char* name);
    void setSpeakers(const SpeakerMap& speakers, SpeakerCacheStrategy strategy);
    const SpeakerMap& getSpeakers() const;

    void setUserDictionary(const UserDictionary& dictionary);
    void setPhonemeCoder(IPhonemeCoder* coder);

private:
    // Private in the DLL too -- engines come from EngineFactory::createEngine.
    Engine(EngineFactory* factory, IOutputSite* site, const Speaker& speaker, Language language);
};

// The factory owns the loaded language data and must outlive every engine it
// creates. Constructing one is expensive; constructing an Engine from one is
// not, which is the whole point of the split.
class EngineFactory
{
public:
    enum { FV2_FACTORY_CAPACITY = 4096 };

    // `path` is the directory that CONTAINS the Data folder -- the engine
    // builds "<path>/Data/TN.dat" and friends, with forward slashes, which is
    // how those strings appear in the DLL. Passing null makes the engine look
    // for the path itself (environment, then registry), which is exactly the
    // dependency this project exists to remove, so always pass a real path.
    // Throws CannotInitializeFactory if the data cannot be read.
    EngineFactory(const char* path);
    ~EngineFactory();

    std::auto_ptr<Engine> createEngine(IOutputSite* site,
                                       const Speaker& speaker,
                                       Language language);

    long addSpeaker(const Speaker& speaker, SpeakerCacheStrategy strategy);
    void removeSpeaker(long id);

private:
    char storage_[FV2_FACTORY_CAPACITY];
};

// The one free function the DLL exports.
const char* getVersion();

}  // namespace MM_TTSAPI
