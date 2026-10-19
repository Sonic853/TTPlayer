#include "ttplayer/audio/file_mutation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ttplayer::audio {
namespace {

constexpr HRESULT Changed() noexcept { return HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }

HRESULT VersionOf(HANDLE file, FileSourceVersion& version) noexcept {
    version = {};
    if (!GetFileInformationByHandle(file, &version.info))
        return HRESULT_FROM_WIN32(GetLastError());
    if (version.info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return E_ACCESSDENIED;
    version.valid = true;
    return S_OK;
}

std::wstring Number(std::uint64_t value) { return std::to_wstring(value); }

std::wstring Identity(const FileSourceVersion& version) {
    const auto& i = version.info;
    return Number(i.dwVolumeSerialNumber) + L"." + Number(i.nFileIndexHigh) + L"." +
        Number(i.nFileIndexLow);
}

std::wstring CanonicalPath(const std::filesystem::path& path) {
    std::wstring key = std::filesystem::absolute(path).lexically_normal().native();
    const DWORD length = GetLongPathNameW(key.c_str(), nullptr, 0);
    if (length) {
        std::wstring expanded(length, L'\0');
        const DWORD copied = GetLongPathNameW(key.c_str(), expanded.data(), length);
        if (copied && copied < length) { expanded.resize(copied); key = std::move(expanded); }
    }
    // Resolve junction/symlink aliases when available, without a Vista import.
    std::vector<wchar_t> buffer(32768);
    const HANDLE file = CreateFileW(key.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        using FinalPath = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
        const auto final_path = reinterpret_cast<FinalPath>(GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetFinalPathNameByHandleW"));
        if (final_path) {
            const DWORD copied = final_path(file, buffer.data(), static_cast<DWORD>(buffer.size()), 0);
            // Close before allocating the normalized string.
            CloseHandle(file);
            if (copied && copied < buffer.size()) key.assign(buffer.data(), copied);
        } else {
            CloseHandle(file);
        }
    }
    CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

std::wstring PathMutex(const std::filesystem::path& path) {
    // A collision only serializes unrelated files; it never grants ownership.
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t ch : CanonicalPath(path)) {
        hash ^= static_cast<std::uint16_t>(ch); hash *= 1099511628211ULL;
    }
    return L"Local\\TTPlayer.FileMutation.v1.Path." + Number(hash);
}

HRESULT Lock(const std::wstring& name, HANDLE& output,
             std::stop_token stop, DWORD started, DWORD timeout) noexcept {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
    for (;;) {
        if (stop.stop_requested()) {
            CloseHandle(mutex); return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        const DWORD elapsed = GetTickCount() - started;
        const DWORD wait = WaitForSingleObject(mutex,
            std::min<DWORD>(50, elapsed < timeout ? timeout - elapsed : 0));
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { output = mutex; return S_OK; }
        if (wait != WAIT_TIMEOUT || GetTickCount() - started >= timeout) {
            const DWORD error = wait == WAIT_TIMEOUT ? ERROR_LOCK_VIOLATION : GetLastError();
            CloseHandle(mutex); return HRESULT_FROM_WIN32(error);
        }
    }
}

} // namespace

HRESULT ReadFileSourceVersion(const std::filesystem::path& path,
                              FileSourceVersion& version) noexcept {
    version = {};
    const HANDLE file = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT status = VersionOf(file, version);
    CloseHandle(file);
    return status;
}

bool SameFileSourceVersion(const FileSourceVersion& left,
                           const FileSourceVersion& right) noexcept {
    const auto& a = left.info; const auto& b = right.info;
    return left.valid && right.valid &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
        a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        CompareFileTime(&a.ftCreationTime, &b.ftCreationTime) == 0 &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}

std::wstring FileSourceVersionToken(const FileSourceVersion& version) {
    if (!version.valid) return {};
    const auto& i = version.info;
    return L"file-v1:" + Identity(version) + L":" + Number(i.nFileSizeHigh) + L":" +
        Number(i.nFileSizeLow) + L":" + Number(i.ftCreationTime.dwHighDateTime) + L":" +
        Number(i.ftCreationTime.dwLowDateTime) + L":" + Number(i.ftLastWriteTime.dwHighDateTime) +
        L":" + Number(i.ftLastWriteTime.dwLowDateTime);
}

std::wstring CaptureFileSourceVersion(const std::filesystem::path& path) {
    FileSourceVersion version;
    if (FAILED(ReadFileSourceVersion(path, version))) return {};
    return FileSourceVersionToken(version);
}

FileMutationGuard::~FileMutationGuard() { Reset(); }

void FileMutationGuard::Reset() noexcept {
    if (identity_mutex_) { ReleaseMutex(identity_mutex_); CloseHandle(identity_mutex_); identity_mutex_ = nullptr; }
    if (path_mutex_) { ReleaseMutex(path_mutex_); CloseHandle(path_mutex_); path_mutex_ = nullptr; }
    version_ = {};
}

HRESULT FileMutationGuard::Acquire(const std::filesystem::path& path,
                                   std::stop_token stop, DWORD timeout_ms) noexcept {
    Reset();
    try {
        path_ = path;
        const DWORD started = GetTickCount();
        HRESULT status = Lock(PathMutex(path), path_mutex_, stop, started, timeout_ms);
        if (FAILED(status)) return status;
        for (unsigned attempt = 0; attempt != 3; ++attempt) {
            FileSourceVersion before;
            status = ReadFileSourceVersion(path_, before);
            if (FAILED(status)) break;
            status = Lock(L"Local\\TTPlayer.FileMutation.v1.Id." + Identity(before),
                          identity_mutex_, stop, started, timeout_ms);
            if (FAILED(status)) break;
            status = ReadFileSourceVersion(path_, version_);
            if (SUCCEEDED(status) && SameFileSourceVersion(before, version_)) return S_OK;
            ReleaseMutex(identity_mutex_); CloseHandle(identity_mutex_); identity_mutex_ = nullptr;
            if (FAILED(status)) break;
            status = Changed(); // The alias changed while another writer owned its old ID.
        }
        Reset(); return status;
    } catch (...) { Reset(); return E_OUTOFMEMORY; }
}

HRESULT FileMutationGuard::Validate(HANDLE source) const noexcept {
    if (!path_mutex_ || !identity_mutex_ || !version_.valid) return E_UNEXPECTED;
    FileSourceVersion current;
    HRESULT status = ReadFileSourceVersion(path_, current);
    if (FAILED(status)) return status;
    if (!SameFileSourceVersion(version_, current)) return Changed();
    if (source != INVALID_HANDLE_VALUE && source) {
        status = VersionOf(source, current);
        if (FAILED(status)) return status;
        if (!SameFileSourceVersion(version_, current)) return Changed();
    }
    return S_OK;
}

} // namespace ttplayer::audio
