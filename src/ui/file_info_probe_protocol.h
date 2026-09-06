#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <windows.h>
#include <mmreg.h>

namespace ttplayer::ui::detail {

// File Properties deliberately crosses a process boundary before entering a
// legacy sound add-in.  Some third-party readers never return from Open or a
// tag writer.  A versioned, length-prefixed packet lets the UI terminate that
// helper without ever terminating an in-process thread or trusting partial
// output from a killed process.
inline constexpr std::uint32_t kFileInfoProbeMagic = 0x50494654U; // TFIP
inline constexpr std::uint32_t kFileInfoProbeVersion = 3;
inline constexpr std::uint32_t kFileInfoProbeReadPacket = 1;
inline constexpr std::uint32_t kFileInfoProbeWriteRequestPacket = 2;
inline constexpr std::uint32_t kFileInfoProbeWriteResultPacket = 3;
inline constexpr std::uint32_t kFileInfoProbeReadRequestPacket = 4;
inline constexpr std::uint32_t kFileInfoProbeMaximumFields = 4096;
inline constexpr std::uint32_t kFileInfoProbeMaximumCharacters = 1024U * 1024U;
inline constexpr std::uint32_t kFileInfoProbeMaximumCoverBytes = 64U * 1024U * 1024U;

struct FileInfoProbeMetadata {
    std::wstring name;
    std::wstring value;
};

struct FileInfoProbeReadResult {
    HRESULT status{E_FAIL};
    DWORD capabilities{};
    WAVEFORMATEX format{};
    DWORD duration_ms{};
    DWORD encoded_bits_per_second{};
    std::wstring codec;
    std::vector<FileInfoProbeMetadata> metadata;
    std::vector<unsigned char> cover;
};

struct FileInfoProbeWriteField {
    std::string name;
    std::wstring value;
};

enum class FileInfoProbeCoverAction : std::uint32_t {
    unchanged = 0,
    replace = 1,
    remove = 2,
};

// FUN_004D9B0B passes MP3ReadTagPriority to 004DC174, while
// FUN_004D9C09/004D9C7E/004D9C87 pass the other three values to the built-in
// MP3 tag writer.  Keep them in the isolated-helper protocol so the Options
// values affect actual tag I/O rather than only the property-page controls.
struct FileInfoProbeMp3Policy {
    std::uint32_t read_priority{0x04080000U};
    std::uint32_t write_type{5U};
    std::uint32_t id3v2_encoding{};
    std::uint32_t id3v2_padding{1U};
};

struct FileInfoProbeReadRequest {
    FileInfoProbeMp3Policy mp3;
};

struct FileInfoProbeWriteRequest {
    FileInfoProbeMp3Policy mp3;
    std::vector<FileInfoProbeWriteField> fields;
    FileInfoProbeCoverAction cover_action{FileInfoProbeCoverAction::unchanged};
    std::vector<unsigned char> cover;
};

struct FileInfoProbeWriteResult {
    HRESULT status{E_FAIL};
    std::vector<HRESULT> fields;
    // S_FALSE means the request did not contain a cover mutation.
    HRESULT cover_status{S_FALSE};
};

inline bool FileInfoProbeWriteBytes(HANDLE file, const void* data,
                                    std::uint32_t size) noexcept {
    if (file == INVALID_HANDLE_VALUE || (size != 0 && !data)) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t written_total{};
    while (written_total < size) {
        DWORD written{};
        if (!WriteFile(file, bytes + written_total, size - written_total,
                       &written, nullptr) || written == 0)
            return false;
        written_total += written;
    }
    return true;
}

inline bool FileInfoProbeReadBytes(HANDLE file, void* data,
                                   std::uint32_t size) noexcept {
    if (file == INVALID_HANDLE_VALUE || (size != 0 && !data)) return false;
    auto* bytes = static_cast<unsigned char*>(data);
    std::uint32_t read_total{};
    while (read_total < size) {
        DWORD read{};
        if (!ReadFile(file, bytes + read_total, size - read_total, &read,
                      nullptr) || read == 0)
            return false;
        read_total += read;
    }
    return true;
}

template <typename T>
inline bool FileInfoProbeWriteScalar(HANDLE file, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return FileInfoProbeWriteBytes(file, &value, sizeof(value));
}

template <typename T>
inline bool FileInfoProbeReadScalar(HANDLE file, T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    return FileInfoProbeReadBytes(file, &value, sizeof(value));
}

inline bool FileInfoProbeWriteWide(HANDLE file,
                                   std::wstring_view value) noexcept {
    if (value.size() > kFileInfoProbeMaximumCharacters) return false;
    const auto count = static_cast<std::uint32_t>(value.size());
    return FileInfoProbeWriteScalar(file, count) &&
        FileInfoProbeWriteBytes(file, value.data(),
            count * static_cast<std::uint32_t>(sizeof(wchar_t)));
}

inline bool FileInfoProbeReadWide(HANDLE file, std::wstring& value) {
    std::uint32_t count{};
    if (!FileInfoProbeReadScalar(file, count) ||
        count > kFileInfoProbeMaximumCharacters)
        return false;
    try { value.resize(count); }
    catch (...) { return false; }
    return FileInfoProbeReadBytes(file, value.data(),
        count * static_cast<std::uint32_t>(sizeof(wchar_t)));
}

inline bool FileInfoProbeWriteNarrow(HANDLE file,
                                     std::string_view value) noexcept {
    if (value.size() > kFileInfoProbeMaximumCharacters) return false;
    const auto count = static_cast<std::uint32_t>(value.size());
    return FileInfoProbeWriteScalar(file, count) &&
           FileInfoProbeWriteBytes(file, value.data(), count);
}

inline bool FileInfoProbeReadNarrow(HANDLE file, std::string& value) {
    std::uint32_t count{};
    if (!FileInfoProbeReadScalar(file, count) ||
        count > kFileInfoProbeMaximumCharacters)
        return false;
    try { value.resize(count); }
    catch (...) { return false; }
    return FileInfoProbeReadBytes(file, value.data(), count);
}

inline bool FileInfoProbeWriteHeader(HANDLE file,
                                     std::uint32_t kind) noexcept {
    return FileInfoProbeWriteScalar(file, kFileInfoProbeMagic) &&
           FileInfoProbeWriteScalar(file, kFileInfoProbeVersion) &&
           FileInfoProbeWriteScalar(file, kind);
}

inline bool FileInfoProbeReadHeader(HANDLE file,
                                    std::uint32_t expected_kind) noexcept {
    std::uint32_t magic{}, version{}, kind{};
    return FileInfoProbeReadScalar(file, magic) &&
           FileInfoProbeReadScalar(file, version) &&
           FileInfoProbeReadScalar(file, kind) &&
           magic == kFileInfoProbeMagic &&
           version == kFileInfoProbeVersion && kind == expected_kind;
}

inline bool FileInfoProbeWriteMp3Policy(
    HANDLE file, const FileInfoProbeMp3Policy& policy) noexcept {
    return FileInfoProbeWriteScalar(file, policy.read_priority) &&
           FileInfoProbeWriteScalar(file, policy.write_type) &&
           FileInfoProbeWriteScalar(file, policy.id3v2_encoding) &&
           FileInfoProbeWriteScalar(file, policy.id3v2_padding);
}

inline bool FileInfoProbeReadMp3Policy(
    HANDLE file, FileInfoProbeMp3Policy& policy) noexcept {
    return FileInfoProbeReadScalar(file, policy.read_priority) &&
           FileInfoProbeReadScalar(file, policy.write_type) &&
           FileInfoProbeReadScalar(file, policy.id3v2_encoding) &&
           FileInfoProbeReadScalar(file, policy.id3v2_padding);
}

inline bool WriteFileInfoProbeReadRequest(
    const std::filesystem::path& path,
    const FileInfoProbeReadRequest& request) noexcept {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool okay = FileInfoProbeWriteHeader(
        file, kFileInfoProbeReadRequestPacket) &&
        FileInfoProbeWriteMp3Policy(file, request.mp3);
    CloseHandle(file);
    if (!okay) DeleteFileW(path.c_str());
    return okay;
}

inline bool ReadFileInfoProbeReadRequest(
    const std::filesystem::path& path, FileInfoProbeReadRequest& request) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FileInfoProbeReadRequest decoded;
    const bool okay = FileInfoProbeReadHeader(
        file, kFileInfoProbeReadRequestPacket) &&
        FileInfoProbeReadMp3Policy(file, decoded.mp3);
    CloseHandle(file);
    if (okay) request = decoded;
    return okay;
}

