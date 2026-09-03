// sapi_probe.cpp -- drive the real SAPI5 stack end to end without needing
// administrator rights.
//
// DllRegisterServer writes to HKLM, which needs elevation, so nothing would
// exercise the path SAPI actually takes until install time -- and a bug that
// only appears once SAPI reads a token back out of the registry would pass
// every other test. SAPI merges the voice-token lists from HKCU and HKLM, so
// registering the CLSID and the tokens under HKCU gives the identical code
// path with no elevation. (The *token enumerator* really is HKLM-only, which
// is why the installer still registers properly; the static tokens are what
// every client reads.)
//
//   sapi_probe register   <path to FlexVoiceSAPI.dll>
//   sapi_probe unregister
//   sapi_probe list
//   sapi_probe speak <voice name substring> <out.wav> [text]
//   sapi_probe events <voice name substring>
//   sapi_probe rate   <voice name substring> <out dir>

#ifdef _MSC_VER
#  pragma warning(disable : 4996)
#endif

#include <windows.h>
#include <sapi.h>
#include <stdio.h>

#include <string>
#include <vector>

// sphelper.h is not usable here: it pulls in ATL, which the Visual Studio Build
// Tools do not ship. Everything it would have provided is a handful of lines
// against the raw interfaces.
namespace {

ISpObjectToken* enum_voices(IEnumSpObjectTokens** out)
{
    *out = nullptr;
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&cat))) || !cat) {
        return nullptr;
    }
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
        cat->EnumTokens(nullptr, nullptr, out);
    }
    cat->Release();
    return nullptr;
}

// A token's display name is its default value, which is what SpGetDescription
// resolves to once the LCID lookup falls through.
WCHAR* token_description(ISpObjectToken* tok)
{
    WCHAR* desc = nullptr;
    if (FAILED(tok->GetStringValue(nullptr, &desc))) return nullptr;
    return desc;
}

void clear_event(SPEVENT& ev)
{
    if (ev.elParamType == SPET_LPARAM_IS_STRING ||
        ev.elParamType == SPET_LPARAM_IS_POINTER) {
        CoTaskMemFree(reinterpret_cast<void*>(ev.lParam));
    } else if (ev.elParamType == SPET_LPARAM_IS_TOKEN ||
               ev.elParamType == SPET_LPARAM_IS_OBJECT) {
        if (ev.lParam) reinterpret_cast<IUnknown*>(ev.lParam)->Release();
    }
    ev.lParam = 0;
}

ISpStream* bind_wav(const wchar_t* path, int sampleRate)
{
    ISpStream* stream = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                reinterpret_cast<void**>(&stream))) || !stream) {
        return nullptr;
    }
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    if (FAILED(stream->BindToFile(path, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx,
                                  &wfx, SPFEI_ALL_EVENTS))) {
        stream->Release();
        return nullptr;
    }
    return stream;
}


const wchar_t* kClassesPath = L"Software\\Classes\\CLSID";
const wchar_t* kTokensPath = L"Software\\Microsoft\\Speech\\Voices\\Tokens";

// Mirrors of the two CLSIDs in the DLL. Kept as literals so this tool does not
// have to link the wrapper's headers.
const wchar_t* kEngineClsid = L"{5E1FA20E-0311-4DBA-88A4-8455C601B75F}";

struct VoiceDef {
    const wchar_t* token;
    const wchar_t* name;
    const wchar_t* gender;
    const wchar_t* age;
    int            index;
};

const VoiceDef kVoices[] = {
    { L"FlexVoice_CustomVoice", L"FlexVoice Custom Voice", L"Female", L"Adult", 0 },
    { L"FlexVoice_Julie",       L"FlexVoice Julie",        L"Female", L"Adult", 1 },
    { L"FlexVoice_Kim",         L"FlexVoice Kim",          L"Female", L"Adult", 2 },
    { L"FlexVoice_Tim",         L"FlexVoice Tim",          L"Male",   L"Adult", 3 },
    { L"FlexVoice_Bill",        L"FlexVoice Bill",         L"Male",   L"Adult", 4 },
    { L"FlexVoice_Julius",      L"FlexVoice Julius",       L"Male",   L"Adult", 5 },
    { L"FlexVoice_Julia",       L"FlexVoice Julia",        L"Female", L"Adult", 6 },
    { L"FlexVoice_Jill",        L"FlexVoice Jill",         L"Female", L"Child", 7 },
    { L"FlexVoice_Kit",         L"FlexVoice Kit",          L"Male",   L"Child", 8 },
};
const int kVoiceCount = static_cast<int>(sizeof(kVoices) / sizeof(kVoices[0]));

