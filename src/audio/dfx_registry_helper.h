#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::audio::detail {
// A broker for the audited, packed x86 settings helper. Its API stubs exchange
// arguments through debug events; the player remains the sole registry writer.
class DfxRegistryHelper {
public:
    using Call = std::function<LSTATUS(std::string_view, std::array<ULONG_PTR, 12>&)>;
    DfxRegistryHelper(const std::filesystem::path& executable, Call call);
    ~DfxRegistryHelper();
    DfxRegistryHelper(const DfxRegistryHelper&) = delete;
    bool Running() const;
    std::vector<std::wstring> Diagnostics();
    std::vector<std::wstring> Trace();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
