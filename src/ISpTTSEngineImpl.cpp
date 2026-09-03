#include "ISpTTSEngineImpl.hpp"

#include <climits>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "fv2_client.h"
#include "fv2_log.h"
#include "fv2_settings.h"
#include "fv2_text.hpp"
#include "utils.hpp"

namespace fv2 {
namespace sapi {

namespace {

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// SAPI rate is -10..+10 around the voice's own speed. Three-fold either way
// keeps +-10 usable rather than unintelligible, and the engine's own rate
// multiplier was measured to scale duration exactly across that whole span.
double rate_factor(int sapiRate) { return std::pow(3.0, clampi(sapiRate, -10, 10) / 10.0); }

// SAPI pitch is -10..+10; an octave either way, applied through pitchRate,
// which scales the whole F0 contour without touching duration.
double pitch_factor(int sapiPitch) { return std::pow(2.0, clampi(sapiPitch, -10, 10) / 10.0); }

struct Utterance {
    ISpTTSEngineSite* site = nullptr;
    ULONGLONG         bytesWritten = 0;
    bool              aborted = false;
    bool              wantWords = false;
    bool              wantSentences = false;

    // Word and sentence bookmarks carry an offset into the bytes we handed the
    // engine, not into the client's original string. This maps one to the other.
    std::vector<std::pair<uint32_t, uint32_t>> offsetMap;

