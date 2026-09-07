#include "ttplayer/app/application.h"
#include "ttplayer/app/runtime.h"
#include "ttplayer/app/single_instance.h"

#include <commctrl.h>
#include <objbase.h>
#include <string>
#include <windows.h>

namespace {
constexpr wchar_t kSingleInstanceName[] = L"TTPlayer";
constexpr DWORD kSingleInstanceMappingSize = 4096;

std::wstring ResourceText(HMODULE module, UINT identifier) {
    const wchar_t* value{};
    const int length = module ? LoadStringW(module, identifier,
        reinterpret_cast<LPWSTR>(&value), 0) : 0;
    return length > 0 && value ? std::wstring(value, static_cast<size_t>(length))
                               : std::wstring{};
}

int TTPlayer_wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    // Keep 004C0E8F's EXE-local dependency loading order, but deliberately
    // omit its exact-version gate. ABI-compatible DLL revisions (including
    // those without a version export) are accepted; consumers check exports.
    ttplayer::app::TtpCommRuntime ttpcomm;
    if (!ttpcomm.Initialize()) {
        MessageBoxA(nullptr, "Error in ttpcomm.dll, please resetup this program!",
                    "TTPlayer", MB_OK | MB_ICONERROR);
        return -1;
    }

    // CSingleInstanceIpc_Initialize is a static-runtime predecessor of the
    // recovered wWinMain. Its observable branch is reproduced here.
    const auto command_line = ttplayer::app::ParseCommandLine();
    std::wstring instance_name = kSingleInstanceName;
    if (command_line.smoke_test)
        instance_name += L".Smoke." + std::to_wstring(GetCurrentProcessId());
    ttplayer::app::SingleInstanceIpc single_instance(
        instance_name, kSingleInstanceMappingSize);
    if (!single_instance.Valid()) return 1;
    if (!single_instance.IsPrimary()) {
        single_instance.ForwardCommandLine(command_line.file.native(),
                                           command_line.FileMode());
        single_instance.Shutdown();
        return 1;
    }
    single_instance.PublishProcess();

    // Main-instance initialization order from 004C0E8F.
    const DWORD tls_index = TlsAlloc();
    static constexpr char kMainThreadName[] = "wWinMain";
    if (tls_index != TLS_OUT_OF_INDEXES)
        TlsSetValue(tls_index, const_cast<char*>(kMainThreadName));
    const HRESULT ole_result = OleInitialize(nullptr);
    // Dialogs 250 and 260 in ttpres.dll use SysDateTimePick32 and
    // ComboBoxEx32 respectively.  Register them explicitly rather than
    // depending on another component having initialized those classes.
    INITCOMMONCONTROLSEX controls{
        sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
                              ICC_DATE_CLASSES | ICC_USEREX_CLASSES};
    InitCommonControlsEx(&controls);

    ttplayer::app::ResourceRuntime resources;
    if (!resources.Initialize()) {
        if (SUCCEEDED(ole_result)) OleUninitialize();
        if (tls_index != TLS_OUT_OF_INDEXES) TlsFree(tls_index);
        MessageBoxA(nullptr, "Load resource (ttpres.dll) failed, please resetup this program!",
                    "TTPlayer", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (ttpcomm.Api().srand48) ttpcomm.Api().srand48(GetTickCount());
    ttplayer::app::SoundLibraryRuntime sound_library;
    const auto addin_path = ttplayer::app::FindRuntimePath(L"AddIn");
    const HRESULT sound_result = sound_library.Initialize(addin_path);
    if (FAILED(sound_result)) {
        // DAT_005474FC remains a live application-title string after
        // CPlayerApp_ShutdownResources in 004C0E8F.  Preserve its resource
        // value before unloading ttpres.dll.
        const auto title = ResourceText(resources.Module(), 0x80);
        resources.Shutdown();
        if (SUCCEEDED(ole_result)) OleUninitialize();
        if (tls_index != TLS_OUT_OF_INDEXES) TlsFree(tls_index);
        MessageBoxW(nullptr, L"Sound Library init failed, the player can not run!",
                    title.c_str(), MB_OK | MB_ICONERROR);
        return static_cast<int>(sound_result);
    }

    // 004B58AD (WH_CALLWNDPROC + WH_CBT) precedes CoolSB ordinal 200.
    ttplayer::app::ThreadMessageHooksRuntime message_hooks;
    message_hooks.Install();
    if (ttpcomm.Api().coolsb_init_app) ttpcomm.Api().coolsb_init_app();
    const int result = ttplayer::app::TTPlayer_RunApplicationSession(
        instance, show_command, single_instance, resources.Module(),
        ttpcomm.Api().module,
        sound_library.Manager());

    // Exact reverse-order teardown from 004C0E8F.
    single_instance.Shutdown();
    if (ttpcomm.Api().coolsb_uninit_app) ttpcomm.Api().coolsb_uninit_app();
    message_hooks.Shutdown();
    sound_library.Shutdown();
    resources.Shutdown();
    if (SUCCEEDED(ole_result)) OleUninitialize();
    if (tls_index != TLS_OUT_OF_INDEXES) { TlsSetValue(tls_index, nullptr); TlsFree(tls_index); }
    ttpcomm.Shutdown();
    return result;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    return TTPlayer_wWinMain(instance, previous, command_line, show_command);
}
