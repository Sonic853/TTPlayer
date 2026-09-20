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
    bool Attach(HWND player,HWND playlist,HWND equalizer,HWND lyrics=nullptr);
    void Detach();
    const SkinPluginModule* Provider() const noexcept { return module_.get(); }
    HBITMAP Preview() const { return module_->Api().preview(instance_); }
    void Shade() { module_->Api().shade(instance_); }
    void Paint(HWND window,HDC dc) const { module_->Api().paint(instance_,window,dc); }
    bool Translate(const MSG& message) const { return module_->Api().translate(instance_,&message)!=FALSE; }
    bool Handles(HWND window) const { return window && module_->Api().handles && module_->Api().handles(instance_,window); }
    HMENU Menu(HWND window,uint32_t command=0) const { return module_->Api().menu?module_->Api().menu(instance_,window,command):nullptr; }
    bool ContentState(TtpSkinContent& state,bool apply=false) const {
        return module_->Api().content_state && module_->Api().content_state(instance_,&state,apply);
    }
    bool LyricColors(HWND window,TtpSkinLyricColors& colors) const noexcept {
        return module_->Api().lyric_colors && module_->Api().lyric_colors(instance_,window,&colors);
    }
    int32_t LyricFontHeight() const noexcept {
        return module_->Api().lyric_font_height?module_->Api().lyric_font_height(instance_):0;
    }
    bool Layout(TtpSkinLayout& state,bool restore) const {
        return module_->Api().layout && SUCCEEDED(module_->Api().layout(instance_,&state,restore));
    }
private:
    SkinPluginInstance(std::shared_ptr<SkinPluginModule> module,void* instance):module_(std::move(module)),instance_(instance) {}
    std::shared_ptr<SkinPluginModule> module_;
    void* instance_{};
};
}
