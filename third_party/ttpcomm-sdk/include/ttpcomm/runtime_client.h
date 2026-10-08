#pragma once
#include <windows.h>
#include <ttpcomm/runtime_api.h>

namespace ttpcomm::host {
// Thread-safe lazy initialization from the EXE directory, for player workers
// and standalone probes too. A successful module reference lives until process
// exit so stored C function pointers/opaque objects cannot outlive their DLL.
const TtpCommRuntimeApi* Runtime() noexcept;
bool QueryRuntime(HMODULE module, TtpCommRuntimeApi& api) noexcept;
}
