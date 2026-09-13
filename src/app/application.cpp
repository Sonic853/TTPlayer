#include "ttplayer/app/application.h"

#include "ttplayer/app/runtime.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/ui/player_window.h"
#include "ttplayer/ui/player_runtime_policy.h"

#include <algorithm>
#include <cwctype>
#include <memory>
#include <shellapi.h>

namespace ttplayer::app {
namespace {
// WinUser.h does not expose this user-mode-internal timer message in every
// supported SDK. CMessageLoop_ShouldResumeIdle (004B54F2) compares the
// literal 0x0118 alongside WM_TIMER.
constexpr UINT kWmSysTimer = 0x0118;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
settings::Settings LoadSettingsIfPresent() {
    return settings::LoadRuntimeSettings(RuntimePath({}));
}
std::filesystem::path ResolveSkin(const settings::Settings& settings) {
    return skin::ResolveSkinPackagePath(RuntimePath(L"Skin"), settings.skin_file);
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

    const bool default_skin = settings.skin_file.empty() ||
        _wcsicmp(settings.skin_file.c_str(), L"<Default_Skin>") == 0;
    const auto skin_path = ResolveSkin(settings);
    // FUN_0045D5FA selects the package-specific geometry snapshot before
    // constructing the live skin windows. TTPlayerRebuild.xml supplies the startup
    // visual globals; see the deliberately restored value below.
    std::filesystem::path skin_profile = default_skin
        ? RuntimePath(L"Skin/Default.xml") : skin_path;
    if (!default_skin && !skin_profile.empty()) skin_profile += L".xml";
    std::error_code profile_error;
    if (!skin_profile.empty() &&
        std::filesystem::exists(skin_profile, profile_error) && !profile_error) {
        const auto global_visual = settings.visual;
        static_cast<void>(settings::LoadSkinVisualProfile(
            skin_profile, settings.player, settings.playlist,
            settings.lyric, settings.visual));
        // The startup restore path materialises the saved skin geometry but
        // leaves the root TTPlayerRebuild.xml visual globals active. Package and
        // sidecar visual overrides are applied only by the live skin-change
        // transaction (FUN_0045D5FA/FUN_0045DDEE).  This distinction is
        // directly observable with LX-iPlay: a restored startup uses the
        // root #194d5c spectrum, whereas selecting the skin at runtime uses
        // its grey Visual.xml palette.
        settings.visual = global_visual;
    }
    ui::PlayerWindow player(std::move(settings));
    player.SetSkinResourceModule(resource_module);
    player.SetTtpCommModule(ttpcomm_module);
    player.SetSoundLibrary(&sound_library);
    bool skin_loaded{};
    if (default_skin)
        skin_loaded = player.LoadSkinResource(resource_module, L"<Default_Skin>");
    else if (!skin_path.empty())
        skin_loaded = player.LoadSkinPackage(skin_path);
    // FUN_0045FAD8/FUN_0045D5FA always leave CPlayerWnd with the embedded
    // package when the configured external package cannot be opened.  Do not
    // expose the reconstruction-only 860x540 fallback merely because a saved
    // skin was renamed or damaged.
    if (!skin_loaded && !default_skin)
        skin_loaded = player.LoadSkinResource(resource_module, L"<Default_Skin>");
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

    MSG message{};
    bool run_idle = true;
    while (true) {
        // CMessageLoop_Run (004B5470) performs one WTL idle phase whenever
        // the queue becomes empty.  The original PlayerWindow idle handler
        // updates command-bar enable/check caches; this rebuild computes the
        // same state synchronously when each owner-drawn menu is opened, so
        // there is no private cache to mutate here.  Preserve the scheduling
        // boundary nevertheless: it is what prevents paint/mouse/timer
        // traffic from continuously restarting idle work.
        while (!PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE) &&
               run_idle) {
            run_idle = false;
        }

        BOOL result{};
        // The decompiled loop retries GetMessage after -1 rather than
        // interpreting the stale MSG or terminating the player session.
        do {
            result = GetMessageW(&message, nullptr, 0, 0);
        } while (result == -1);
        if (result == 0) break;
        // CPlayerWnd::PreTranslateMessage (00449DA8) first gives the active
        // lyric editor/dialog its private accelerator path, then relays the
        // untouched queued MSG to the tooltip filter.  Consumed accelerator
        // messages must not reach TranslateMessage/DispatchMessage again.
        if (!player.PreTranslateMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // CMessageLoop_ShouldResumeIdle (004B54F2) excludes exactly these
        // high-frequency messages.  Keep the literal message set rather than
        // replacing it with a broad range test: WM_NCMOUSEMOVE is the only
        // non-client mouse message suppressed by the original.
        if (message.message != WM_PAINT &&
            message.message != WM_NCMOUSEMOVE &&
            message.message != WM_TIMER &&
            message.message != kWmSysTimer &&
            message.message != WM_MOUSEMOVE) {
            run_idle = true;
        }
    }
    single_instance.PublishWindow(nullptr);
    return static_cast<int>(message.wParam);
}
} // namespace ttplayer::app