bool set_value(HKEY root, const std::wstring& path, const wchar_t* name,
               const std::wstring& value)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void delete_tree(HKEY root, const std::wstring& path)
{
    RegDeleteTreeW(root, path.c_str());
}

int cmd_register(const wchar_t* dllPath)
{
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(dllPath, MAX_PATH, full, nullptr)) {
        printf("cannot resolve %S\n", dllPath);
        return 1;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        printf("no such file: %S\n", full);
        return 1;
    }

    const std::wstring inproc =
        std::wstring(kClassesPath) + L"\\" + kEngineClsid + L"\\InProcServer32";
    set_value(HKEY_CURRENT_USER, inproc, nullptr, full);
    set_value(HKEY_CURRENT_USER, inproc, L"ThreadingModel", L"Both");

    for (int i = 0; i < kVoiceCount; ++i) {
        const std::wstring token = std::wstring(kTokensPath) + L"\\" + kVoices[i].token;
        set_value(HKEY_CURRENT_USER, token, nullptr, kVoices[i].name);
        set_value(HKEY_CURRENT_USER, token, L"CLSID", kEngineClsid);
        set_value(HKEY_CURRENT_USER, token, L"409", kVoices[i].name);

        const std::wstring attrs = token + L"\\Attributes";
        set_value(HKEY_CURRENT_USER, attrs, L"Name", kVoices[i].name);
        set_value(HKEY_CURRENT_USER, attrs, L"Gender", kVoices[i].gender);
        set_value(HKEY_CURRENT_USER, attrs, L"Age", kVoices[i].age);
        set_value(HKEY_CURRENT_USER, attrs, L"Language", L"409");
        set_value(HKEY_CURRENT_USER, attrs, L"Vendor", L"MindMaker");

        wchar_t idx[16];
        swprintf_s(idx, L"%d", kVoices[i].index);
        set_value(HKEY_CURRENT_USER, token, L"TokenIndex", idx);
        set_value(HKEY_CURRENT_USER, attrs, L"FvIndex", idx);
    }
    printf("registered %d voices under HKCU pointing at %S\n", kVoiceCount, full);
    return 0;
}

int cmd_unregister()
{
    delete_tree(HKEY_CURRENT_USER,
                std::wstring(kClassesPath) + L"\\" + kEngineClsid);
    for (int i = 0; i < kVoiceCount; ++i) {
        delete_tree(HKEY_CURRENT_USER,
                    std::wstring(kTokensPath) + L"\\" + kVoices[i].token);
    }
    printf("unregistered\n");
    return 0;
}

// Build a token straight from a registry path. SAPI's category enumeration
// only reads HKLM unless the category carries a UserTokensPath value, and this
// machine's does not -- so an HKCU token is invisible to EnumTokens even though
// it is perfectly well formed. ISpObjectToken::SetId takes an absolute registry
// path including the root, which reaches it, and everything downstream of that
// point is the identical SAPI code path an installed voice takes.
ISpObjectToken* token_from_hkcu(const wchar_t* tokenKey)
{
    ISpObjectToken* tok = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                IID_ISpObjectToken, reinterpret_cast<void**>(&tok))) ||
        !tok) {
        return nullptr;
    }
    const std::wstring id = std::wstring(L"HKEY_CURRENT_USER\\") + kTokensPath +
                            L"\\" + tokenKey;
    if (FAILED(tok->SetId(nullptr, id.c_str(), FALSE))) {
        tok->Release();
        return nullptr;
    }
    return tok;
}

