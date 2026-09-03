#include "ISpTTSEngineImpl.hpp"
#include "engine_client.h"
#include "flexvoice_log.h"
#include "settings.h"
#include "text_normalize.h"
#include "utils.hpp"
#include "voice_data.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <vector>

namespace FlexVoice {
namespace sapi {

namespace {

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// SAPI rate is -10..+10 around the voice's own speed. Three-fold either way
// matches what other engines in this family do and keeps ±10 usable rather
// than unintelligible.
double rate_factor(int sapiRate) { return std::pow(3.0, clampi(sapiRate, -10, 10) / 10.0); }

// SAPI pitch is -10..+10; an octave either way. pitchRate scales the whole F0
// contour without touching duration, which the sweep confirmed is exact.
double pitch_factor(int sapiPitch) { return std::pow(2.0, clampi(sapiPitch, -10, 10) / 10.0); }

struct Utterance {
    ISpTTSEngineSite* site = nullptr;
    ULONGLONG         bytesWritten = 0;
    bool              aborted = false;
    bool              wantWords = false;
    bool              wantSentences = false;

    // Word/sentence bookmarks carry an offset into the text we handed the
    // engine, not into the client's original string. This maps one to the other.
    std::vector<std::pair<uint32_t, uint32_t>> offsetMap;   // engineOffset -> srcOffset

    // Bookmark names, indexed by the integer id the engine round-trips.
    std::vector<std::wstring> bookmarks;
};

// Translate an engine-side character offset back into the caller's text.
//
// The map is dense -- one entry per byte the engine was given -- because only
// text segments advance the engine's own offset. A spelled-out fragment is the
// exception: the host wraps it in \spell\...\endspell\, which shifts the
// engine's offsets by the length of those markers, so a word highlight inside
// a <spell> run can land a few characters early. Rare enough to accept, and it
// degrades to a nearby offset rather than a wrong one.
uint32_t map_offset(const Utterance& u, uint32_t engineOffset)
{
    if (u.offsetMap.empty()) return 0;
    if (engineOffset < u.offsetMap.size()) return u.offsetMap[engineOffset].second;
    return u.offsetMap.back().second;
}

// Start the engine host, once per process, off the calling thread. The first
// utterance would otherwise pay ~190 ms for launching it and loading the
// language -- and the first utterance is usually the one the user is waiting
// on. Everything after that reuses the running host and costs ~6 ms.
DWORD WINAPI warm_up_thread(LPVOID)
{
    FlexVoicePong pong = {};
    std::string error;
    if (!sharedClient().ping(pong, error)) {
        FV_LOG("sapi: warm-up ping failed: %s", error.c_str());
    }
    return 0;
}

void warm_up_host()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        if (HANDLE h = CreateThread(nullptr, 0, warm_up_thread, nullptr, 0, nullptr)) {
            CloseHandle(h);
        }
        return TRUE;
    }, nullptr, nullptr);
}

