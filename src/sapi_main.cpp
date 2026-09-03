#include <new>
#include <sapi.h>

#include "com.hpp"
#include "registry.hpp"
#include "engine_client.h"
#include "flexvoice_log.h"
#include "ISpTTSEngineImpl.hpp"
#include "IEnumSpObjectTokensImpl.hpp"
#include "voice_registry.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
FlexVoice::com::class_object_factory g_cls_obj_factory;

const std::wstring token_enums_path = L"Software\\Microsoft\\Speech\\Voices\\TokenEnums";

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

// Releases up to 1.0.2 registered a dynamic token enumerator here as well as
// writing the static tokens. SAPI merges the two lists without noticing they
// describe the same nine voices -- the enumerator gives its tokens ids under
// TokenEnums\FlexVoice rather than under Tokens -- so every FlexVoice voice
// appeared twice in every voice list. The enumerator was never load-bearing:
// the static tokens are what every SAPI5 client reads.
//
// So this is now only ever removed, never written, and removing it is part of
// registering as well as of unregistering, which heals a machine upgrading
// from 1.0.2. One less machine-wide registration is also one less thing an
// uninstall can leave behind.
bool remove_token_enumerator() noexcept
{
    using namespace FlexVoice::registry;
    try {
        key enums_key(HKEY_LOCAL_MACHINE, token_enums_path, delete_access);
        return enums_key.delete_subtree(L"FlexVoice");
    }
    catch (...) {
        // TokenEnums itself may not exist, which is the state we want anyway.
        return true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);

#ifdef _WIN64
        FlexVoice::log::component() = L"sapi64";
#else
        FlexVoice::log::component() = L"sapi32";
#endif

        try {
            g_cls_obj_factory.register_class<FlexVoice::sapi::IEnumSpObjectTokensImpl>();
            g_cls_obj_factory.register_class<FlexVoice::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            return FALSE;
        }
        // Nothing else here on purpose: the pipe client is created lazily on
        // the first utterance, so loading the DLL never starts a process.
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    return g_cls_obj_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return FlexVoice::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        FlexVoice::com::class_registrar r(g_dll_handle);
        r.register_class<FlexVoice::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<FlexVoice::sapi::ISpTTSEngineImpl>();

        const std::wstring engine_clsid =
            clsid_to_string(__uuidof(FlexVoice::sapi::ISpTTSEngineImpl));

        // Sweep first, so a voice renamed or dropped since the version that is
        // being replaced cannot leave an orphaned token pointing at this
        // CLSID. Up to 1.0.2 this could not work at all -- see
        // remove_voice_tokens -- which is why an upgrade from 1.0.2 has real
        // work to do here.
        FlexVoice::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE);
        remove_token_enumerator();
        FlexVoice::sapi::write_voice_tokens(HKEY_LOCAL_MACHINE, engine_clsid);
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

// Every step here is independent and none of them throws. An uninstall gets
// one shot at this: whatever is left behind stays on the machine, and a voice
// token naming a CLSID whose DLL has just been deleted is not a FlexVoice
// problem, it is a problem for every SAPI5 client on the machine. So a failure
// in one step must never skip the others.
//
// The installer repeats all of this from its own uninstall code, in both
// registry views, and does not depend on regsvr32 having run at all.
STDAPI DllUnregisterServer()
{
    // Stop the host first so its files can be replaced by an upgrade.
    try { FlexVoice::EngineClient::shutdownServer(); } catch (...) {}

    bool ok = true;

    // Order matters only in that the tokens are the dangerous leftover: they
    // are what a SAPI5 client sees, and what a stale default-voice setting
    // points at.
    try { FlexVoice::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE); } catch (...) { ok = false; }
    try { ok = remove_token_enumerator() && ok; } catch (...) { ok = false; }

    ok = FlexVoice::com::class_registrar::unregister_class<
             FlexVoice::sapi::IEnumSpObjectTokensImpl>() && ok;
    ok = FlexVoice::com::class_registrar::unregister_class<
             FlexVoice::sapi::ISpTTSEngineImpl>() && ok;

    return ok ? S_OK : S_FALSE;
}
