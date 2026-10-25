#pragma once
#include <ttp_osu/visual.h>
#include <initializer_list>

namespace ttplayer::audio::detail {
// Negotiate each released ABI without reading a callback absent in that table.
inline bool QueryOsuVisualApi(HMODULE module,TtpOsuVisualApi& api) noexcept {
    const auto query=reinterpret_cast<TtpOsuQueryVisual>(GetProcAddress(module,TTP_OSU_VISUAL_ENTRY));
    if(!query)return false;
    for(const uint32_t version:{TTP_OSU_VISUAL_ABI,4U,3U,2U,1U}) {
        api={};api.size=sizeof(api);
        if(SUCCEEDED(query(version,&api)) && api.abi==version &&
           api.size>=TTP_OSU_VISUAL_ABI1_SIZE && api.create && api.destroy && api.configure &&
           api.timing && api.image && api.update && api.paint && api.present && api.detach) {
            if(api.abi<2 || api.size<TTP_OSU_VISUAL_ABI2_SIZE)api.logo=nullptr;
            if(api.abi<3 || api.size<TTP_OSU_VISUAL_ABI3_SIZE) {
                api.transition=nullptr;api.transition_state=nullptr;
            }
            if(api.abi<4 || api.size<TTP_OSU_VISUAL_ABI4_SIZE)api.style=nullptr;
            if(api.abi<5 || api.size<sizeof(api))api.intro_text=nullptr;
            return true;
        }
    }
    api={};return false;
}
}
