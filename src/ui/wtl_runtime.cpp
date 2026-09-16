#include "ttplayer/ui/wtl_runtime.h"

WTL::CAppModule _Module;

namespace ttplayer::ui {
HRESULT EnsureWtlRuntime() {
    struct Runtime {
        HRESULT result = _Module.Init(nullptr, GetModuleHandleW(nullptr));
        ~Runtime() { if (SUCCEEDED(result)) _Module.Term(); }
    };
    static Runtime runtime;
    return runtime.result;
}
}