ISpObjectToken* find_token(const wchar_t* substr)
{
    ISpObjectToken* found = nullptr;
    IEnumSpObjectTokens* en = nullptr;
    enum_voices(&en);
    if (!en) {
        // Fall back to the per-user tokens this tool wrote.
        for (int i = 0; i < kVoiceCount; ++i) {
            if (!substr || wcsstr(kVoices[i].name, substr)) {
                return token_from_hkcu(kVoices[i].token);
            }
        }
        return nullptr;
    }

    ISpObjectToken* tok = nullptr;
    while (en->Next(1, &tok, nullptr) == S_OK && tok) {
        WCHAR* desc = token_description(tok);
        if (desc) {
            if (!substr || wcsstr(desc, substr)) {
                CoTaskMemFree(desc);
                found = tok;
                break;
            }
            CoTaskMemFree(desc);
        }
        tok->Release();
        tok = nullptr;
    }
    en->Release();

    if (!found) {
        for (int i = 0; i < kVoiceCount; ++i) {
            if (!substr || wcsstr(kVoices[i].name, substr)) {
                found = token_from_hkcu(kVoices[i].token);
                if (found) {
                    printf("  (not in the category enumeration; reached the HKCU "
                           "token \"%S\" directly)\n", kVoices[i].token);
                }
                break;
            }
        }
    }
    return found;
}

int cmd_list()
{
    IEnumSpObjectTokens* en = nullptr;
    enum_voices(&en);
    if (!en) {
        printf("cannot enumerate the voice category\n");
        return 1;
    }
    ULONG count = 0;
    en->GetCount(&count);
    printf("%lu SAPI5 voices installed:\n", count);

    ISpObjectToken* tok = nullptr;
    while (en->Next(1, &tok, nullptr) == S_OK && tok) {
        WCHAR* desc = token_description(tok);
        ISpDataKey* attrs = nullptr;
        WCHAR* gender = nullptr;
        WCHAR* lang = nullptr;
        if (SUCCEEDED(tok->OpenKey(L"Attributes", &attrs)) && attrs) {
            attrs->GetStringValue(L"Gender", &gender);
            attrs->GetStringValue(L"Language", &lang);
            attrs->Release();
        }
        printf("  %-34S %-8S %S\n", desc ? desc : L"?", gender ? gender : L"-",
               lang ? lang : L"-");
        if (desc) CoTaskMemFree(desc);
        if (gender) CoTaskMemFree(gender);
        if (lang) CoTaskMemFree(lang);
        tok->Release();
        tok = nullptr;
    }
    en->Release();
    return 0;
}

int speak_to_wav(const wchar_t* voiceSubstr, const wchar_t* outPath,
                 const wchar_t* text, long rate, long pitchPct, USHORT volume)
{
    ISpObjectToken* tok = find_token(voiceSubstr);
    if (!tok) {
        printf("voice matching \"%S\" not found\n", voiceSubstr);
        return 1;
    }

    ISpVoice* voice = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&voice)))) {
        printf("cannot create SpVoice\n");
        tok->Release();
        return 1;
    }
    if (FAILED(voice->SetVoice(tok))) {
        printf("SetVoice failed\n");
        voice->Release();
        tok->Release();
        return 1;
    }
    voice->SetRate(rate);
    voice->SetVolume(volume);

    ISpStream* stream = bind_wav(outPath, 16000);
    if (!stream) {
        printf("cannot open %S\n", outPath);
        voice->Release();
        tok->Release();
        return 1;
    }
    voice->SetOutput(stream, TRUE);

    std::wstring xml(text);
    if (pitchPct != 0) {
        wchar_t buf[64];
        swprintf_s(buf, L"<pitch middle=\"%ld\"/>", pitchPct);
        xml = std::wstring(buf) + xml;
    }

    const DWORD t0 = GetTickCount();
    const HRESULT hr = voice->Speak(xml.c_str(), SPF_DEFAULT, nullptr);
    voice->WaitUntilDone(60000);
    const DWORD ms = GetTickCount() - t0;

    stream->Close();
    stream->Release();
    voice->Release();
    tok->Release();

    if (FAILED(hr)) {
        printf("Speak failed, hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    GetFileAttributesExW(outPath, GetFileExInfoStandard, &fad);
    printf("  %S  rate %+ld pitch %+ld vol %u -> %lu bytes in %lu ms\n",
           outPath, rate, pitchPct, volume, fad.nFileSizeLow,
           static_cast<unsigned long>(ms));
    return 0;
}

int cmd_events(const wchar_t* voiceSubstr)
{
    ISpObjectToken* tok = find_token(voiceSubstr);
    if (!tok) { printf("voice not found\n"); return 1; }

    ISpVoice* voice = nullptr;
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                     reinterpret_cast<void**>(&voice));
    if (!voice) { tok->Release(); return 1; }
    voice->SetVoice(tok);
    voice->SetInterest(SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
                           SPFEI(SPEI_TTS_BOOKMARK) | SPFEI(SPEI_END_INPUT_STREAM),
                       SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
                           SPFEI(SPEI_TTS_BOOKMARK) | SPFEI(SPEI_END_INPUT_STREAM));

    const wchar_t* xml =
        L"Hello there. <bookmark mark=\"7\"/>This is the second sentence, "
        L"with <silence msec=\"250\"/>a pause and a <spell>SAPI</spell> spell out.";

    voice->Speak(xml, SPF_ASYNC | SPF_IS_XML, nullptr);

    int words = 0, sentences = 0, marks = 0;
    bool done = false;
    const DWORD deadline = GetTickCount() + 30000;
    while (!done && GetTickCount() < deadline) {
        if (voice->WaitForNotifyEvent(200) != S_OK) continue;
        SPEVENT ev = {};
        ULONG fetched = 0;
        while (voice->GetEvents(1, &ev, &fetched) == S_OK && fetched) {
            switch (ev.eEventId) {
            case SPEI_WORD_BOUNDARY:
                if (words < 8) {
                    printf("  word   offset %lu len %lu at audio byte %llu\n",
                           static_cast<unsigned long>(ev.lParam),
                           static_cast<unsigned long>(ev.wParam),
                           ev.ullAudioStreamOffset);
                }
                ++words;
                break;
            case SPEI_SENTENCE_BOUNDARY:
                printf("  sentence offset %lu len %lu\n",
                       static_cast<unsigned long>(ev.lParam),
                       static_cast<unsigned long>(ev.wParam));
                ++sentences;
                break;
            case SPEI_TTS_BOOKMARK:
                printf("  bookmark \"%S\" value %lu\n",
                       reinterpret_cast<const wchar_t*>(ev.lParam),
                       static_cast<unsigned long>(ev.wParam));
                ++marks;
                break;
            case SPEI_END_INPUT_STREAM:
                done = true;
                break;
            default:
                break;
            }
            clear_event(ev);
            fetched = 0;
        }
    }
    printf("  totals: %d word, %d sentence, %d bookmark events; stream %s\n",
           words, sentences, marks, done ? "ended" : "DID NOT END");
    voice->Release();
    tok->Release();
    return (words > 0 && marks > 0 && done) ? 0 : 1;
}

