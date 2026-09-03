#include <new>
#include <sapi.h>

#include "com.hpp"
#include "fv2_client.h"
#include "fv2_log.h"
#include "ISpTTSEngineImpl.hpp"
#include "registry.hpp"
#include "voice_registry.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
fv2::com::class_object_factory g_cls_obj_factory;

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);

#ifdef _WIN64
        fv2::log::component() = L"sapi64";
#else
        fv2::log::component() = L"sapi32";
#endif

        try {
            g_cls_obj_factory.register_class<fv2::sapi::ISpTTSEngineImpl>();
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
    return fv2::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        fv2::com::class_registrar r(g_dll_handle);
        r.register_class<fv2::sapi::ISpTTSEngineImpl>();

        const std::wstring engine_clsid =
            clsid_to_string(__uuidof(fv2::sapi::ISpTTSEngineImpl));

        // Sweep first, so a voice renamed or dropped since the version being
        // replaced cannot leave an orphaned token pointing at this CLSID.
        fv2::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE);
        fv2::sapi::write_voice_tokens(HKEY_LOCAL_MACHINE, engine_clsid);
        return S_OK;
    }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_UNEXPECTED; }
}

// Every step here is independent and none of them throws. An uninstall gets one
// shot at this: whatever is left behind stays on the machine, and a voice token
// naming a CLSID whose DLL has just been deleted is not our problem alone, it
// is a problem for every SAPI5 client on the machine -- a screen reader whose
// remembered voice is one of ours would fail to start SAPI5 at all. So a
// failure in one step must never skip the others.
//
// The installer repeats all of this from its own uninstall code, in both
// registry views, and does not depend on regsvr32 having run.
STDAPI DllUnregisterServer()
{
    // Stop the host first so its files can be replaced by an upgrade.
    try { fv2::EngineClient::shutdownServer(); } catch (...) {}

    bool ok = true;

    // The tokens are the dangerous leftover: they are what a SAPI5 client sees,
    // and what a stale default-voice setting points at.
    try { fv2::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE); } catch (...) { ok = false; }
    try { fv2::sapi::clear_default_voice_if_ours(HKEY_LOCAL_MACHINE); } catch (...) { ok = false; }
    try { fv2::sapi::clear_default_voice_if_ours(HKEY_CURRENT_USER); } catch (...) { ok = false; }

    ok = fv2::com::class_registrar::unregister_class<
             fv2::sapi::ISpTTSEngineImpl>() && ok;

    return ok ? S_OK : S_FALSE;
}
