#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <stdexcept>

namespace FlexVoice {
namespace registry {

class error : public std::runtime_error
{
public:
    explicit error(const std::string& msg) : std::runtime_error(msg) {}
};

// What RegDeleteTreeW documents it needs on the handle it is given, plus
// KEY_SET_VALUE because it removes values as well as subkeys. Asking for
// KEY_ALL_ACCESS instead would also demand WRITE_DAC and WRITE_OWNER, which
// nothing here needs.
inline constexpr REGSAM delete_access =
    DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE;

class key
{
public:
    key(HKEY parent, const std::wstring& name, REGSAM access_mask = KEY_READ, bool create = false)
    {
        const LONG result = create
            ? RegCreateKeyExW(parent, name.c_str(), 0, nullptr, 0, access_mask, nullptr, &handle_, nullptr)
            : RegOpenKeyExW(parent, name.c_str(), 0, access_mask, &handle_);

        if (result != ERROR_SUCCESS) {
            throw error("Unable to open/create a registry key");
        }
    }

    ~key()
    {
        if (handle_) {
            RegCloseKey(handle_);
        }
    }

    key(const key&) = delete;
    key& operator=(const key&) = delete;

    key(key&& other) noexcept : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    key& operator=(key&& other) noexcept
    {
        if (this != &other) {
            if (handle_) {
                RegCloseKey(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] operator HKEY() const noexcept
    {
        return handle_;
    }

    // RegDeleteKeyW removes a key only when it has no subkeys of its own.
    // Every SAPI voice token has an Attributes subkey, so this quietly failed
    // on every token FlexVoice ever wrote and the uninstall left all nine
    // behind, pointing at a DLL that was no longer there. Deleting a registry
    // key here therefore always means deleting the subtree.
    //
    // An empty name is refused rather than passed through: RegDeleteTreeW with
    // a null or empty subkey empties the key the handle refers to, which for
    // these callers would be Speech\Voices\Tokens -- every voice on the
    // machine, from every vendor.
    [[nodiscard]] bool delete_subtree(const std::wstring& name) noexcept
    {
        if (name.empty()) {
            return false;
        }
        const LONG r = RegDeleteTreeW(handle_, name.c_str());
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }

    void delete_subkey(const std::wstring& name)
    {
        if (!delete_subtree(name)) {
            throw error("Unable to delete a registry key");
        }
    }

    // Subkey names, for sweeps that must catch keys this build does not know
    // the names of -- a voice renamed since the version that wrote it.
    [[nodiscard]] std::vector<std::wstring> subkey_names() const;

    [[nodiscard]] std::wstring get(const std::wstring& name) const;

    [[nodiscard]] std::wstring get() const
    {
        return get(L"");
    }

    void set(const std::wstring& name, const std::wstring& value);

    void set(const std::wstring& value)
    {
        set(L"", value);
    }

private:
    HKEY handle_ = nullptr;
};

}
}
