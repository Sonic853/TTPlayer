#include "ttplayer/skin/skin_plugin.h"
#include <algorithm>
#include "ttplayer/skin/skin_paths.h"
#include <stdexcept>

namespace ttplayer::skin {
SkinPluginModule::SkinPluginModule(HMODULE module,const TtpSkinPlugin& api):module_(module),api_(api) {
    if(api.name) name_.assign(api.name,wcsnlen_s(api.name,128));
    if(name_.find_first_not_of(L" \t\r\n")==std::wstring::npos) name_=L"Plugin Skin";
    if(!api.skin_directory || !api.extensions) throw std::invalid_argument("Missing plugin skin namespace");
    directory_=std::wstring(api.skin_directory,wcsnlen_s(api.skin_directory,260));
    if(!IsRelativeSkinPath(directory_)) throw std::invalid_argument("Invalid plugin skin directory");
    directory_=directory_.lexically_normal();
    if(directory_==L"." || directory_.filename().empty() || _wcsicmp(directory_.c_str(),L"new")==0)
        throw std::invalid_argument("Reserved plugin skin directory");
    const std::wstring extensions(api.extensions,wcsnlen_s(api.extensions,1024));
    size_t begin=0;
    while(begin<extensions.size()) {
        const auto end=extensions.find(L';',begin);
        auto suffix=extensions.substr(begin,end==std::wstring::npos?end:end-begin);
        if(suffix.size()<2 || suffix[0]!=L'.' || suffix.find_first_not_of(L".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-",1)!=std::wstring::npos ||
           suffix.find(L'.',1)!=std::wstring::npos || IsNativeSkinPackage(std::filesystem::path(L"skin"+suffix)))
            throw std::invalid_argument("Invalid plugin skin suffix");
        extensions_.push_back(std::move(suffix));
        if(end==std::wstring::npos) break;
        begin=end+1;
    }
    if(extensions_.empty()) throw std::invalid_argument("No plugin skin suffixes");
    if(api.default_package && *api.default_package) {
        default_package_=std::wstring(api.default_package,wcsnlen_s(api.default_package,260));
        if(!IsRelativeSkinPath(default_package_) || default_package_.has_parent_path() ||
           !Supports(default_package_)) throw std::invalid_argument("Invalid built-in package name");
    }
    if(api.skin_download_url) {
        const auto length=wcsnlen_s(api.skin_download_url,2048);
        if(length<2048 && ((_wcsnicmp(api.skin_download_url,L"https://",8)==0 && length>8) ||
                          (_wcsnicmp(api.skin_download_url,L"http://",7)==0 && length>7)))
            download_url_.assign(api.skin_download_url,length);
    }
}
bool SkinPluginModule::IsDefaultPackage(const std::filesystem::path& path) const {
    return !default_package_.empty() && _wcsicmp(path.filename().c_str(),default_package_.c_str())==0;
}
bool SkinPluginModule::Supports(const std::filesystem::path& path) const {
    const auto suffix=path.extension().wstring();
    return std::any_of(extensions_.begin(),extensions_.end(),[&](const auto& value) {return _wcsicmp(suffix.c_str(),value.c_str())==0;});
}
bool SkinPluginModule::OwnsInstalledPackage(const std::filesystem::path& root,const std::filesystem::path& path) const {
    return Supports(path) && _wcsicmp(path.parent_path().lexically_normal().c_str(),Directory(root).lexically_normal().c_str())==0;
}
SkinPluginModule::~SkinPluginModule() { if(module_) FreeLibrary(module_); }
std::vector<std::shared_ptr<SkinPluginModule>> SkinPluginModule::Discover(const std::filesystem::path& directory) {
    std::vector<std::shared_ptr<SkinPluginModule>> result;
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for(std::filesystem::directory_iterator it(directory,error),end;!error && it!=end;it.increment(error)) {
        const auto& path=it->path();
        if(it->is_regular_file(error) && !error && _wcsicmp(path.extension().c_str(),L".dll")==0 &&
           _wcsnicmp(path.filename().c_str(),L"ttp_",4)==0) paths.push_back(std::filesystem::absolute(path));
    }
    std::sort(paths.begin(),paths.end());
    for(const auto& path:paths) {
        HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module) continue;
        const auto getter=reinterpret_cast<TtpGetSkinPlugin>(GetProcAddress(module,TTP_SKIN_ENTRY));
        TtpSkinPlugin api{};api.size=sizeof(api);
        if(!getter || FAILED(getter(TTP_SKIN_ABI,&api)) || api.version!=TTP_SKIN_ABI || api.size<TTP_SKIN_PLUGIN_DECLARATION_SIZE ||
           !api.probe || !api.create || !api.attach || !api.detach || !api.destroy || !api.preview || !api.shade || !api.paint || !api.translate) {
            FreeLibrary(module);continue;
        }
        if(api.size<offsetof(TtpSkinPlugin,handles)) api.layout=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,menu)) api.handles=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,content_state)) api.menu=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,lyric_colors)) api.content_state=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,lyric_font_height)) api.lyric_colors=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,playlist_drop)) api.lyric_font_height=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,content_minimum)) api.playlist_drop=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,default_package)) api.content_minimum=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,skin_download_url)) api.default_package=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,check)) api.skin_download_url=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,lyric_font)) api.check=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,volume_tracking)) api.lyric_font=nullptr;
        if(api.size<offsetof(TtpSkinPlugin,playlist_reveal)) api.volume_tracking=nullptr;
        if(api.size<sizeof(api)) api.playlist_reveal=nullptr;
        std::unique_ptr<SkinPluginModule> provider;
        try {provider.reset(new SkinPluginModule(module,api));}
        catch(const std::invalid_argument&) {FreeLibrary(module);continue;}
        catch(...) {FreeLibrary(module);throw;}
        result.emplace_back(std::move(provider));
    }
    return result;
}
bool SkinPluginModule::Probe(const std::filesystem::path& path,TtpSkinInfo& info) const {
    info={};info.size=sizeof(info);return SUCCEEDED(api_.probe(path.c_str(),&info));
}
std::wstring SkinPluginModule::Diagnostic(const std::filesystem::path& path) const {
    wchar_t message[2048]{};
    if(api_.check) static_cast<void>(api_.check(path.c_str(),message,static_cast<uint32_t>(std::size(message))));
    message[std::size(message)-1]=0;return message;
}
std::unique_ptr<SkinPluginInstance> SkinPluginInstance::Create(std::shared_ptr<SkinPluginModule> module,
    const std::filesystem::path& path,const TtpSkinHost* host) {
    void* instance{};
    if(FAILED(module->Api().create(path.c_str(),host,&instance)) || !instance) return {};
    try {return std::unique_ptr<SkinPluginInstance>(new SkinPluginInstance(module,instance));}
    catch(...) {module->Api().destroy(instance);throw;}
}
SkinPluginInstance::~SkinPluginInstance() { module_->Api().detach(instance_);module_->Api().destroy(instance_); }
bool SkinPluginInstance::Attach(HWND player,HWND playlist,HWND equalizer,HWND lyrics) {
    const TtpSkinWindows windows{sizeof(TtpSkinWindows),player,playlist,equalizer,lyrics};
    return SUCCEEDED(module_->Api().attach(instance_,&windows));
}
void SkinPluginInstance::Detach() { module_->Api().detach(instance_); }
}
