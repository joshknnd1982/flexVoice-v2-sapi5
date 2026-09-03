#pragma once

#include <windows.h>
#include <sapi.h>
#include <string.h>

#include <string>

#include "registry.hpp"
#include "voice_attributes.hpp"

namespace fv2 {
namespace sapi {

// Voices are written as static registry tokens, and only as static registry
// tokens. There is no dynamic token enumerator: FlexVoice 3.01 registered one
// alongside these and SAPI merged the two lists without noticing they described
// the same voices, so every voice appeared twice. Static tokens are what every
// SAPI5 client reads, Windows Narrator included.
inline constexpr const wchar_t* voices_path =
    L"Software\\Microsoft\\Speech\\Voices\\Tokens";

inline void write_voice_tokens(HKEY root, const std::wstring& clsid_str)
{
    using namespace fv2::registry;

    key tokens(root, voices_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);

    const int count = voice_token_count();
    for (int i = 0; i < count; ++i) {
        const voice_attributes v(i);
        const std::wstring name = v.get_name();

        key token(tokens, v.token_id(), KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
        token.set(name);
        token.set(L"CLSID", clsid_str);
        // SAPI looks a display name up under a value named for the LCID it is
        // asking about, falling back to the key's default value set just above.
        token.set(v.get_language(), name);

        key attrs(token, L"Attributes", KEY_SET_VALUE, true);
        attrs.set(L"Name", name);
        attrs.set(L"Gender", v.get_gender());
        attrs.set(L"Age", v.get_age());
        attrs.set(L"Language", v.get_language());
        attrs.set(L"Vendor", L"Mindmaker");
        // Read back by SetObjectToken, so the voice is recovered exactly rather
        // than by parsing a display name apart.
        wchar_t index_str[16];
        swprintf_s(index_str, L"%d", i);
        attrs.set(L"Fv2Index", index_str);
    }
}

// Every token id this build writes starts with this. It is also what makes an
// orphan sweep safe: only keys under Speech\Voices\Tokens carrying this prefix
// are ours, so nothing another vendor wrote can ever match.
inline constexpr const wchar_t* token_prefix = L"FlexVoice2_";

// Removes every FlexVoice2 token, whether or not this build still knows its
// name, and returns how many went.
//
// Two things matter here, and getting the first one wrong is what broke every
// other SAPI5 voice on a machine that uninstalled FlexVoice 3.01:
//
//   * the delete is recursive. A token has an Attributes subkey, and
//     RegDeleteKeyW refuses to remove a key that has subkeys -- it returns
//     ERROR_ACCESS_DENIED. In 3.01 every removal silently failed inside a
//     catch(...), so uninstalling left the tokens behind naming a CLSID whose
//     DLL had just been deleted. A SAPI5 client then lists voices it cannot
//     instantiate, and because NVDA remembers its chosen voice by token path,
//     a user whose voice was one of those could not start the SAPI5
//     synthesiser at all -- for any voice, from any vendor.
//
//   * the sweep is by prefix, not by the current voice table, so a voice
//     renamed or dropped between releases still gets cleaned up.
inline int remove_voice_tokens(HKEY root) noexcept
{
    using namespace fv2::registry;
    int removed = 0;
    try {
        key tokens(root, voices_path, delete_access);
        const std::wstring prefix = token_prefix;

        for (const std::wstring& name : tokens.subkey_names()) {
            if (name.size() < prefix.size() ||
                _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) {
                continue;
            }
            if (tokens.delete_subtree(name)) {
                ++removed;
            }
        }
    }
    catch (...) {
    }
    return removed;
}

// If the machine's default voice points at one of ours, clear the pointer
// rather than leaving it dangling. A default-voice value naming a token that
// no longer exists is the other way to stop SAPI5 starting.
inline void clear_default_voice_if_ours(HKEY root) noexcept
{
    using namespace fv2::registry;
    try {
        key speech(root, L"Software\\Microsoft\\Speech\\Voices",
                   KEY_QUERY_VALUE | KEY_SET_VALUE);
        const std::wstring current = speech.get(L"DefaultTokenId");
        if (current.find(token_prefix) != std::wstring::npos) {
            speech.set(L"DefaultTokenId", L"");
        }
    }
    catch (...) {
    }
}

}  // namespace sapi
}  // namespace fv2
