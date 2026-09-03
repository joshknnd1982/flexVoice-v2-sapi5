#pragma once

#include <windows.h>
#include <sapi.h>
#include <string.h>

#include <string>

#include "registry.hpp"
#include "voice_attributes.hpp"

namespace FlexVoice {
namespace sapi {

// Every voice is written as a static registry token as well as being offered
// by the dynamic enumerator. Static tokens are what every SAPI5 client reads,
// Windows Narrator included, so they are the form with the widest reach; the
// enumerator exists so a future runtime-discovered voice list stays in sync
// without a re-registration.
inline constexpr const wchar_t* voices_path =
    L"Software\\Microsoft\\Speech\\Voices\\Tokens";

inline void write_voice_tokens(HKEY root, const std::wstring& clsid_str)
{
    using namespace FlexVoice::registry;

    key tokens(root, voices_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);

    for (int i = 0; i < voices::kVoiceCount; ++i) {
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
        attrs.set(L"Vendor", L"MindMaker");
        // Read back by SetObjectToken, so the voice is recovered exactly rather
        // than by parsing a display name apart.
        wchar_t index_str[16];
        swprintf_s(index_str, L"%d", i);
        attrs.set(L"FvIndex", index_str);
    }
}

// Every token id this build writes starts with this. It is also what makes an
// orphan sweep safe: only keys under Speech\Voices\Tokens carrying this prefix
// are ours, so nothing another vendor wrote can ever match.
inline constexpr const wchar_t* token_prefix = L"FlexVoice_";

// Removes every FlexVoice token, whether or not this build still knows its
// name, and returns how many went. Two things matter here and neither did
// before:
//
//   * the delete is recursive. A token has an Attributes subkey, and
//     RegDeleteKeyW refuses to remove a key that has subkeys -- it returns
//     ERROR_ACCESS_DENIED. Every removal silently failed, so uninstalling
//     left nine tokens behind naming a CLSID whose DLL had just been deleted.
//     A SAPI5 client then lists nine voices it cannot instantiate, and if one
//     of them was the user's default voice, speech stops everywhere.
//
//   * the sweep is by prefix, not by the current voice table, so a voice
//     renamed or dropped between releases still gets cleaned up.
inline int remove_voice_tokens(HKEY root) noexcept
{
    using namespace FlexVoice::registry;
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

}  // namespace sapi
}  // namespace FlexVoice
