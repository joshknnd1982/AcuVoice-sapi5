#pragma once

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <comdef.h>
#include <comip.h>

#include "com.hpp"
#include "voice_attributes.hpp"

#ifndef BUILD_X64
#include "av_engine.h"
#endif
#include "pipe_client.h"

namespace AcuVoice {
namespace sapi {

class __declspec(uuid("{6d1a8c47-2f30-4b6e-9a1c-7e5b0d3a4f21}")) ISpTTSEngineImpl :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId,
                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    ISpObjectTokenPtr token_;
    voice_attributes voice_;

#ifndef BUILD_X64
    // avcore is loaded the first time this voice speaks, not in the constructor: a SAPI
    // engine object is created whenever a host enumerates voices, and most of those
    // never say anything.
    [[nodiscard]] bool ensure_engine_loaded();
#endif
};

#ifdef BUILD_X64
void InitPipeClient();
void CleanupPipeClient();
void ShutdownPipeServer();
#endif

}  // namespace sapi
}  // namespace AcuVoice
