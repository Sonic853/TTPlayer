#pragma once
#include <ttp_osu/visual.h>
#include <initializer_list>

namespace ttplayer::audio::detail {
// Accept an older visual DLL without reading its absent ABI 2 tail.
inline bool QueryOsuVisualApi(HMODULE module,TtpOsuVisualApi& api) noexcept {
    const auto query=reinterpret_cast<TtpOsuQueryVisual>(GetProcAddress(module,TTP_OSU_VISUAL_ENTRY));
    if(!query)return false;
    for(const uint32_t version:{TTP_OSU_VISUAL_ABI,1U}) {
        api={};api.size=sizeof(api);
        if(SUCCEEDED(query(version,&api)) && api.abi==version &&
           api.size>=TTP_OSU_VISUAL_ABI1_SIZE && api.create && api.destroy && api.configure &&
           api.timing && api.image && api.update && api.paint && api.present && api.detach) {
            if(api.abi<2 || api.size<sizeof(api))api.logo=nullptr;
            return true;
        }
    }
    api={};return false;
}
}
