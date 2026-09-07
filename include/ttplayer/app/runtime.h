#pragma once

#include "ttpcomm_api.h"
#include "ttplayer/plugins/plugin_manager.h"

#include <filesystem>
#include <windows.h>

namespace ttplayer::app {
std::filesystem::path RuntimePath(const std::filesystem::path& relative);
std::filesystem::path FindRuntimePath(const std::filesystem::path& relative);

class TtpCommRuntime {
public:
    ~TtpCommRuntime();
    bool Initialize();
    void Shutdown();
    [[nodiscard]] TtpCommApi& Api() noexcept { return api_; }
private:
    TtpCommApi api_{};
};

class ResourceRuntime {
public:
    ~ResourceRuntime();
    bool Initialize();
    void Shutdown();
    [[nodiscard]] HMODULE Module() const noexcept { return module_; }
private:
    HMODULE module_{};
};

class SoundLibraryRuntime {
public:
    ~SoundLibraryRuntime();
    HRESULT Initialize(const std::filesystem::path& addin_directory);
    void Shutdown();
    [[nodiscard]] size_t DiscoveredPluginCount() const noexcept {
        return manager_.RegisteredPluginCount();
    }
    [[nodiscard]] const std::vector<plugins::ReaderFormat>& ReaderFormats() const noexcept {
        return manager_.ReaderFormats();
    }
    [[nodiscard]] const std::vector<plugins::PluginInfo>& Plugins() const noexcept {
        return manager_.Plugins();
    }
    [[nodiscard]] const plugins::PluginManager& Manager() const noexcept {
        return manager_;
    }
private:
    plugins::PluginManager manager_;
};

class ThreadMessageHooksRuntime {
public:
    ~ThreadMessageHooksRuntime();
    bool Install();
    void Shutdown();
private:
    static LRESULT CALLBACK CallWndProcHook(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK CbtHook(int code, WPARAM wparam, LPARAM lparam);
    HHOOK call_window_hook_{};
    HHOOK cbt_hook_{};
};
} // namespace ttplayer::app
