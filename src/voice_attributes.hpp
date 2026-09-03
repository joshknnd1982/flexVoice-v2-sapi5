// voice_attributes.hpp -- the SAPI-facing view of the voice roster.
//
// Six tokens: the Custom Voice, then Mindmaker's five. The Custom Voice is
// first because it is the one a user is most likely to be looking for, and
// because it is the reason this wrapper exists: a screen reader can offer rate,
// pitch and volume, and FlexVoice has thirteen working parameters.

#pragma once

#include <windows.h>

#include <string>

#include "fv2_voices.hpp"

namespace fv2 {
namespace sapi {

struct VoiceToken {
    const char* name;        // without the prefix
    const char* baseVoice;   // which built-in .tav it is built from
    const char* gender;
    const char* age;
    bool        isCustom;
};

inline const VoiceToken* voice_tokens(int* count)
{
    static VoiceToken tokens[16];
    static int n = 0;
    if (n == 0) {
        tokens[n].name = customVoiceName();
        tokens[n].baseVoice = "Julie";   // overridden at speak time by the settings
        tokens[n].gender = "Female";
        tokens[n].age = "Adult";
        tokens[n].isCustom = true;
        ++n;
        const std::vector<VoiceInfo>& v = builtinVoices();
        for (size_t i = 0; i < v.size() && n < 16; ++i) {
            tokens[n].name = v[i].name;
            tokens[n].baseVoice = v[i].name;
            tokens[n].gender = v[i].gender;
            tokens[n].age = v[i].age;
            tokens[n].isCustom = false;
            ++n;
        }
    }
    if (count) *count = n;
    return tokens;
}

inline int voice_token_count()
{
    int n = 0;
    voice_tokens(&n);
    return n;
}

inline std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

class voice_attributes {
public:
    explicit voice_attributes(int index = 0)
    {
        const int n = voice_token_count();
        index_ = (index < 0 || index >= n) ? 0 : index;
    }

    [[nodiscard]] int token_index() const { return index_; }
    [[nodiscard]] const VoiceToken& def() const { return voice_tokens(nullptr)[index_]; }

    // "FlexVoice2 Julie" -- what appears in a voice list.
    [[nodiscard]] std::wstring get_name() const
    {
        return widen(std::string(voicePrefix()) + def().name);
    }
    // "FlexVoice2_Julie" -- the registry key name, and the prefix an uninstall
    // sweeps by.
    [[nodiscard]] std::wstring token_id() const
    {
        return widen(voiceTokenId(def().name));
    }
    [[nodiscard]] std::wstring get_gender() const { return widen(def().gender); }
    [[nodiscard]] std::wstring get_age() const { return widen(def().age); }
    // Only English data exists for FlexVoice 2.0; see the README.
    [[nodiscard]] std::wstring get_language() const { return L"409"; }
    [[nodiscard]] std::string base_voice() const { return def().baseVoice; }
    [[nodiscard]] bool is_custom() const { return def().isCustom; }

private:
    int index_ = 0;
};

}  // namespace sapi
}  // namespace fv2
