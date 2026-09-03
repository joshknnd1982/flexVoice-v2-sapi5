#include <new>
#include <comdef.h>
#include "voice_token.hpp"
#include "ISpTTSEngineImpl.hpp"

namespace FlexVoice {
namespace sapi {

voice_token::voice_token(const voice_attributes& attr)
{
    const std::wstring name = attr.get_name();
    set(name);

    utils::out_ptr<wchar_t> clsid_str(CoTaskMemFree);
    StringFromCLSID(__uuidof(ISpTTSEngineImpl), clsid_str.address());
    set(L"CLSID", clsid_str.get());

    // SAPI looks a display name up under a value named for the LCID it is
    // asking about, falling back to the key's default value.
    set(attr.get_language(), name);

    // How ISpTTSEngineImpl::SetObjectToken recovers which voice this is.
    wchar_t index_str[16];
    swprintf_s(index_str, L"%d", attr.token_index());
    set(L"TokenIndex", index_str);

    attributes_[L"Name"] = name;
    attributes_[L"Gender"] = attr.get_gender();
    attributes_[L"Age"] = attr.get_age();
    attributes_[L"Language"] = attr.get_language();
    attributes_[L"Vendor"] = L"MindMaker";
}

STDMETHODIMP voice_token::OpenKey(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey)
{
    if (!pszSubKeyName) return E_INVALIDARG;
    if (!ppSubKey) return E_POINTER;
    *ppSubKey = nullptr;

    try {
        if (!str_equal(pszSubKeyName, L"Attributes")) return SPERR_NOT_FOUND;

        com::object<ISpDataKeyImpl> obj;
        for (const auto& [key, value] : attributes_) {
            obj->set(key, value);
        }

        com::interface_ptr<ISpDataKey> int_ptr(obj);
        *ppSubKey = int_ptr.get();
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

STDMETHODIMP voice_token::EnumKeys(ULONG Index, LPWSTR* ppszSubKeyName)
{
    if (!ppszSubKeyName) return E_POINTER;
    *ppszSubKeyName = nullptr;
    if (Index > 0) return SPERR_NO_MORE_ITEMS;

    try {
        *ppszSubKeyName = com::strdup(L"Attributes");
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

}  // namespace sapi
}  // namespace FlexVoice
