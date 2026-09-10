#pragma once

namespace ttplayer::app {
inline constexpr wchar_t kFileInfoWorkerSwitch[] = L"--ttplayer-file-info-worker";

// Runs only in a disposable child process, before player startup, DLL/resource
// validation, single-instance IPC, settings or any windows are initialized.
// argv[0] is the executable name (or the private switch in the GUI entry).
int RunFileInfoWorker(int count, wchar_t** arguments);
} // namespace ttplayer::app
