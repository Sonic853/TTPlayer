#include "ttplayer/plugins/legacy_file_stream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <wrl/client.h>

namespace ttplayer::plugins {
namespace {

using Microsoft::WRL::ComPtr;

// 00437C0D / 004C3F52: the decoder has a logical cursor inside a window of
// encoded bytes. Use the file handle directly: SHCreateStreamOnFileEx does
// its own read-ahead even when the caller has requested zero buffering.
class NamedFileStream final : public IStream, public LegacyFileBuffer {
public:
    NamedFileStream(HANDLE file, const wchar_t* path, DWORD mode)
        : file_(file), path_(path), mode_(mode) {
        constexpr DWORD initial_bytes = 4096; // 004C522B, before reader Open
        buffer_.reset(new (std::nothrow) BYTE[initial_bytes]);
        if (buffer_) capacity_ = initial_bytes;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream)
            *value = static_cast<IStream*>(this);
        else if (iid == kFileBufferInterface)
            *value = static_cast<LegacyFileBuffer*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetCapacity(DWORD* bytes) override {
        if (!bytes) return E_POINTER;
        std::scoped_lock lock(mutex_);
        *bytes = capacity_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCapacity(DWORD bytes) override {
        std::scoped_lock lock(mutex_);
        if (bytes == capacity_) return S_OK;
        if (bytes && (mode_ & (STGM_WRITE | STGM_READWRITE)) == STGM_READ) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file_, &size))
                bytes = static_cast<DWORD>(std::min<LONGLONG>(bytes, size.QuadPart));
        }
        if (bytes == capacity_) return S_OK;
        valid_ = 0;
        buffer_.reset();
        if (bytes) buffer_.reset(new (std::nothrow) BYTE[bytes]);
        capacity_ = buffer_ ? bytes : 0;
        return capacity_ == bytes ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Read(void* value, ULONG size, ULONG* read) override {
        ULONG ignored{};
        if (!read) read = &ignored;
        *read = 0;
        if (!size) return S_OK;
        if (!value) return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_);
        if (mode_ & STGM_WRITE) return STG_E_ACCESSDENIED;
        auto* destination = static_cast<BYTE*>(value);
        const DWORD offset = valid_ ? static_cast<DWORD>(position_ - begin_) : 0;
        const ULONG available = std::min(size, valid_ - offset);
        if (available) {
            std::memcpy(destination, buffer_.get() + offset, available);
            position_ += available;
            *read = available;
        }
        const ULONG remaining = size - available;
        if (!remaining) return S_OK;
        valid_ = 0;
        const HRESULT result = PositionFile(position_);
        if (FAILED(result)) return result;
        const bool refill = remaining < capacity_;
        DWORD got{};
        if (!ReadFile(file_, refill ? buffer_.get() : destination + available,
                      refill ? capacity_ : remaining, &got, nullptr)) {
            physical_ = -1;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        physical_ += got;
        if (refill) {
            begin_ = position_;
            valid_ = got;
            got = std::min(got, remaining);
            std::memcpy(destination + available, buffer_.get(), got);
        }
        position_ += got;
        *read += got;
        return *read == size ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* position) override {
        std::scoped_lock lock(mutex_);
        if (origin == STREAM_SEEK_CUR && !move.QuadPart) {
            if (position) position->QuadPart = position_;
            return S_OK;
        }
        LARGE_INTEGER base{};
        if (origin == STREAM_SEEK_CUR) base.QuadPart = position_;
        else if (origin == STREAM_SEEK_END) {
            if (!GetFileSizeEx(file_, &base)) return HRESULT_FROM_WIN32(GetLastError());
        } else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart < -base.QuadPart || move.QuadPart > INT64_MAX - base.QuadPart)
            return STG_E_INVALIDFUNCTION;
        const LONGLONG target = base.QuadPart + move.QuadPart;
        if (!valid_ || target < begin_ || target - begin_ >= valid_) {
            const HRESULT result = PositionFile(target);
            if (FAILED(result)) return result;
            valid_ = 0;
        }
        position_ = target;
        if (position) position->QuadPart = target;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void* value, ULONG size, ULONG* written) override {
        if (written) *written = 0;
        if (size && !value) return STG_E_INVALIDPOINTER;
        if (!(mode_ & (STGM_READWRITE | STGM_WRITE))) return STG_E_ACCESSDENIED;
        std::scoped_lock lock(mutex_);
        const HRESULT result = PositionFile(position_);
        if (FAILED(result)) return result;
        valid_ = 0;
        DWORD actual{};
        if (!WriteFile(file_, value, size, &actual, nullptr)) {
            physical_ = -1;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        position_ += actual;
        physical_ = position_;
        if (written) *written = actual;
        return actual == size ? S_OK : STG_E_MEDIUMFULL;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        if (!(mode_ & (STGM_READWRITE | STGM_WRITE))) return STG_E_ACCESSDENIED;
        if (size.QuadPart > INT64_MAX) return STG_E_INVALIDFUNCTION;
        std::scoped_lock lock(mutex_);
        const HRESULT result = PositionFile(static_cast<LONGLONG>(size.QuadPart));
        if (FAILED(result)) return result;
        if (!SetEndOfFile(file_)) return HRESULT_FROM_WIN32(GetLastError());
        valid_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER size,
        ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!target) return STG_E_INVALIDPOINTER;
        std::array<BYTE, 16 * 1024> block;
        while (size.QuadPart) {
            const ULONG count = static_cast<ULONG>(std::min<ULONGLONG>(size.QuadPart, block.size()));
            ULONG got{}, sent{};
            const HRESULT result = Read(block.data(), count, &got);
            if (read) read->QuadPart += got;
            if (FAILED(result)) return result;
            if (got) {
                const HRESULT copied = target->Write(block.data(), got, &sent);
                if (written) written->QuadPart += sent;
                if (FAILED(copied)) return copied;
                if (sent != got) return STG_E_MEDIUMFULL;
            }
            if (got != count) return S_FALSE;
            size.QuadPart -= got;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        std::scoped_lock lock(mutex_);
        if (!(mode_ & (STGM_READWRITE | STGM_WRITE))) return S_OK;
        return FlushFileBuffers(file_) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type) override {
        OVERLAPPED region{};
        region.Offset = offset.LowPart;
        region.OffsetHigh = offset.HighPart;
        const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | ((type & LOCK_EXCLUSIVE) ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        return LockFileEx(file_, flags, 0, size.LowPart, size.HighPart, &region)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD) override {
        return UnlockFile(file_, offset.LowPart, offset.HighPart, size.LowPart, size.HighPart)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file_, &info)) return HRESULT_FROM_WIN32(GetLastError());
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->grfMode = mode_;
        stat->cbSize.LowPart = info.nFileSizeLow;
        stat->cbSize.HighPart = info.nFileSizeHigh;
        stat->ctime = info.ftCreationTime;
        stat->mtime = info.ftLastWriteTime;
        stat->atime = info.ftLastAccessTime;
        // FLAC/APE use the physical Stat name for metadata capabilities.
        if (!(flags & STATFLAG_NONAME)) {
            const size_t bytes = (path_.size() + 1) * sizeof(wchar_t);
            stat->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
            if (!stat->pwcsName) return E_OUTOFMEMORY;
            std::memcpy(stat->pwcsName, path_.c_str(), bytes);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        return E_NOTIMPL; // Neither the original file IStream nor the shell backing supported Clone.
    }

private:
    ~NamedFileStream() { CloseHandle(file_); }
    HRESULT PositionFile(LONGLONG target) noexcept {
        if (physical_ == target) return S_OK;
        LARGE_INTEGER offset{};
        offset.QuadPart = target;
        if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN))
            return HRESULT_FROM_WIN32(GetLastError());
        physical_ = target;
        return S_OK;
    }

    LONG references_{1};
    HANDLE file_;
    std::wstring path_;
    DWORD mode_{};
    std::mutex mutex_;
    std::unique_ptr<BYTE[]> buffer_;
    DWORD capacity_{};
    LONGLONG position_{}, physical_{}, begin_{};
    DWORD valid_{};
};

} // namespace

