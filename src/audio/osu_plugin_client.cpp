#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/audio/archive_member.h"
#include <ttp_osu/abi.h>
#include "osu_visual_query.h"
#include <new>

namespace ttplayer::audio {
namespace {
// Only archive I/O and ABI adaptation live here. The DLL owns all .osu grammar,
// path/reference resolution, deduplication and background-selection rules.
struct Plugin {
    HMODULE module{};
    TtpOsuApi api{};
    Plugin() {
        std::wstring exe(32768,L'\0');
        const DWORD n=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
        if(!n || n>=exe.size())throw ArchiveError(kOszPluginMissing);
        exe.resize(n);
        const auto path=std::filesystem::path(exe).parent_path()/L"AddIn"/L"ttp_osu.dll";
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module)throw ArchiveError(GetLastError()==ERROR_BAD_EXE_FORMAT?kOszPluginIncompatible:kOszPluginMissing);
        const auto query=reinterpret_cast<TtpOsuQuery>(GetProcAddress(module,"ttp_osu_query"));
        api.size=sizeof(api);
        if(!query || FAILED(query(TTP_OSU_ABI_VERSION,&api)) || api.size<sizeof(api) ||
            api.abi!=TTP_OSU_ABI_VERSION || !api.resolve || !api.cover) {
            FreeLibrary(module);module=nullptr;throw ArchiveError(kOszPluginIncompatible);
        }
    }
    ~Plugin(){if(module)FreeLibrary(module);}
};
const Plugin& LoadedPlugin() {static const Plugin plugin;return plugin;}
const TtpOsuApi& Api() {return LoadedPlugin().api;}
struct Capabilities {bool archive{},visual{},logo{},transitions{};};
const Capabilities& AvailableCapabilities() noexcept {
    static const Capabilities result=[]() noexcept {
        Capabilities value;
        try {
            const auto& plugin=LoadedPlugin();
            value.archive=true;
            TtpOsuVisualApi api{};
            value.visual=detail::QueryOsuVisualApi(plugin.module,api);
            value.logo=value.visual && api.logo;
            value.transitions=value.visual && api.transition && api.transition_state;
        } catch(...) {}
        return value;
    }();
    return result;
}
template<class F> HRESULT Callback(F&& body) noexcept {
    try {body();return S_OK;}
    catch(const ArchiveError& e){return e.Result();}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(...){return E_FAIL;}
}
struct Context {
    HMODULE module;
    const std::function<bool()>& canceled;
    const std::function<void(uint64_t,uint64_t)>& progress;
    std::vector<unsigned char> bytes;
    TtpOsuHost Host() {
        return {sizeof(TtpOsuHost),this,
            [](void* opaque,const wchar_t* archive,TtpOsuVisitName visitor,void* output)->HRESULT {
                auto& c=*static_cast<Context*>(opaque);
                return Callback([&]{
                    const auto names=ListArchiveMembers(archive,c.module,c.canceled,c.progress);
                    for(const auto& name:names) {
                        const auto hr=visitor(output,name.c_str());
                        if(FAILED(hr))throw ArchiveError(hr);
                    }
                });
            },
            [](void* opaque,const wchar_t* archive,const wchar_t* member,const unsigned char** bytes,uint32_t* size)->HRESULT {
                auto& c=*static_cast<Context*>(opaque);*bytes=nullptr;*size=0;
                return Callback([&]{c.bytes=ReadArchiveMember({archive,member,ArchiveKind::zip},c.module,c.canceled);
                    *bytes=c.bytes.data();*size=static_cast<uint32_t>(c.bytes.size());});
            },
            [](void* opaque)->BOOL {auto& c=*static_cast<Context*>(opaque);
                try{return c.canceled && c.canceled();}catch(...){return TRUE;}},
            [](void* opaque,uint64_t completed,uint64_t total)->HRESULT {
                auto& c=*static_cast<Context*>(opaque);
                return Callback([&]{if(c.progress)c.progress(completed,total);});
            }};
    }
};
void Check(HRESULT hr){if(FAILED(hr))throw ArchiveError(hr);}
}
bool HasOszSupport() noexcept {return AvailableCapabilities().archive;}
bool HasOsuVisualSupport() noexcept {return AvailableCapabilities().visual;}
bool HasOsuLogoSupport() noexcept {return AvailableCapabilities().logo;}
bool HasOsuTransitionSupport() noexcept {return AvailableCapabilities().transitions;}
bool IsOszArchive(const std::filesystem::path& path) {
    // Routing must survive a missing DLL: never import an OSZ as generic ZIP
    // or offer to rename it merely because the plugin is absent.
    return _wcsicmp(path.extension().c_str(),L".osz")==0;
}
std::vector<OszMedia> ResolveOszMedia(const std::filesystem::path& archive,
    HMODULE module,const std::function<bool()>& canceled,
    const std::function<void(uint64_t,uint64_t)>& progress,std::wstring_view required_audio) {
    if(canceled && canceled())throw ArchiveError(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    Context context{module,canceled,progress,{}};const auto host=context.Host();
    const std::wstring required(required_audio);
    std::vector<OszMedia> result;
    const auto hr=Api().resolve(&host,archive.c_str(),required.c_str(),
        [](void* opaque,const TtpOsuMedia* media)->HRESULT {
            return Callback([&]{static_cast<std::vector<OszMedia>*>(opaque)->push_back(
                {media->beatmap,media->audio,media->background});});
        },&result);
    Check(hr);return result;
}
std::vector<unsigned char> ReadOszCover(const std::filesystem::path& archive,
    HMODULE module,const std::function<bool()>& canceled,
    std::wstring_view required_audio) {
    const std::function<void(uint64_t,uint64_t)> progress;
    Context context{module,canceled,progress,{}};const auto host=context.Host();
    const std::wstring required(required_audio);
    std::vector<unsigned char> result;
    const auto hr=Api().cover(&host,archive.c_str(),required.c_str(),
        [](void* opaque,const unsigned char* bytes,uint32_t size)->HRESULT {
            return Callback([&]{static_cast<std::vector<unsigned char>*>(opaque)->assign(bytes,bytes+size);});
        },&result);
    Check(hr);return result;
}
} // namespace ttplayer::audio
