#include "ttplayer/app/worker_process.h"
#include "../ui/output_devices.h"

#include <windows.h>
#include <objbase.h>

#include <cwchar>

namespace {

int ProbeSelectedDevice(const wchar_t* request_path,
                        const wchar_t* output_path) {
    const auto request =
        ttplayer::ui::detail::ReadLegacyOutputDeviceProbe(request_path);
    if (!request || request->size() != 1) return 5;

    auto device = request->front();
    std::wstring diagnostic;
    if (!ttplayer::ui::detail::PopulateLegacyOutputDeviceDetails(
            device, &diagnostic)) {
        return 6;
    }
    return ttplayer::ui::detail::WriteLegacyOutputDeviceProbe(
               output_path, {device})
        ? 0 : 3;
}

} // namespace

int ttplayer::app::RunOutputDeviceWorker(int argc, wchar_t** argv) {
    const bool enumerate = argc == 2 && argv[1] && argv[1][0] != L'\0';
    const bool details = argc == 4 && argv[1] &&
                         std::wcscmp(argv[1], L"--details") == 0 &&
                         argv[2] && argv[2][0] != L'\0' &&
                         argv[3] && argv[3][0] != L'\0';
    if (!enumerate && !details) return 2;

    // FUN_004E207B creates ASIO's in-proc COM object.  The original player has
    // already initialized COM by the time the Options page reaches it; this
    // isolated executable must establish that apartment itself.
    const HRESULT initialized =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 4;

    int exit_code{};
    if (details) {
        // Isolated equivalent of FUN_0049989D's selected-row capability
        // query. The parent may terminate a stalled legacy driver without
        // losing the already-published catalogue.
        exit_code = ProbeSelectedDevice(argv[2], argv[3]);
    } else {
        // Catalogue discovery does not activate DirectSound or ASIO devices.
        // KS is the exception: original FUN_004F0CB0 validates its pins before
        // publishing the descriptor.
        const auto devices =
            ttplayer::ui::detail::EnumerateLegacyOutputDevices();
        exit_code = ttplayer::ui::detail::WriteLegacyOutputDeviceProbe(
                        argv[1], devices)
            ? 0 : 3;
    }
    CoUninitialize();
    return exit_code;
}