int cmd_rate(const wchar_t* voiceSubstr, const wchar_t* outDir)
{
    CreateDirectoryW(outDir, nullptr);
    const wchar_t* text = L"She had your dark suit in greasy wash water all year.";
    for (long r = -10; r <= 10; r += 5) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\rate_%+03ld.wav", outDir, r);
        speak_to_wav(voiceSubstr, path, text, r, 0, 100);
    }
    for (long p = -10; p <= 10; p += 5) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\pitch_%+03ld.wav", outDir, p);
        speak_to_wav(voiceSubstr, path, text, 0, p, 100);
    }
    for (USHORT v = 25; v <= 100; v = static_cast<USHORT>(v + 25)) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\vol_%03u.wav", outDir, v);
        speak_to_wav(voiceSubstr, path, text, 0, 0, v);
    }
    return 0;
}

std::wstring widen(const char* s)
{
    if (!s) return L"";
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: sapi_probe register <dll> | unregister | list |\n"
               "       speak <voice> <out.wav> [text] | events <voice> | rate <voice> <dir>\n");
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "register") {
        if (argc < 3) { printf("register needs a DLL path\n"); return 2; }
        return cmd_register(widen(argv[2]).c_str());
    }
    if (cmd == "unregister") return cmd_unregister();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int rc = 2;
    if (cmd == "list") {
        rc = cmd_list();
    } else if (cmd == "speak" && argc >= 4) {
        rc = speak_to_wav(widen(argv[2]).c_str(), widen(argv[3]).c_str(),
                          argc > 4 ? widen(argv[4]).c_str()
                                   : L"The quick brown fox jumps over the lazy dog.",
                          0, 0, 100);
    } else if (cmd == "events" && argc >= 3) {
        rc = cmd_events(widen(argv[2]).c_str());
    } else if (cmd == "rate" && argc >= 4) {
        rc = cmd_rate(widen(argv[2]).c_str(), widen(argv[3]).c_str());
    } else {
        printf("unknown or incomplete command\n");
    }
    CoUninitialize();
    return rc;
}
