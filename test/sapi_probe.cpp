// sapi_probe -- drive the wrapper the way a screen reader does: through SAPI 5.
//
// Registers into HKCU so it needs no administrator, then goes out through
// ISpVoice and back. Everything before this point tests our own code calling
// our own code; this is the first thing that proves Windows can find the voice,
// create it, and get audio out of it.
//
// It deliberately calls the SHIPPING write_voice_tokens and remove_voice_tokens
// rather than helpers of its own. FlexVoice 3.01 had a probe with its own
// RegDeleteTree cleanup, so the test tidied up correctly while the product left
// every token behind -- and that bug reached users.

#include <windows.h>
#include <sapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/registry.hpp"
#include "../src/voice_registry.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    ++g_checks;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::wstring module_dir()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/"));
}

const wchar_t* kClsid = L"{b7e42d61-3c95-4a18-9f6b-2ed40c8a5713}";

bool register_clsid(const std::wstring& dll)
{
    using namespace fv2::registry;
    try {
        key inproc(HKEY_CURRENT_USER,
                   std::wstring(L"Software\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32",
                   KEY_SET_VALUE | KEY_CREATE_SUB_KEY, true);
        inproc.set(dll);
        inproc.set(L"ThreadingModel", L"Both");
        return true;
    } catch (...) { return false; }
}

void unregister_clsid()
{
    using namespace fv2::registry;
    try {
        key classes(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", delete_access);
        classes.delete_subtree(kClsid);
    } catch (...) {}
}

// A token from another vendor, planted before the sweep and checked after, so
// "the sweep removed our tokens" is distinguishable from "the sweep removed
// everything".
const wchar_t* kBystander = L"SomeOtherVendorVoice";

void plant_bystander()
{
    using namespace fv2::registry;
    try {
        key tok(HKEY_CURRENT_USER,
                std::wstring(fv2::sapi::voices_path) + L"\\" + kBystander,
                KEY_SET_VALUE | KEY_CREATE_SUB_KEY, true);
        tok.set(L"A voice belonging to somebody else");
        key attrs(tok, L"Attributes", KEY_SET_VALUE, true);
        attrs.set(L"Name", L"Somebody Else");
    } catch (...) {}
}

bool bystander_survives()
{
    using namespace fv2::registry;
    try {
        key tok(HKEY_CURRENT_USER,
                std::wstring(fv2::sapi::voices_path) + L"\\" + kBystander, KEY_READ);
        return true;
    } catch (...) { return false; }
}

void remove_bystander()
{
    using namespace fv2::registry;
    try {
        key tokens(HKEY_CURRENT_USER, fv2::sapi::voices_path, delete_access);
        tokens.delete_subtree(kBystander);
    } catch (...) {}
}

int count_our_tokens()
{
    using namespace fv2::registry;
    int n = 0;
    try {
        key tokens(HKEY_CURRENT_USER, fv2::sapi::voices_path, KEY_READ);
        for (const std::wstring& name : tokens.subkey_names()) {
            if (name.compare(0, wcslen(fv2::sapi::token_prefix), fv2::sapi::token_prefix) == 0) {
                ++n;
            }
        }
    } catch (...) {}
    return n;
}


// sphelper.h is not usable here: it needs ATL, which the Build Tools install
// does not carry. These are the two things it would have provided.
HRESULT token_from_id(const std::wstring& id, ISpObjectToken** out)
{
    *out = nullptr;
    ISpObjectToken* tok = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                                  IID_ISpObjectToken, (void**)&tok);
    if (FAILED(hr)) return hr;
    hr = tok->SetId(nullptr, id.c_str(), FALSE);
    if (FAILED(hr)) { tok->Release(); return hr; }
    *out = tok;
    return S_OK;
}

void wave_format(WAVEFORMATEX* w)
{
    w->wFormatTag = WAVE_FORMAT_PCM;
    w->nChannels = 1;
    w->nSamplesPerSec = 16000;
    w->wBitsPerSample = 16;
    w->nBlockAlign = 2;
    w->nAvgBytesPerSec = 32000;
    w->cbSize = 0;
}

HRESULT bind_wav(const wchar_t* path, ISpStream** out)
{
    *out = nullptr;
    ISpStream* st = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                  IID_ISpStream, (void**)&st);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX wfx;
    wave_format(&wfx);
    hr = st->BindToFile(path, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
    if (FAILED(hr)) { st->Release(); return hr; }
    *out = st;
    return S_OK;
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::wstring dll = (argc > 1)
        ? std::wstring(argv[1], argv[1] + strlen(argv[1]))
        : module_dir() + L"\\FlexVoice2SAPI.dll";

    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wprintf(L"cannot find %s\n", dll.c_str());
        return 2;
    }
    std::wprintf(L"DLL: %s\n\n", dll.c_str());

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    std::printf("registration (HKCU, no administrator needed)\n");
    check(register_clsid(dll), "CLSID registered");
    plant_bystander();
    try {
        fv2::sapi::write_voice_tokens(HKEY_CURRENT_USER, kClsid);
        check(true, "voice tokens written");
    } catch (...) {
        check(false, "voice tokens written");
    }
    const int written = count_our_tokens();
    std::printf("  %d tokens under HKCU\n", written);
    check(written == fv2::sapi::voice_token_count(), "one token per voice");

    std::printf("\nSAPI can find and speak every voice\n");
    {
        ISpVoice* voice = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                      IID_ISpVoice, (void**)&voice);
        check(SUCCEEDED(hr) && voice, "created an ISpVoice");

        if (voice) {
            for (int i = 0; i < fv2::sapi::voice_token_count(); ++i) {
                const fv2::sapi::voice_attributes v(i);
                const std::wstring want = v.get_name();

                ISpObjectToken* token = nullptr;
                std::wstring path = std::wstring(L"HKEY_CURRENT_USER\\") +
                                    fv2::sapi::voices_path + L"\\" + v.token_id();
                hr = token_from_id(path, &token);
                if (FAILED(hr) || !token) {
                    std::wprintf(L"  FAIL could not open the token for %s\n", want.c_str());
                    ++g_failures; ++g_checks;
                    continue;
                }

                hr = voice->SetVoice(token);
                token->Release();
                if (FAILED(hr)) {
                    std::wprintf(L"  FAIL SetVoice for %s (0x%08lx)\n", want.c_str(), hr);
                    ++g_failures; ++g_checks;
                    continue;
                }

                // Speak into a memory stream so nothing plays out loud and the
                // byte count can be checked.
                ISpStream* stream = nullptr;
                wchar_t tmpDir[MAX_PATH], tmpFile[MAX_PATH];
                GetTempPathW(MAX_PATH, tmpDir);
                GetTempFileNameW(tmpDir, L"fv2", 0, tmpFile);
                hr = bind_wav(tmpFile, &stream);
                if (SUCCEEDED(hr)) {
                    voice->SetOutput(stream, TRUE);
                    const DWORD t0 = GetTickCount();
                    hr = voice->Speak(L"The quick brown fox jumps over the lazy dog.",
                                      SPF_DEFAULT, nullptr);
                    const DWORD ms = GetTickCount() - t0;
                    stream->Close();
                    stream->Release();

                    WIN32_FILE_ATTRIBUTE_DATA fad = {};
                    GetFileAttributesExW(tmpFile, GetFileExInfoStandard, &fad);
                    const DWORD bytes = fad.nFileSizeLow;
                    std::wprintf(L"  %-24s %6lu bytes in %lu ms\n",
                                 want.c_str(), bytes, ms);
                    ++g_checks;
                    if (FAILED(hr) || bytes < 20000) {
                        std::printf("  FAIL that voice produced no usable audio\n");
                        ++g_failures;
                    }
                }
                DeleteFileW(tmpFile);
            }

            // Rate and volume must reach the engine through SAPI's own knobs.
            std::printf("\nSAPI rate reaches the engine\n");
            {
                auto lengthAt = [&](long rate) -> DWORD {
                    voice->SetRate(rate);
                    ISpStream* st = nullptr;
                    wchar_t d[MAX_PATH], p[MAX_PATH];
                    GetTempPathW(MAX_PATH, d);
                    GetTempFileNameW(d, L"fv2", 0, p);
                    DWORD bytes = 0;
                    if (SUCCEEDED(bind_wav(p, &st))) {
                        voice->SetOutput(st, TRUE);
                        voice->Speak(L"The quick brown fox jumps over the lazy dog.",
                                     SPF_DEFAULT, nullptr);
                        st->Close();
                        st->Release();
                        WIN32_FILE_ATTRIBUTE_DATA a = {};
                        GetFileAttributesExW(p, GetFileExInfoStandard, &a);
                        bytes = a.nFileSizeLow;
                    }
                    DeleteFileW(p);
                    return bytes;
                };
                const DWORD slow = lengthAt(-8);
                const DWORD fast = lengthAt(8);
                std::printf("  rate -8: %lu bytes, rate +8: %lu bytes\n", slow, fast);
                check(slow > fast * 2, "a slower rate produces markedly more audio");
                voice->SetRate(0);
            }

            voice->Release();
        }
    }

    std::printf("\nuninstall leaves nothing behind\n");
    {
        const int removed = fv2::sapi::remove_voice_tokens(HKEY_CURRENT_USER);
        std::printf("  removed %d tokens\n", removed);
        check(removed == written, "every token this build wrote was removed");
        check(count_our_tokens() == 0, "no FlexVoice2 token survives");
        check(bystander_survives(), "another vendor's token is untouched");
    }

    remove_bystander();
    unregister_clsid();
    CoUninitialize();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
