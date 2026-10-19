#pragma once

#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/builtin_file_info.h"

#include <memory>
#include <optional>

namespace ttplayer::audio {

class ArchiveInfoVerificationState;
using ArchiveInfoVerification = std::shared_ptr<const ArchiveInfoVerificationState>;

// Keep this token for the duration of the foreground WAVE metadata/CRC job.
// Its destruction also clears an abandoned job; Stop must never leave playlist
// reads indefinitely waiting for a verification which no longer has an owner.
[[nodiscard]] ArchiveInfoVerification BeginArchiveInfoVerification(
    const std::filesystem::path& audio_member,
    const ArchiveCacheIdentity& identity) noexcept;

// The caller must have observed successful final archive validation and read
// the complete built-in WAVE metadata. Capture identity before opening that
// stream, not after reading it. MPEG/add-in tag policy remains isolated.
void PublishVerifiedArchiveInfo(const std::filesystem::path& audio_member,
    const BuiltinFileInfo& info, const ArchiveCacheIdentity& identity) noexcept;

// Bounded, process-local playlist information cache. Neither operation starts
// an archive task or loads a plug-in. CUE requests require an already cached
// CUE snapshot and derive metadata/time from its referenced verified member.
// File identities and the password/volume session epoch are checked each time.
[[nodiscard]] std::optional<BuiltinFileInfo> TryReadVerifiedArchiveInfo(
    const std::filesystem::path& logical_path, int subtrack = 0) noexcept;
[[nodiscard]] bool IsArchiveInfoVerificationPending(
    const std::filesystem::path& logical_path, int subtrack = 0) noexcept;

} // namespace ttplayer::audio
