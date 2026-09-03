// registry_test.cpp -- the uninstall path, tested against the code that
// actually runs at uninstall time.
//
// 1.0.2 shipped with remove_voice_tokens() unable to remove anything. It called
// RegDeleteKeyW, which refuses to delete a key that has subkeys, and every
// voice token has an Attributes subkey -- so uninstalling FlexVoice left nine
// tokens on the machine naming a CLSID whose DLL had just been deleted. Any
// SAPI5 client then listed nine voices it could not create, and a user whose
// default voice was one of them lost speech everywhere.
//
// sapi_probe did not catch it because it registers and unregisters with its own
// RegDeleteTreeW helper: the test cleaned up correctly while the product did
// not. This one drives the product's own functions.
//
// Everything happens under a sandbox key in HKCU. write_voice_tokens() and
// remove_voice_tokens() take the root as a parameter, so handing them a private
// key exercises the identical code with nothing machine-wide at stake.

#include <windows.h>
#include <stdio.h>
#include <string>

#include "registry.hpp"
#include "voice_registry.hpp"

namespace {

const wchar_t* kSandbox = L"Software\\FlexVoiceSAPI\\RegistryTest";
const wchar_t* kClsid = L"{5E1FA20E-0311-4DBA-88A4-8455C601B75F}";

int g_failures = 0;

void check(bool ok, const char* what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

int count_flexvoice_tokens(HKEY sandbox)
{
    HKEY tokens = nullptr;
    if (RegOpenKeyExW(sandbox, L"Software\\Microsoft\\Speech\\Voices\\Tokens", 0,
                      KEY_READ, &tokens) != ERROR_SUCCESS) {
        return 0;
    }
    int n = 0;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD size = 256;
        if (RegEnumKeyExW(tokens, i, name, &size, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) {
            break;
        }
        if (_wcsnicmp(name, FlexVoice::sapi::token_prefix,
                      wcslen(FlexVoice::sapi::token_prefix)) == 0) {
            ++n;
        }
    }
    RegCloseKey(tokens);
    return n;
}

}  // namespace

int main()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FlexVoiceSAPI\\RegistryTest");

    HKEY sandbox = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSandbox, 0, nullptr, 0,
                        KEY_READ | KEY_WRITE, nullptr, &sandbox, nullptr) != ERROR_SUCCESS) {
        printf("cannot create the sandbox key\n");
        return 1;
    }

    printf("write_voice_tokens / remove_voice_tokens round trip\n");

    FlexVoice::sapi::write_voice_tokens(sandbox, kClsid);
    const int written = count_flexvoice_tokens(sandbox);
    check(written == FlexVoice::voices::kVoiceCount, "every voice was written");

    // The specific shape that broke 1.0.2: the token carries a subkey.
    HKEY attrs = nullptr;
    const bool hasAttrs =
        RegOpenKeyExW(sandbox,
                      L"Software\\Microsoft\\Speech\\Voices\\Tokens\\FlexVoice_Julie\\Attributes",
                      0, KEY_READ, &attrs) == ERROR_SUCCESS;
    if (attrs) RegCloseKey(attrs);
    check(hasAttrs, "a token has an Attributes subkey (the 1.0.2 trap)");

    const int removed = FlexVoice::sapi::remove_voice_tokens(sandbox);
    check(removed == written, "remove_voice_tokens reports removing them all");
    check(count_flexvoice_tokens(sandbox) == 0, "no FlexVoice token survives the sweep");

    printf("\nthe sweep is by prefix, so a renamed voice is still cleaned up\n");
    FlexVoice::sapi::write_voice_tokens(sandbox, kClsid);
    HKEY stale = nullptr;
    RegCreateKeyExW(sandbox,
                    L"Software\\Microsoft\\Speech\\Voices\\Tokens\\FlexVoice_VoiceFromAnOlderRelease\\Attributes",
                    0, nullptr, 0, KEY_WRITE, nullptr, &stale, nullptr);
    if (stale) RegCloseKey(stale);
    check(count_flexvoice_tokens(sandbox) == FlexVoice::voices::kVoiceCount + 1,
          "a token this build does not know about is present");
    FlexVoice::sapi::remove_voice_tokens(sandbox);
    check(count_flexvoice_tokens(sandbox) == 0, "it is swept too");

    printf("\nthe sweep leaves other vendors alone\n");
    FlexVoice::sapi::write_voice_tokens(sandbox, kClsid);
    HKEY other = nullptr;
    RegCreateKeyExW(sandbox,
                    L"Software\\Microsoft\\Speech\\Voices\\Tokens\\SomeOtherVendor_Voice\\Attributes",
                    0, nullptr, 0, KEY_WRITE, nullptr, &other, nullptr);
    if (other) RegCloseKey(other);
    FlexVoice::sapi::remove_voice_tokens(sandbox);
    HKEY survivor = nullptr;
    const bool survived =
        RegOpenKeyExW(sandbox, L"Software\\Microsoft\\Speech\\Voices\\Tokens\\SomeOtherVendor_Voice",
                      0, KEY_READ, &survivor) == ERROR_SUCCESS;
    if (survivor) RegCloseKey(survivor);
    check(survived, "another vendor's token is untouched");

    printf("\ndelete_subtree refuses an empty name\n");
    {
        // RegDeleteTreeW with an empty subkey empties the key the handle names.
        // For these callers that key is Speech\\Voices\\Tokens: every voice on
        // the machine, from every vendor. It must never be reachable.
        FlexVoice::registry::key tokens(sandbox, L"Software\\Microsoft\\Speech\\Voices\\Tokens",
                                        FlexVoice::registry::delete_access, true);
        HKEY canary = nullptr;
        RegCreateKeyExW(sandbox, L"Software\\Microsoft\\Speech\\Voices\\Tokens\\Canary",
                        0, nullptr, 0, KEY_WRITE, nullptr, &canary, nullptr);
        if (canary) RegCloseKey(canary);

        check(!tokens.delete_subtree(L""), "delete_subtree(\"\") returns false");
        HKEY still = nullptr;
        const bool alive =
            RegOpenKeyExW(sandbox, L"Software\\Microsoft\\Speech\\Voices\\Tokens\\Canary",
                          0, KEY_READ, &still) == ERROR_SUCCESS;
        if (still) RegCloseKey(still);
        check(alive, "and deletes nothing");
    }

    RegCloseKey(sandbox);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FlexVoiceSAPI\\RegistryTest");
    HKEY leftover = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\FlexVoiceSAPI", 0, KEY_READ, &leftover)
            == ERROR_SUCCESS) {
        RegCloseKey(leftover);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\FlexVoiceSAPI");
    }

    printf("\n%s\n", g_failures ? "FAILURES" : "all checks passed");
    return g_failures ? 1 : 0;
}