    // Bookmark names, indexed by the integer id the engine round-trips.
    std::vector<std::wstring> bookmarks;
};

uint32_t map_offset(const Utterance& u, uint32_t engineOffset)
{
    if (u.offsetMap.empty()) return 0;
    if (engineOffset < u.offsetMap.size()) return u.offsetMap[engineOffset].second;
    return u.offsetMap.back().second;
}

// Start the engine host, once per process, off the calling thread. The first
// utterance would otherwise pay about 150 ms for launching it and loading the
// engine -- and the first utterance is usually the one the user is waiting on.
// Everything after that reuses the running host and costs about 5 ms.
DWORD WINAPI warm_up_thread(LPVOID)
{
    Fv2Pong pong = {};
    std::string error;
    if (!sharedClient().ping(pong, error)) {
        FV2_LOG("sapi: warm-up ping failed: %s", error.c_str());
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

    const int count = voice_token_count();
    int index = -1;

    // Written by write_voice_tokens, so the voice is recovered exactly rather
    // than by taking a display name apart.
    {
        ISpDataKey* attrs = nullptr;
        if (SUCCEEDED(pToken->OpenKey(L"Attributes", &attrs)) && attrs) {
            utils::out_ptr<wchar_t> value(CoTaskMemFree);
            if (SUCCEEDED(attrs->GetStringValue(L"Fv2Index", value.address())) && value.get()) {
                index = _wtoi(value.get());
            }
            if (index < 0 || index >= count) {
                // A token written by a different build may not carry the index;
                // fall back to matching the name so it still resolves.
                utils::out_ptr<wchar_t> name(CoTaskMemFree);
                if (SUCCEEDED(attrs->GetStringValue(L"Name", name.address())) && name.get()) {
                    for (int i = 0; i < count; ++i) {
                        if (_wcsicmp(name.get(), voice_attributes(i).get_name().c_str()) == 0) {
                            index = i;
                            break;
                        }
                    }
                }
            }
            attrs->Release();
        }
    }

    attr_ = voice_attributes(clampi(index < 0 ? 0 : index, 0, count - 1));
    FV2_LOG("sapi: token bound to voice %d (%S)", attr_.token_index(),
            attr_.get_name().c_str());
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

    // No engine work here: SAPI does not promise that GetOutputFormat and Speak
    // run on the same thread. But this is the first call SAPI makes after a
    // voice is selected, so start the host now, on a background thread, rather
    // than making the user's first keystroke pay for it.
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
        log::verbose() = s.verboseLogging;

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

        SpeakParams params;
        params.wantWordEvents = u.wantWords;
        params.wantSentenceEvents = u.wantSentences;

        // Two levels, kept strictly apart:
        //   .tav parameters    = who the voice is             (absolute units)
        //   engine multipliers = what the client asked for    (relative, 1.0 = as-is)
        //
        // The Custom Voice takes its .tav parameters from the utility. A named
        // voice takes them from its own file and nothing from the utility, or
        // every named voice would collapse onto the same sound. The host
        // applies each named voice's measured level trim.
        double userPitchRate = 1.0;

        if (attr_.is_custom()) {
            params.baseVoice = s.baseVoice;
            for (int i = 0; i < P_ACTIVE_COUNT; ++i) {
                const ParamId p = (ParamId)i;
                const double v = percentToValue(p, s.percent[i]);
                if (p == P_PITCH_RATE) {
                    // Held back so SAPI's own pitch can multiply it below.
                    userPitchRate = v;
                } else {
                    params.set(p, v);
                }
            }
        } else {
            params.baseVoice = attr_.base_voice();
        }

        // The client's rate, pitch and volume always apply, to every voice.
        // Rate and volume are engine-level multipliers: they take effect
        // immediately and never rewrite a voice file, which is what keeps a
        // rate change off the critical path of a keystroke.
        params.rate = rate_factor(static_cast<int>(sapiRate));
        params.volume = sapiVolume / 100.0;
        params.set(P_PITCH_RATE, userPitchRate * pitch_factor(0));

        // Build the segment list, tracking where each piece of engine text came
        // from so word events can be reported against the caller's offsets.
        std::vector<SpeakSegment> segments;
        uint32_t engineOffset = 0;
        int fragRate = INT_MIN, fragPitch = INT_MIN;
        long curVolume = -1;

        auto push_text = [&](const wchar_t* wide, uint32_t wideLen,
                             uint32_t srcOffset, text::Mode mode) {
            const text::Normalized n = text::normalize(wide, wideLen, mode);
            if (n.text.empty()) return;
            SpeakSegment seg;
            // Always FV2_SEG_TEXT: spelling has already happened here, where
            // the offset map that word events depend on can be built alongside.
            seg.kind = FV2_SEG_TEXT;
            seg.text = n.text;
            segments.push_back(seg);
            for (size_t i = 0; i < n.srcMap.size(); ++i) {
                u.offsetMap.push_back(std::make_pair(
                    engineOffset + static_cast<uint32_t>(i), srcOffset + n.srcMap[i]));
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
                seg.kind = FV2_SEG_BOOKMARK;
                seg.value = static_cast<uint32_t>(u.bookmarks.size());
                u.bookmarks.push_back(std::wstring(
                    frag->pTextStart ? frag->pTextStart : L"", frag->ulTextLen));
                segments.push_back(seg);
                continue;
            }
            if (frag->State.eAction == SPVA_Silence) {
                SpeakSegment seg;
                seg.kind = FV2_SEG_SILENCE;
                seg.value = static_cast<uint32_t>(clampi(frag->State.SilenceMSecs, 0, 60000));
                segments.push_back(seg);
                continue;
            }
            // Anything that is not speech -- ParseUnknown, Pronounce tables and
            // the rest -- must be skipped, or the engine reads the host's own
            // bookkeeping aloud. NVDA's bookmark numbers arriving as speech is
            // exactly what this prevents.
            if (frag->State.eAction != SPVA_Speak &&
                frag->State.eAction != SPVA_SpellOut &&
                frag->State.eAction != SPVA_Pronounce) {
                continue;
            }
            if (!frag->pTextStart || frag->ulTextLen == 0) continue;

            // Per-fragment rate, pitch and volume, emitted only when they
            // change, so the change lands at the right word rather than "as
            // soon as possible". They travel as numbers and the host applies
            // them, so no client text can ever be mistaken for a command.
            const int wantRate = clampi(frag->State.RateAdj, -10, 10);
            const int wantPitch = clampi(frag->State.PitchAdj.MiddleAdj, -10, 10);
            const long wantVolume = clampi(static_cast<int>(frag->State.Volume), 0, 100);

            auto push_param = [&](Fv2SegmentKind kind, int percent) {
                SpeakSegment seg;
                seg.kind = kind;
                seg.value = static_cast<uint32_t>(percent);
                segments.push_back(seg);
            };

            if (wantRate != fragRate) {
                fragRate = wantRate;
                const double f = rate_factor(static_cast<int>(sapiRate) + fragRate) /
                                 rate_factor(static_cast<int>(sapiRate));
                push_param(FV2_SEG_RATE,
                           clampi(static_cast<int>(f * 100.0 + 0.5), 10, 1000));
            }
            if (wantPitch != fragPitch) {
                fragPitch = wantPitch;
                push_param(FV2_SEG_PITCH,
                           clampi(static_cast<int>(pitch_factor(fragPitch) * 100.0 + 0.5),
                                  25, 400));
            }
            if (wantVolume != curVolume) {
                curVolume = wantVolume;
                push_param(FV2_SEG_VOLUME, clampi(static_cast<int>(curVolume), 0, 100));
            }

            // SPVA_SpellOut is what SAPI produces for the <spell> tag, which is
            // how a screen reader's spell-word command reaches an engine.
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
            // buffer was accepted in full. Trusting it truncates speech.
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
            FV2_LOG("sapi: speak failed: %s", error.c_str());
            return E_FAIL;
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

}  // namespace sapi
}  // namespace fv2