bool emit_event(Utterance& u, SPEVENTENUM id, ULONG stream, WPARAM wp, LPARAM lp,
                SPEVENTLPARAMTYPE lpType)
{
    SPEVENT ev = {};
    ev.eEventId = id;
    ev.elParamType = lpType;
    ev.ulStreamNum = stream;
    ev.ullAudioStreamOffset = u.bytesWritten;
    ev.wParam = wp;
    ev.lParam = lp;
    return SUCCEEDED(u.site->AddEvents(&ev, 1));
}

}  // namespace

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) return E_INVALIDARG;

    token_ = pToken;

    int index = -1;
    {
        utils::out_ptr<wchar_t> value(CoTaskMemFree);
        if (SUCCEEDED(pToken->GetStringValue(L"TokenIndex", value.address())) && value.get()) {
            index = _wtoi(value.get());
        }
    }

    // A statically registered token may predate the TokenIndex value, or have
    // been written by a different build; fall back to matching the Name
    // attribute so a stale token still resolves to the right voice.
    if (index < 0 || index >= voices::kVoiceCount) {
        ISpDataKey* attrs = nullptr;
        if (SUCCEEDED(pToken->OpenKey(L"Attributes", &attrs)) && attrs) {
            utils::out_ptr<wchar_t> name(CoTaskMemFree);
            if (SUCCEEDED(attrs->GetStringValue(L"Name", name.address())) && name.get()) {
                for (int i = 0; i < voices::kVoiceCount; ++i) {
                    if (_wcsicmp(name.get(), voices::kVoices[i].displayName) == 0) {
                        index = i;
                        break;
                    }
                }
            }
            attrs->Release();
        }
    }

    attr_ = voice_attributes(clampi(index < 0 ? 0 : index, 0, voices::kVoiceCount - 1));
    FV_LOG("sapi: token bound to voice %d (%S)", attr_.token_index(), attr_.get_name().c_str());
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) return E_POINTER;
    *ppToken = nullptr;
    if (!token_) return E_UNEXPECTED;
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(const GUID*, const WAVEFORMATEX*,
                                               GUID* pOutputFormatId,
                                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) return E_POINTER;
    *ppCoMemOutputWaveFormatEx = nullptr;

    // No engine work here: SAPI does not promise that GetOutputFormat and
    // Speak run on the same thread. But this is the first call SAPI makes
    // after a voice is selected, and starting the host costs about 190 ms, so
    // do that now on a background thread rather than making the user's first
    // keystroke pay for it.
    warm_up_host();

    const int hz = SettingsStore::current().sampleRate;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) return E_OUTOFMEMORY;

    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = 1;
    wfx->nSamplesPerSec = static_cast<DWORD>(hz);
    wfx->wBitsPerSample = 16;
    wfx->nBlockAlign = 2;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = 0;

    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = wfx;
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD, REFGUID, const WAVEFORMATEX*,
                                     const SPVTEXTFRAG* pTextFragList,
                                     ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) return E_INVALIDARG;

    try {
        const Settings& s = SettingsStore::current();
        log::enabled() = s.debugLogging;

        long sapiRate = 0;
        USHORT sapiVolume = 100;
        ULONGLONG interest = 0;
        pOutputSite->GetRate(&sapiRate);
        pOutputSite->GetVolume(&sapiVolume);
        pOutputSite->GetEventInterest(&interest);

        Utterance u;
        u.site = pOutputSite;
        u.wantWords = (interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        u.wantSentences = (interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;

        const voices::Voice& voice = attr_.def();
        const bool configurable = voice.isCustom;
        const unsigned codepage = voices::kLanguages[voice.languageIndex].codepage;

        SpeakParams params;
        params.voiceIndex = static_cast<uint32_t>(attr_.token_index());
        params.language = attr_.language_id();
        params.sampleRate = static_cast<uint32_t>(s.sampleRate);
        params.wantWordEvents = u.wantWords;
        params.wantSentenceEvents = u.wantSentences;

        // Two levels, kept strictly apart:
        //   speaker parameters  = who the voice is    (absolute units)
        //   engine multipliers  = what the client asked for (relative, 1.0 = as-is)
        //
        // The configurable voice takes both from the utility. A named preset
        // takes its speaker parameters from its own table and nothing from the
        // utility, or every preset would collapse onto the same voice.
        double rateScale = 1.0;
        double volumeScale = 1.0;
        double pitchScale = 1.0;

        if (configurable) {
            for (int i = 0; i < FVP_COUNT; ++i) {
                const auto p = static_cast<FlexVoiceParam>(i);
                const double v = voices::percent_to_value(voices::param(p), s.percent[i]);
                switch (p) {
                case FVP_SPEECH_RATE: rateScale = v; break;
                case FVP_VOLUME:      volumeScale = v; break;
                case FVP_PITCH_RATE:  pitchScale = v; break;
                default:              params.set(p, v); break;
                }
            }
            // The Custom voice's timbre starts from whichever diphone database
            // the user picked. Roster index 3 is Tim, the only entry pointing at
            // the Tom database; the parameters above then overwrite Tim's own
            // character, so what survives is the database, not the preset.
            if (s.baseVoice == 1) params.voiceIndex = 3;
        } else {
            static const char* kNames[FVP_COUNT] = {
                "speechRate", "volume", "defaultPitch", "pitchRate", "pitchMin",
                "pitchMax", "intonationLevel", "headsize", "tilt", "richness",
                "breathiness", "smoothness", "fricationRate", "plosiveRate" };
            for (int k = 0; k < voice.overrideCount; ++k) {
                for (int p = 0; p < FVP_COUNT; ++p) {
                    if (strcmp(voice.overrides[k].name, kNames[p]) == 0) {
                        params.set(static_cast<FlexVoiceParam>(p), voice.overrides[k].value);
                        break;
                    }
                }
            }
        }

        // The client's rate and volume always apply, to every voice, on top of
        // the voice's own measured level trim.
        params.set(FVP_SPEECH_RATE, rateScale * rate_factor(sapiRate));
        params.set(FVP_VOLUME, voice.levelTrim * volumeScale * (sapiVolume / 100.0));
        params.set(FVP_PITCH_RATE, pitchScale);

        // Build the segment list, tracking where each piece of engine text came
        // from so word events can be reported against the caller's offsets.
        std::vector<SpeakSegment> segments;
        uint32_t engineOffset = 0;
        int fragRate = INT_MIN, fragPitch = INT_MIN;
        long curVolume = -1;

        // Normalization rewrites the text -- every numeral becomes words,
        // because a numeral reaching the engine faults or wedges it -- so a
        // word-boundary offset the engine reports is an offset into the
        // rewritten text. The normalizer hands back a per-byte map into the
        // fragment, and this turns that into a map into the caller's string.
        auto push_text = [&](const wchar_t* wide, uint32_t wideLen,
                             uint32_t srcOffset, text::Mode mode) {
            const text::Normalized n = text::normalize(wide, wideLen, codepage, mode);
            if (n.text.empty()) return;
            SpeakSegment seg;
            // Always FV_SEG_TEXT: spelling has already happened here, where the
            // offset map that word events depend on can be built alongside it.
            seg.kind = FV_SEG_TEXT;
            seg.text = n.text;
            segments.push_back(seg);
            for (size_t i = 0; i < n.srcMap.size(); ++i) {
                u.offsetMap.emplace_back(engineOffset + static_cast<uint32_t>(i),
                                         srcOffset + n.srcMap[i]);
            }
            engineOffset += static_cast<uint32_t>(n.text.size());
        };

        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext) {
            DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) { u.aborted = true; break; }
            if (actions & SPVES_SKIP) { pOutputSite->CompleteSkip(0); u.aborted = true; break; }
            if (actions & SPVES_RATE) pOutputSite->GetRate(&sapiRate);
            if (actions & SPVES_VOLUME) pOutputSite->GetVolume(&sapiVolume);

            if (frag->State.eAction == SPVA_Bookmark) {
                // SAPI names a bookmark with a string, and hosts expect both
                // that string and its numeric value back in the event. The
                // engine only carries an integer, so hand it an index into a
                // side table and look the text up again on the way out.
                SpeakSegment seg;
                seg.kind = FV_SEG_BOOKMARK;
                seg.value = static_cast<uint32_t>(u.bookmarks.size());
                u.bookmarks.emplace_back(frag->pTextStart ? frag->pTextStart : L"",
                                         frag->ulTextLen);
                segments.push_back(seg);
                continue;
            }
            if (frag->State.eAction == SPVA_Silence) {
                SpeakSegment seg;
                seg.kind = FV_SEG_SILENCE;
                seg.value = static_cast<uint32_t>(clampi(frag->State.SilenceMSecs, 0, 60000));
                segments.push_back(seg);
                continue;
            }
            // Anything that is not speech - ParseUnknown, Pronounce tables and
            // the rest - must be skipped, or the engine reads the host's own
            // bookkeeping aloud.
            if (frag->State.eAction != SPVA_Speak &&
                frag->State.eAction != SPVA_SpellOut &&
                frag->State.eAction != SPVA_Pronounce) {
                continue;
            }
            if (!frag->pTextStart || frag->ulTextLen == 0) continue;

            // Per-fragment rate, pitch and volume, emitted only when they
            // change so the change lands at the right word rather than
            // "as soon as possible". They travel as numbers; the host turns
            // them into the engine's own inline commands, so no command text
            // ever passes through the normalizer that would mangle it.
            const int wantRate = clampi(frag->State.RateAdj, -10, 10);
            const int wantPitch = clampi(frag->State.PitchAdj.MiddleAdj, -10, 10);
            const long wantVolume = clampi(static_cast<int>(frag->State.Volume), 0, 100);

            auto push_param = [&](FlexVoiceSegmentKind kind, int percent) {
                SpeakSegment seg;
                seg.kind = kind;
                seg.value = static_cast<uint32_t>(percent);
                segments.push_back(seg);
            };

            if (wantRate != fragRate) {
                fragRate = wantRate;
                const double f = rate_factor(sapiRate + fragRate) / rate_factor(sapiRate);
                push_param(FV_SEG_RATE,
                           clampi(static_cast<int>(std::lround(f * 100.0)), 10, 1000));
            }
            if (wantPitch != fragPitch) {
                fragPitch = wantPitch;
                push_param(FV_SEG_PITCH,
                           clampi(static_cast<int>(std::lround(pitch_factor(fragPitch) * 100.0)),
                                  25, 400));
            }
            if (wantVolume != curVolume) {
                curVolume = wantVolume;
                push_param(FV_SEG_VOLUME, clampi(static_cast<int>(curVolume), 0, 100));
            }

            // SPVA_SpellOut is what SAPI produces for the <spell> tag, which is
            // how NVDA's spell-word command reaches an engine.
            push_text(frag->pTextStart, frag->ulTextLen, frag->ulTextSrcOffset,
                      frag->State.eAction == SPVA_SpellOut ? text::Spell : text::Prose);
        }

        if (u.aborted || segments.empty()) return S_OK;

        SpeakSink sink;
        sink.audio = [&](const unsigned char* pcm, uint32_t bytes) -> bool {
            if (pOutputSite->GetActions() & SPVES_ABORT) { u.aborted = true; return false; }
            ULONG written = 0;
            const HRESULT hr = pOutputSite->Write(pcm, bytes, &written);
            if (FAILED(hr)) { u.aborted = true; return false; }
            // pcbWritten is not trustworthy across SAPI hosts; S_OK means the
            // buffer was accepted in full.
            u.bytesWritten += bytes;
            return true;
        };
        sink.bookmark = [&](uint32_t id) -> bool {
            if (id >= u.bookmarks.size()) return true;
            // The buffer has to outlive AddEvents, so it lives in the
            // utterance, not on this lambda's stack.
            const std::wstring& name = u.bookmarks[id];
            return emit_event(u, SPEI_TTS_BOOKMARK, 0,
                              static_cast<WPARAM>(_wtoi(name.c_str())),
                              reinterpret_cast<LPARAM>(name.c_str()),
                              SPET_LPARAM_IS_STRING);
        };
        sink.word = [&](uint32_t pos, uint32_t len) -> bool {
            return emit_event(u, SPEI_WORD_BOUNDARY, 0, static_cast<WPARAM>(len),
                              static_cast<LPARAM>(map_offset(u, pos)),
                              SPET_LPARAM_IS_UNDEFINED);
        };
        sink.sentence = [&](uint32_t pos, uint32_t len) -> bool {
            return emit_event(u, SPEI_SENTENCE_BOUNDARY, 0, static_cast<WPARAM>(len),
                              static_cast<LPARAM>(map_offset(u, pos)),
                              SPET_LPARAM_IS_UNDEFINED);
        };

        std::string error;
        if (!sharedClient().speak(params, segments, sink, error) && !u.aborted) {
            FV_LOG("sapi: speak failed: %s", error.c_str());
            return E_FAIL;
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

}  // namespace sapi
}  // namespace FlexVoice
