#include "ttplayer/audio/format_probe.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>

namespace ttplayer::audio {
namespace {
bool MpegHeader(std::span<const unsigned char> bytes) {
    if (bytes.size() < 4) return false;
    const DWORD value = (DWORD(bytes[0]) << 24) | (DWORD(bytes[1]) << 16) |
                        (DWORD(bytes[2]) << 8) | bytes[3];
    const DWORD bitrate = (value >> 12) & 15;
    return (value & 0xffe00000) == 0xffe00000 && bitrate != 0 && bitrate != 15 &&
           (value & 0x60000) != 0 && (value & 0xc00) != 0xc00 &&
           (value & 0x180000) != 0x80000;
}
bool Id3Header(std::span<const unsigned char> bytes) {
    return bytes.size() >= 10 && std::memcmp(bytes.data(), "ID3", 3) == 0 &&
           bytes[3] >= 2 && bytes[3] <= 4 && bytes[4] != 0xff &&
           ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80) == 0;
}
std::wstring Lower(std::wstring_view value) {
    std::wstring result(value);
    for (auto& ch : result) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    return result;
}
}

bool IsWebmContainerHeader(std::span<const unsigned char> header) noexcept {
    if (header.size() < 5 || header[0] != 0x1a || header[1] != 0x45 ||
        header[2] != 0xdf || header[3] != 0xa3) return false;
    size_t at = 4;
    const auto read = [&](bool id, std::uint64_t& value) {
        if (at >= header.size() || !header[at]) return false;
        const unsigned first = header[at++]; unsigned mask = 128, n = 1;
        while (!(first & mask)) { mask >>= 1; ++n; }
        if (n > (id ? 4u : 8u) || n - 1 > header.size() - at) return false;
        value = id ? first : first & (mask - 1);
        for (unsigned i = 1; i < n; ++i) value = (value << 8) | header[at++];
        return id || value != ((std::uint64_t(1) << (7 * n)) - 1);
    };
    std::uint64_t size{};
    if (!read(false, size) || size > 4096 - at || size > header.size() - at) return false;
    const size_t end = at + static_cast<size_t>(size);
    bool webm{};
    while (at < end) {
        std::uint64_t id{}, length{};
        if (!read(true, id) || !read(false, length) || at > end || length > end - at) return false;
        if (id == 0x4282) {
            if (webm || length != 4 || std::memcmp(header.data() + at, "webm", 4)) return false;
            webm = true;
        }
        at += static_cast<size_t>(length);
    }
    return webm;
}

std::wstring AudioExtensionHint(const std::filesystem::path& path) {
    auto name = path.native();
    if (name.find(L"://") != std::wstring::npos) {
        const auto query = name.find(L'?');
        if (query != std::wstring::npos) name.resize(query);
    }
    return Lower(std::filesystem::path(name).extension().native());
}

std::wstring RecoverAudioFormatHint(
    std::wstring_view extension, std::span<const unsigned char> header) {
    auto result = Lower(extension);
    if (header.size() < 16) return result;
    if (IsWebmContainerHeader(header)) return result == L".weba" ? result : L".webm";
    if (std::memcmp(header.data(), "MAC", 3) == 0) return L".mac";
    if (std::memcmp(header.data(), "MP+", 3) == 0) return L".mpc";
    if (std::memcmp(header.data(), "TTA", 3) == 0) return L".tta";
    if (std::memcmp(header.data(), ".RMF", 4) == 0) return L".ram";
    if (std::memcmp(header.data() + 4, "ftyp", 4) == 0) return L".mp4";
    constexpr std::array<unsigned char, 16> asf{
        0x30,0x26,0xb2,0x75,0x8e,0x66,0xcf,0x11,0xa6,0xd9,0x00,0xaa,0x00,0x62,0xce,0x6c};
    if (std::equal(asf.begin(), asf.end(), header.begin())) return L".wma";
    if (Id3Header(header)) {
        if (result.empty() || result == L".wma") return L".mp3";
    } else if ((result.empty() || result == L".asf" || result == L".wma" ||
                result == L".ra" || result == L".ram" || result == L".rm") &&
               MpegHeader(header)) return L".mp3";
    return result;
}

HRESULT ProbeAudioFormatHint(const std::filesystem::path& path, std::wstring& extension) {
    extension = AudioExtensionHint(path);
    if (path.native().find(L"://") != std::wstring::npos) return S_FALSE;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    std::array<unsigned char, 4096> header{};
    DWORD count{};
    BOOL read = ReadFile(file, header.data(), 16, &count, nullptr);
    if (read && count == 16 && header[0] == 0x1a && header[1] == 0x45 && header[2] == 0xdf && header[3] == 0xa3) {
        DWORD extra{};
        read = ReadFile(file, header.data() + count, static_cast<DWORD>(header.size()) - count, &extra, nullptr);
        count += extra;
    }
    const HRESULT status = read ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    if (SUCCEEDED(status)) extension = RecoverAudioFormatHint(extension, {header.data(), count});
    return status;
}

HRESULT ProbeAudioFormatHint(IStream* stream, std::wstring& extension) {
    if (!stream) return E_POINTER;
    LARGE_INTEGER zero{};
    ULARGE_INTEGER saved{};
    HRESULT status = stream->Seek(zero, STREAM_SEEK_CUR, &saved);
    if (FAILED(status)) return status;
    status = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::array<unsigned char, 4096> header{};
    ULONG count{};
    ULONG wanted = 16;
    while (SUCCEEDED(status) && count < wanted) {
        ULONG got{};
        status = stream->Read(header.data() + count, wanted - count, &got);
        if (got > wanted - count) { status = STG_E_READFAULT; break; }
        count += got;
        if (!got) break;
        if (count == 16 && header[0] == 0x1a && header[1] == 0x45 && header[2] == 0xdf && header[3] == 0xa3)
            wanted = static_cast<ULONG>(header.size());
    }
    LARGE_INTEGER previous{};
    previous.QuadPart = static_cast<LONGLONG>(saved.QuadPart);
    const HRESULT restored = stream->Seek(previous, STREAM_SEEK_SET, nullptr);
    if (FAILED(restored)) return restored;
    if (SUCCEEDED(status)) extension = RecoverAudioFormatHint(extension, {header.data(), count});
    return status;
}

bool IsTerminalAudioOpenError(HRESULT result) noexcept {
    // 004C84CD distinguishes abort/transport failures from unrecognized media.
    // File access and memory failures likewise cannot be fixed by another codec.
    if (result == E_ABORT || result == E_OUTOFMEMORY || result == E_ACCESSDENIED ||
        result == STG_E_ACCESSDENIED || result == STG_E_FILENOTFOUND ||
        result == STG_E_PATHNOTFOUND || result == STG_E_SHAREVIOLATION ||
        result == STG_E_LOCKVIOLATION || result == STG_E_READFAULT ||
        result == static_cast<HRESULT>(0x80004014) ||
        result == static_cast<HRESULT>(0x80040064) ||
        result == static_cast<HRESULT>(0x80040065)) return true;
    if (HRESULT_FACILITY(result) != FACILITY_WIN32) return false;
    switch (HRESULT_CODE(result)) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_READ_FAULT:
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_CANCELLED:
    case ERROR_OPERATION_ABORTED: case 0x42b: case 0x4d3: case 0x4d4:
    case 0x13d2: case 0x2745: return true;
    default: return false;
    }
}
} // namespace ttplayer::audio