HRESULT CreateLegacyFileStream(const wchar_t* path, DWORD mode, IStream** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!path || !*path) return E_INVALIDARG;
    const DWORD access_mode = mode & (STGM_WRITE | STGM_READWRITE);
    if (access_mode == (STGM_WRITE | STGM_READWRITE)) return E_INVALIDARG;
    DWORD access = access_mode == STGM_WRITE ? GENERIC_WRITE : GENERIC_READ;
    if (access_mode == STGM_READWRITE || (mode & STGM_CREATE)) access |= GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    switch (mode & 0x70) {
    case STGM_SHARE_EXCLUSIVE: share = 0; break;
    case STGM_SHARE_DENY_WRITE: share &= ~FILE_SHARE_WRITE; break;
    case STGM_SHARE_DENY_READ: share &= ~FILE_SHARE_READ; break;
    }
    const HANDLE file = CreateFileW(path, access, share, nullptr,
        (mode & STGM_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    try {
        *output = new NamedFileStream(file, path, mode);
        return S_OK;
    } catch (const std::bad_alloc&) {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
}

HRESULT SetLegacyFileBuffer(IStream* stream, int bytes) noexcept {
    if (!stream) return E_POINTER;
    ComPtr<LegacyFileBuffer> buffer;
    const HRESULT result = stream->QueryInterface(kFileBufferInterface,
        reinterpret_cast<void**>(buffer.GetAddressOf()));
    if (result == E_NOINTERFACE) return S_FALSE;
    if (FAILED(result) || !buffer) return FAILED(result) ? result : E_UNEXPECTED;
    return buffer->SetCapacity(static_cast<DWORD>(std::max(0, bytes)));
}

} // namespace ttplayer::plugins
