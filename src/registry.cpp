#include <vector>
#include "registry.hpp"

namespace FlexVoice {
namespace registry {

std::wstring key::get(const std::wstring& name) const
{
    DWORD type = 0;
    DWORD size = 0;

    if (RegQueryValueExW(handle_, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
        if (type == REG_SZ) {
            std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
            if (RegQueryValueExW(handle_, name.c_str(), nullptr, &type,
                                  reinterpret_cast<BYTE*>(buffer.data()), &size) == ERROR_SUCCESS) {
                if (type == REG_SZ) {
                    return std::wstring(buffer.data());
                }
            }
        }
    }

    throw error("Unable to read a value from the registry");
}

std::vector<std::wstring> key::subkey_names() const
{
    std::vector<std::wstring> names;
    // Enumerating while deleting skips entries, so the whole list is taken
    // first and the caller deletes afterwards.
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD size = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        const LONG r = RegEnumKeyExW(handle_, i, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (r != ERROR_SUCCESS) {
            break;
        }
        names.emplace_back(name, size);
    }
    return names;
}

void key::set(const std::wstring& name, const std::wstring& value)
{
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(handle_, name.c_str(), 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()), size) != ERROR_SUCCESS) {
        throw error("Unable to write a value in the registry");
    }
}
}
}
