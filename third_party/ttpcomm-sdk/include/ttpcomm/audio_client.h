#pragma once
#include <windows.h>
#include <ttpcomm/audio_api.h>

namespace ttpcomm::host {
/* Caller owns the module reference and must keep it alive until every object
   and function pointer from this table is no longer in use. */
inline bool QueryAudio(HMODULE module, TtpCommAudioApi& api) noexcept {
    api={};
    if(!module)return false;
    const auto query=reinterpret_cast<TtpCommQueryRuntimeFn>(
        GetProcAddress(module,"ttpcomm_query_runtime"));
    api.size=sizeof(api);
    if(!query){api={};return false;}
    __try {
        if(!query(TTPCOMM_AUDIO_ABI,TTPCOMM_AUDIO_REQUIRED,
            reinterpret_cast<TtpCommRuntimeApi*>(&api)) ||
            api.size<sizeof(api) || api.abi_version!=TTPCOMM_AUDIO_ABI ||
            (api.capabilities&TTPCOMM_AUDIO_REQUIRED)!=TTPCOMM_AUDIO_REQUIRED ||
            !api.encoder_create || !api.encoder_destroy || !api.encoder_encode ||
            !api.encoder_reset || !api.effects_create || !api.effects_destroy ||
            !api.effects_info || !api.effects_configure || !api.effects_process || !api.effects_reset) {
            api={};return false;
        }
    }__except(EXCEPTION_EXECUTE_HANDLER) {
        api={};return false;
    }
    return true;
}
}
