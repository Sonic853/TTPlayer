#pragma once
#include "ttplayer/audio/cue_sheet.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <map>
#include <span>

namespace ttplayer::audio {
struct DiscTrack {
    unsigned number{};
    unsigned start{}; // Absolute MSF frames, including the 150-frame lead-in.
    unsigned end{};
    bool data{};
    bool operator==(const DiscTrack&) const = default;
};
struct DiscLayout {
    std::vector<DiscTrack> tracks;
    DWORD serial{};
    bool complete{true};
    bool operator==(const DiscLayout&) const = default;
    [[nodiscard]] std::string MusicBrainzId() const;
    [[nodiscard]] std::wstring TocParameter() const;
    [[nodiscard]] std::wstring Fingerprint() const;
    [[nodiscard]] bool IsAudioDisc() const noexcept;
};
// Throws for truncated TOCs, bad MSF, duplicate/missing tracks and bad lead-out.
DiscLayout ParseDiscToc(const CDROM_TOC& toc, DWORD returned);
DiscLayout ReadDiscLayout(const std::filesystem::path& path);
HANDLE OpenCdDrive(const std::filesystem::path& path);
unsigned CdaTrackNumber(const std::filesystem::path& path);
const DiscTrack& FindDiscTrack(const DiscLayout&, unsigned number);
struct CdaDescriptor { unsigned number{}, start{}, frames{}; DWORD serial{}; };
CdaDescriptor ReadCdaDescriptor(const std::filesystem::path& path);
using DiscMetadata = std::map<unsigned, CueMetadata>;
std::filesystem::path DiscCacheDirectory();
// directory override is for isolated local tests. Existing v1/v2/v3 caches work.
DiscMetadata ReadDiscMetadata(const DiscLayout&, const std::filesystem::path& directory = {});
void WriteDiscMetadata(const DiscLayout&, const DiscMetadata& changes,
                       const std::filesystem::path& directory = {});
bool IsDiscMetadataWritable(const DiscLayout&) noexcept;
bool IsDiscMetadataField(std::wstring_view name) noexcept;
std::wstring DiscField(const CueMetadata&, const wchar_t* key);
// Validate whole-sector completion before advancing the device cursor.
unsigned CheckedCdRead(unsigned requested, DWORD returned);
bool ContainsDtsCd(std::span<const std::byte> bytes) noexcept;
} // namespace ttplayer::audio