inline bool WriteFileInfoProbeReadResult(
    const std::filesystem::path& path,
    const FileInfoProbeReadResult& result) noexcept {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto close = [&] { CloseHandle(file); };
    const std::uint32_t metadata_count = static_cast<std::uint32_t>(
        std::min<size_t>(result.metadata.size(), kFileInfoProbeMaximumFields));
    const std::uint32_t cover_size = static_cast<std::uint32_t>(
        std::min<size_t>(result.cover.size(), kFileInfoProbeMaximumCoverBytes));
    bool okay = FileInfoProbeWriteHeader(file, kFileInfoProbeReadPacket) &&
        FileInfoProbeWriteScalar(file, result.status) &&
        FileInfoProbeWriteScalar(file, result.capabilities) &&
        FileInfoProbeWriteScalar(file, result.format) &&
        FileInfoProbeWriteScalar(file, result.duration_ms) &&
        FileInfoProbeWriteScalar(file, result.encoded_bits_per_second) &&
        FileInfoProbeWriteWide(file, result.codec) &&
        FileInfoProbeWriteScalar(file, metadata_count);
    for (std::uint32_t index = 0; okay && index < metadata_count; ++index) {
        okay = FileInfoProbeWriteWide(file, result.metadata[index].name) &&
               FileInfoProbeWriteWide(file, result.metadata[index].value);
    }
    okay = okay && FileInfoProbeWriteScalar(file, cover_size) &&
        FileInfoProbeWriteBytes(file, result.cover.data(), cover_size);
    close();
    if (!okay) DeleteFileW(path.c_str());
    return okay;
}

