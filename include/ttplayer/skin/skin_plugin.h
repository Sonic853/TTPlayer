#pragma once
#include "ttplayer/skin/skin_plugin_api.h"
#include <filesystem>
#include <memory>
#include <vector>

namespace ttplayer::skin {
class SkinPluginModule {
public:
    ~SkinPluginModule();
    SkinPluginModule(const SkinPluginModule&) = delete;
    SkinPluginModule& operator=(const SkinPluginModule&) = delete;
    static std::vector<std::shared_ptr<SkinPluginModule>> Discover(const std::filesystem::path& directory);
    bool Probe(const std::filesystem::path& path,TtpSkinInfo& info) const;
    const TtpSkinPlugin& Api() const noexcept { return api_; }
    const std::wstring& Name() const noexcept { return name_; }
    std::filesystem::path Directory(const std::filesystem::path& skin_root) const { return skin_root/directory_; }
    bool Supports(const std::filesystem::path& path) const;
    bool OwnsInstalledPackage(const std::filesystem::path& skin_root,const std::filesystem::path& path) const;
private:
    SkinPluginModule(HMODULE module,const TtpSkinPlugin& api);
    HMODULE module_{};
    TtpSkinPlugin api_{};
    std::wstring name_;
    std::filesystem::path directory_;
    std::vector<std::wstring> extensions_;
};
class SkinPluginInstance {
public:
    static std::unique_ptr<SkinPluginInstance> Create(std::shared_ptr<SkinPluginModule> module,
        const std::filesystem::path& path,const TtpSkinHost* host);
    ~SkinPluginInstance();
    bool Attach(HWND player,HWND playlist,HWND equalizer);
    void Detach();
    const SkinPluginModule* Provider() const noexcept { return module_.get(); }
    HBITMAP Preview() const { return module_->Api().preview(instance_); }
    void Shade() { module_->Api().shade(instance_); }
    void Paint(HWND window,HDC dc) const { module_->Api().paint(instance_,window,dc); }
    bool Translate(const MSG& message) const { return module_->Api().translate(instance_,&message)!=FALSE; }
private:
    SkinPluginInstance(std::shared_ptr<SkinPluginModule> module,void* instance):module_(std::move(module)),instance_(instance) {}
    std::shared_ptr<SkinPluginModule> module_;
    void* instance_{};
};
}
