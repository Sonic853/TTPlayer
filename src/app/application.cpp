#include "ttplayer/app/application.h"

#include "ttplayer/app/runtime.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"
#include "ttplayer/ui/wtl_runtime.h"

#include <algorithm>
#include <cwctype>
#include <memory>
#include <shellapi.h>

namespace ttplayer::app {
namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
settings::Settings LoadSettingsIfPresent() {
    return settings::LoadRuntimeSettings(RuntimePath({}));
}
} // namespace

bool ParsedCommandLine::HasSwitch(std::wstring_view name) const {
    const auto expected = Lower(std::wstring(name));
    return std::find(switches.begin(), switches.end(), expected) != switches.end();
}

ULONG_PTR ParsedCommandLine::FileMode() const {
    return static_cast<ULONG_PTR>(ui::ResolveCommandLineFileMode(
        HasSwitch(L"a"), HasSwitch(L"e")));
}

ParsedCommandLine ParseCommandLine() {
    ParsedCommandLine parsed;
    int count{};
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &count), &LocalFree);
    for (int index = 1; arguments && index < count; ++index) {
        std::wstring argument(arguments.get()[index]);
        if (argument == L"--smoke-test") { parsed.smoke_test = true; continue; }
        if (!argument.empty() && (argument.front() == L'/' || argument.front() == L'-')) {
            argument.erase(argument.begin());
            parsed.switches.push_back(Lower(std::move(argument)));
        } else if (parsed.file.empty()) {
            parsed.file = std::move(argument);
        }
    }
    return parsed;
}

int TTPlayer_RunApplicationSession(HINSTANCE instance, int show_command,
                                   SingleInstanceIpc& single_instance,
                                   HMODULE resource_module,
                                   HMODULE ttpcomm_module,
    const plugins::PluginManager& sound_library) {
    if (FAILED(ui::EnsureWtlRuntime())) return -1;
    const auto command_line = ParseCommandLine();
    auto settings = LoadSettingsIfPresent();
    if (command_line.HasSwitch(L"reg") || command_line.HasSwitch(L"unreg")) {
        ui::PlayerWindow association(std::move(settings));
        association.SetSkinResourceModule(resource_module);
        association.SetTtpCommModule(ttpcomm_module);
        association.SetSoundLibrary(&sound_library);
        if (command_line.HasSwitch(L"reg")) {
            // 004C0900 constructs the reduced About + System Association
            // property sheet; it does not silently register every format.
            static_cast<void>(association.ShowRegistrationOptions(instance));
        } else {
            // FUN_0049CD41 removes registered reader/playlist patterns plus
            // the AudioCD and Directory shell verbs, then notifies Explorer.
            static_cast<void>(association.UnregisterAllAssociations());
        }
        return 0;
    }

    ui::PlayerWindow player(std::move(settings));
    player.SetSkinResourceModule(resource_module);
    player.SetTtpCommModule(ttpcomm_module);
    player.SetSoundLibrary(&sound_library);
    static_cast<void>(player.LoadStartupSkin(resource_module));
    if (!player.Create(instance, show_command)) return 0;

    // TTPlayer_RunApplicationSession trims the working set and routes the
    // parsed startup path through the newly created player before publishing
    // the HWND to secondary instances.  This prevents a second process from
    // interleaving WM_COPYDATA with the first process's own startup request.
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    if (!command_line.file.empty())
        player.OpenCommandLinePath(command_line.file, command_line.FileMode());
    else player.RestoreStartupPlayback();
    single_instance.PublishWindow(player.Handle());
    SetForegroundWindow(player.Handle());
    SetActiveWindow(player.Handle());
    if (!command_line.smoke_test) player.CheckStartupAssociations();
    if (command_line.smoke_test) PostMessageW(player.Handle(), WM_CLOSE, 0, 0);

    struct PlayerFilter final : WTL::CMessageFilter {
        ui::PlayerWindow& player;
        explicit PlayerFilter(ui::PlayerWindow& value) : player(value) {}
        BOOL PreTranslateMessage(MSG* message) override {
            return player.PreTranslateMessage(*message);
        }
    } filter(player);
    ui::PlayerMessageLoop loop;
    if (!_Module.AddMessageLoop(&loop)) return -1;
    loop.AddMessageFilter(&filter);
    const int result = loop.Run();
    loop.RemoveMessageFilter(&filter);
    _Module.RemoveMessageLoop();
    single_instance.PublishWindow(nullptr);
    return result;
}
} // namespace ttplayer::app