inline bool ReadFileInfoProbeReadResult(
    const std::filesystem::path& path, FileInfoProbeReadResult& result) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FileInfoProbeReadResult decoded;
    std::uint32_t metadata_count{};
    bool okay = FileInfoProbeReadHeader(file, kFileInfoProbeReadPacket) &&
        FileInfoProbeReadScalar(file, decoded.status) &&
        FileInfoProbeReadScalar(file, decoded.capabilities) &&
        FileInfoProbeReadScalar(file, decoded.format) &&
        FileInfoProbeReadScalar(file, decoded.duration_ms) &&
        FileInfoProbeReadScalar(file, decoded.encoded_bits_per_second) &&
        FileInfoProbeReadWide(file, decoded.codec) &&
        FileInfoProbeReadScalar(file, metadata_count) &&
        metadata_count <= kFileInfoProbeMaximumFields;
    if (okay) {
        try { decoded.metadata.resize(metadata_count); }
        catch (...) { okay = false; }
    }
    for (std::uint32_t index = 0; okay && index < metadata_count; ++index) {
        okay = FileInfoProbeReadWide(file, decoded.metadata[index].name) &&
               FileInfoProbeReadWide(file, decoded.metadata[index].value);
    }
    std::uint32_t cover_size{};
    okay = okay && FileInfoProbeReadScalar(file, cover_size) &&
           cover_size <= kFileInfoProbeMaximumCoverBytes;
    if (okay) {
        try { decoded.cover.resize(cover_size); }
        catch (...) { okay = false; }
    }
    okay = okay && FileInfoProbeReadBytes(file, decoded.cover.data(), cover_size);
    CloseHandle(file);
    if (okay) result = std::move(decoded);
    return okay;
}

inline bool WriteFileInfoProbeWriteRequest(
    const std::filesystem::path& path,
    const FileInfoProbeWriteRequest& request) noexcept {
    if (request.fields.size() > kFileInfoProbeMaximumFields ||
        request.cover.size() > kFileInfoProbeMaximumCoverBytes ||
        static_cast<std::uint32_t>(request.cover_action) >
            static_cast<std::uint32_t>(FileInfoProbeCoverAction::remove) ||
        (request.cover_action == FileInfoProbeCoverAction::replace &&
         request.cover.empty()) ||
        (request.cover_action != FileInfoProbeCoverAction::replace &&
         !request.cover.empty()))
        return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto count = static_cast<std::uint32_t>(request.fields.size());
    bool okay = FileInfoProbeWriteHeader(
        file, kFileInfoProbeWriteRequestPacket) &&
        FileInfoProbeWriteMp3Policy(file, request.mp3) &&
        FileInfoProbeWriteScalar(file, count);
    for (const auto& field : request.fields) {
        okay = okay && FileInfoProbeWriteNarrow(file, field.name) &&
               FileInfoProbeWriteWide(file, field.value);
    }
    const auto cover_action = static_cast<std::uint32_t>(request.cover_action);
    const auto cover_size = static_cast<std::uint32_t>(request.cover.size());
    okay = okay && FileInfoProbeWriteScalar(file, cover_action) &&
        FileInfoProbeWriteScalar(file, cover_size) &&
        FileInfoProbeWriteBytes(file, request.cover.data(), cover_size);
    CloseHandle(file);
    if (!okay) DeleteFileW(path.c_str());
    return okay;
}

