#include <new>
#include <sapi.h>
#include "com.hpp"
#include "registry.hpp"
#include "voice_attributes.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "IEnumSpObjectTokensImpl.hpp"
#include "voice_registry.hpp"
#include "debug_log.h"

#ifdef BUILD_X64
#include "pipe_client.h"
#endif

namespace {

HINSTANCE g_dll_handle = nullptr;
AcuVoice::com::class_object_factory g_cls_obj_factory;

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

// Which registry view these land in is decided by the architecture of the dll doing the
// registering: the 32-bit build writes under WOW6432Node, where 32-bit SAPI looks, and
// the 64-bit build writes to the native view, where 64-bit hosts look. Both are
// installed and both register, so every host sees the full set.
void register_voice_tokens()
{
    AcuVoice::sapi::write_voice_tokens(
        HKEY_LOCAL_MACHINE,
        clsid_to_string(__uuidof(AcuVoice::sapi::ISpTTSEngineImpl)));
}

void unregister_voice_tokens() noexcept
{
    AcuVoice::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE);
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);

#ifdef BUILD_X64
        // avcore.dll is 32-bit, so a 64-bit host reaches the engine through the worker.
        AcuVoice::sapi::InitPipeClient();
#endif

        try {
            g_cls_obj_factory.register_class<AcuVoice::sapi::IEnumSpObjectTokensImpl>();
            g_cls_obj_factory.register_class<AcuVoice::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            return FALSE;
        }
    }
#ifdef BUILD_X64
    else if (dwReason == DLL_PROCESS_DETACH) {
        AcuVoice::sapi::CleanupPipeClient();
    }
#endif
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    return g_cls_obj_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return AcuVoice::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        AcuVoice::com::class_registrar r(g_dll_handle);
        r.register_class<AcuVoice::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<AcuVoice::sapi::ISpTTSEngineImpl>();
        register_voice_tokens();
        DEBUG_LOG("DllRegisterServer: %d AcuVoice voices registered",
                  AcuVoice::sapi::total_token_count());
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer()
{
    try {
#ifdef BUILD_X64
        // The worker outlives any one client, so an uninstall has to ask it to go --
        // otherwise its copy of avcore.dll keeps the install directory locked.
        AcuVoice::sapi::ShutdownPipeServer();
#endif
        unregister_voice_tokens();
        AcuVoice::com::class_registrar r(g_dll_handle);
        r.unregister_class<AcuVoice::sapi::IEnumSpObjectTokensImpl>();
        r.unregister_class<AcuVoice::sapi::ISpTTSEngineImpl>();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}
