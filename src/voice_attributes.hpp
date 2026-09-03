#pragma once

#include <string>

#include "voice_data.hpp"

namespace FlexVoice {
namespace sapi {

// A SAPI voice token's worth of information, derived from one entry in the
// voices::kVoices table. Kept as a value type so the enumerator can hold a
// vector of them and hand each to a voice_token.
class voice_attributes {
public:
    voice_attributes() = default;
    explicit voice_attributes(int index) : index_(index) {}

    int token_index() const noexcept { return index_; }

    const voices::Voice& def() const noexcept
    {
        const int i = (index_ >= 0 && index_ < voices::kVoiceCount) ? index_ : 0;
        return voices::kVoices[i];
    }

    std::wstring get_name() const { return def().displayName; }
    std::wstring get_gender() const { return def().gender; }
    std::wstring get_age() const { return def().age; }

    std::wstring get_language() const
    {
        return voices::kLanguages[def().languageIndex].lcid;
    }

    uint32_t language_id() const
    {
        return voices::kLanguages[def().languageIndex].id;
    }

    bool is_custom() const noexcept { return def().isCustom; }

    // Stable registry token id: no spaces, unique per voice.
    std::wstring token_id() const
    {
        std::wstring id = L"FlexVoice_";
        const std::wstring name = get_name();
        // "FlexVoice Julie" -> "Julie"; anything non-alphanumeric becomes '_'.
        const std::wstring prefix = L"FlexVoice ";
        const std::wstring tail =
            name.compare(0, prefix.size(), prefix) == 0 ? name.substr(prefix.size()) : name;
        for (wchar_t c : tail) {
            id += (iswalnum(c) ? c : L'_');
        }
        return id;
    }

private:
    int index_ = 0;
};

}  // namespace sapi
}  // namespace FlexVoice
