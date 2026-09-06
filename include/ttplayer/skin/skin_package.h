#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace ttplayer::skin {
struct SkinEntry {
    std::string name;
    uint32_t compressed_size{};
    uint32_t size{};
    uint32_t local_header_offset{};
    uint16_t compression{};
};
class SkinPackage {
public:
    static SkinPackage Open(const std::filesystem::path& path);
    static SkinPackage OpenResource(HMODULE module, const wchar_t* name,
                                    const wchar_t* type = L"ZIP");
    void ExtractTo(const std::filesystem::path& directory) const;
    // The player already owns the original ttpcomm inflater.  Runtime skin
    // loads use it directly so package extraction is deterministic and never
    // blocks the UI in Shell.Application/Compressed Folders.
    void ExtractTo(const std::filesystem::path& directory,
                   HMODULE ttpcomm) const;
    [[nodiscard]] const std::vector<SkinEntry>& Entries() const noexcept { return entries_; }
    [[nodiscard]] bool Has(std::string_view name) const;
    [[nodiscard]] std::vector<unsigned char> ReadEntry(
        std::string_view name, HMODULE ttpcomm) const;
    [[nodiscard]] bool IsLegacyCompatible() const;
    [[nodiscard]] size_t ByteSize() const noexcept { return data_.size(); }
    [[nodiscard]] uint64_t Fingerprint() const noexcept;
private:
    static SkinPackage Parse(std::vector<unsigned char> data,
                             std::filesystem::path path = {});
    std::filesystem::path path_;
    std::vector<unsigned char> data_;
    std::vector<SkinEntry> entries_;
};
}
