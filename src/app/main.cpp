#include "ttplayer/app/application.h"
#include "ttplayer/app/file_info_worker.h"
#include "ttplayer/app/runtime.h"
#include "ttplayer/app/single_instance.h"
#include "ttplayer/app/worker_process.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/settings/settings.h"
#include "../ui/file_info_probe_client.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>
#include <windows.h>

namespace {
constexpr wchar_t kSingleInstanceName[] = L"TTPlayer";
constexpr DWORD kSingleInstanceMappingSize = 4096;

// Read only the language and initialize translations before any startup error
// can open a message box. Full settings and skin loading still happen later.
class OptionalI18nRuntime {
public:
    OptionalI18nRuntime() {
        const auto runtime = ttplayer::app::RuntimePath({});
        if (!runtime.empty()) module_ = LoadLibraryW((runtime / L"AddIn" / L"ttp_i18n.dll").c_str());
        if (module_) {
            ttplayer::i18n::Initialize(runtime, ttplayer::settings::LoadRuntimeLanguage(runtime));
        }
    }
    ~OptionalI18nRuntime() {
        ttplayer::i18n::Shutdown();
        if (module_) FreeLibrary(module_);
    }
    OptionalI18nRuntime(const OptionalI18nRuntime&) = delete;
    OptionalI18nRuntime& operator=(const OptionalI18nRuntime&) = delete;
private:
    HMODULE module_{};
};

int TTPlayer_wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    namespace i18n = ttplayer::i18n;
    // Load the optional i18n provider before actively loading ttpcomm. Missing
    // translations never prevent startup. The universal EXE retains a mandatory
    // ttpcomm startup import for static TLS, so Windows loads that dependency
    // before this entry point; do not replace it with LoadLibrary-only loading.
    const OptionalI18nRuntime i18n_runtime;

    // Retain EXE-local dependency paths and omit the original exact-version
    // gate. ABI-compatible revisions are accepted; consumers check exports.
    ttplayer::app::TtpCommRuntime ttpcomm;
    if (!ttpcomm.Initialize()) {
        MessageBoxW(nullptr, i18n::Literal(L"Error in ttpcomm.dll, please resetup this program!"),
                    i18n::Literal(L"TTPlayer"), MB_OK | MB_ICONERROR);
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
        MessageBoxW(nullptr, i18n::Literal(L"Load resource (ttpres.dll) failed, please resetup this program!"),
                    i18n::Literal(L"TTPlayer"), MB_OK | MB_ICONERROR);
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
        const auto title = i18n::ResourceText(resources.Module(), 0x80);
        resources.Shutdown();
        if (SUCCEEDED(ole_result)) OleUninitialize();
        if (tls_index != TLS_OUT_OF_INDEXES) TlsFree(tls_index);
        MessageBoxW(nullptr, i18n::Literal(L"Sound Library init failed, the player can not run!"),
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
    // Original AddIns use pre-ATL-8 heap window thunks. Keep DEP enabled,
    // but let Windows emulate those thunks, rather than declaring the whole
    // legacy plugin graph NX-compatible. Paired with /NXCOMPAT:NO in CMake;
    // applies before both normal startup and embedded plugin workers.
    // AlwaysOn/AlwaysOff or externally imposed process policy takes precedence.
    SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    // Private workers bypass DLL startup, single-instance IPC, settings and
    // all player windows. Dispatch even malformed requests here: a failed
    // probe must never fall through to launch another player instance.
    if (arguments && count >= 2) {
        int result{};
        bool worker = true;
        if (wcscmp(arguments[1], ttplayer::app::kFileInfoWorkerSwitch) == 0)
            result = ttplayer::app::RunFileInfoWorker(count - 1, arguments + 1);
        else if (wcscmp(arguments[1], ttplayer::app::kDspWorkerSwitch) == 0)
            result = ttplayer::app::RunDspWorker(count - 1, arguments + 1, true);
        else if (wcscmp(arguments[1], ttplayer::app::kOutputDeviceWorkerSwitch) == 0)
            result = ttplayer::app::RunOutputDeviceWorker(count - 1, arguments + 1);
        else
            worker = false;
        if (worker) {
            LocalFree(arguments);
            return result;
        }
    }
    if (arguments) LocalFree(arguments);
    ttplayer::ui::detail::EnableEmbeddedFileInfoProbe();
    return TTPlayer_wWinMain(instance, previous, command_line, show_command);
}