inline bool ReadFileInfoProbeWriteRequest(
    const std::filesystem::path& path, FileInfoProbeWriteRequest& request) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FileInfoProbeWriteRequest decoded;
    std::uint32_t count{};
    bool okay = FileInfoProbeReadHeader(
        file, kFileInfoProbeWriteRequestPacket) &&
        FileInfoProbeReadMp3Policy(file, decoded.mp3) &&
        FileInfoProbeReadScalar(file, count) &&
        count <= kFileInfoProbeMaximumFields;
    if (okay) {
        try { decoded.fields.resize(count); }
        catch (...) { okay = false; }
    }
    for (std::uint32_t index = 0; okay && index < count; ++index) {
        okay = FileInfoProbeReadNarrow(file, decoded.fields[index].name) &&
               FileInfoProbeReadWide(file, decoded.fields[index].value);
    }
    std::uint32_t cover_action{};
    std::uint32_t cover_size{};
    okay = okay && FileInfoProbeReadScalar(file, cover_action) &&
        cover_action <= static_cast<std::uint32_t>(
            FileInfoProbeCoverAction::remove) &&
        FileInfoProbeReadScalar(file, cover_size) &&
        cover_size <= kFileInfoProbeMaximumCoverBytes;
    if (okay) {
        decoded.cover_action = static_cast<FileInfoProbeCoverAction>(
            cover_action);
        try { decoded.cover.resize(cover_size); }
        catch (...) { okay = false; }
    }
    okay = okay && FileInfoProbeReadBytes(
        file, decoded.cover.data(), cover_size) &&
        ((decoded.cover_action == FileInfoProbeCoverAction::replace &&
          !decoded.cover.empty()) ||
         (decoded.cover_action != FileInfoProbeCoverAction::replace &&
          decoded.cover.empty()));
    CloseHandle(file);
    if (okay) request = std::move(decoded);
    return okay;
}

inline bool WriteFileInfoProbeWriteResult(
    const std::filesystem::path& path,
    const FileInfoProbeWriteResult& result) noexcept {
    if (result.fields.size() > kFileInfoProbeMaximumFields) return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto count = static_cast<std::uint32_t>(result.fields.size());
    bool okay = FileInfoProbeWriteHeader(
        file, kFileInfoProbeWriteResultPacket) &&
        FileInfoProbeWriteScalar(file, result.status) &&
        FileInfoProbeWriteScalar(file, count);
    for (const HRESULT field : result.fields)
        okay = okay && FileInfoProbeWriteScalar(file, field);
    okay = okay && FileInfoProbeWriteScalar(file, result.cover_status);
    CloseHandle(file);
    if (!okay) DeleteFileW(path.c_str());
    return okay;
}

inline bool ReadFileInfoProbeWriteResult(
    const std::filesystem::path& path, FileInfoProbeWriteResult& result) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FileInfoProbeWriteResult decoded;
    std::uint32_t count{};
    bool okay = FileInfoProbeReadHeader(
        file, kFileInfoProbeWriteResultPacket) &&
        FileInfoProbeReadScalar(file, decoded.status) &&
        FileInfoProbeReadScalar(file, count) &&
        count <= kFileInfoProbeMaximumFields;
    if (okay) {
        try { decoded.fields.resize(count); }
        catch (...) { okay = false; }
    }
    for (std::uint32_t index = 0; okay && index < count; ++index)
        okay = FileInfoProbeReadScalar(file, decoded.fields[index]);
    okay = okay && FileInfoProbeReadScalar(file, decoded.cover_status);
    CloseHandle(file);
    if (okay) result = std::move(decoded);
    return okay;
}

} // namespace ttplayer::ui::detail
