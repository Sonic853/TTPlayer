#include "ttplayer/audio/file_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ttplayer::audio {
namespace {
// LAME project's BladeMP3EncDLL.h: packed BE_CONFIG_LAME/LHV1, cdecl.
// Public ABI, not a structure inferred from a particular compiler's padding.
#pragma pack(push, 1)
struct BladeConfig {
    DWORD type{256}, version{1}, size{331};
    DWORD rate{}, resample{};
    LONG mode{};
    DWORD bitrate{}, maximum_bitrate{};
    LONG preset{-1};
    DWORD mpeg{1}, psycho{}, emphasis{};
    BOOL private_bit{}, crc{}, copyright_bit{}, original{TRUE};
    BOOL info_tag{TRUE}, vbr{};
    int vbr_quality{};
    DWORD abr_bps{};
    int vbr_method{};
    BOOL no_reservoir{}, strict_iso{};
    WORD quality{0xfd02};
    BYTE reserved[237]{};
};
#pragma pack(pop)
static_assert(sizeof(BladeConfig) == 331);
using Init = DWORD (__cdecl*)(BladeConfig*, DWORD*, DWORD*, void**);
using Encode = DWORD (__cdecl*)(void*, DWORD, short*, BYTE*, DWORD*);
using Flush = DWORD (__cdecl*)(void*, BYTE*, DWORD*);
using Close = DWORD (__cdecl*)(void*);
using InfoTag = DWORD (__cdecl*)(void*, const char*);

HMODULE LoadLame() {
    const auto path = LameEncoderPath();
    return path.empty() ? nullptr : LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

bool HasLameApi(HMODULE module) {
    if (!module) return false;
    for (const char* name : {"beInitStream", "beEncodeChunk", "beDeinitStream",
                             "beCloseStream", "beWriteInfoTag"})
        if (!GetProcAddress(module, name)) return false;
    return true;
}

HRESULT BladeError(DWORD error) {
    if (!error) return S_OK;
    return error == 1 || error == 2
        ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : E_FAIL;
}
}

std::filesystem::path LameEncoderPath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                          static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"lame_enc.dll";
}

bool LameEncoderAvailable() {
    HMODULE module = LoadLame();
    const bool result = HasLameApi(module);
    if (module) FreeLibrary(module);
    return result;
}

struct FileEncoder::Impl {
    HANDLE file{INVALID_HANDLE_VALUE};
    HMODULE module{};
    void* stream{};
    Encode encode{};
    Flush flush{};
    Close close{};
    InfoTag info_tag{};
    DWORD samples_per_chunk{};
    std::vector<BYTE> output;
    std::vector<short> pending;
    WAVEFORMATEX format{};
    std::filesystem::path path;
    std::uint64_t bytes{};
    bool finished{};
    ~Impl() {
        if (stream && close) close(stream);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (module) FreeLibrary(module);
    }
    HRESULT WriteBytes(const void* data, size_t size) {
        if (size > MAXDWORD) return E_INVALIDARG;
        DWORD written{};
        if (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        return written == size ? S_OK : STG_E_WRITEFAULT;
    }
    HRESULT EncodeChunk(short* data, DWORD count) {
        DWORD size{};
        HRESULT result = BladeError(encode(stream, count, data, output.data(), &size));
        if (FAILED(result)) return result;
        if (size > output.size()) return E_UNEXPECTED;
        return WriteBytes(output.data(), size);
    }
    HRESULT WaveHeader() {
        // FUN_004E8420/004E8500: PCM fmt chunk is 16 bytes, then data;
        // patch both 32-bit lengths on finalization (no RIFF size wrapping).
        if (bytes > MAXDWORD - 36U) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::array<BYTE, 44> header{};
        const DWORD riff = static_cast<DWORD>(bytes) + 36U;
        const DWORD fmt = 16, data = static_cast<DWORD>(bytes);
        std::memcpy(header.data(), "RIFF", 4);
        std::memcpy(header.data()+4, &riff, 4);
        std::memcpy(header.data()+8, "WAVEfmt ", 8);
        std::memcpy(header.data()+16, &fmt, 4);
        std::memcpy(header.data()+20, &format, 16);
        std::memcpy(header.data()+36, "data", 4);
        std::memcpy(header.data()+40, &data, 4);
        if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN))
            return HRESULT_FROM_WIN32(GetLastError());
        return WriteBytes(header.data(), header.size());
    }
};

FileEncoder::FileEncoder() : impl_(std::make_unique<Impl>()) {}
FileEncoder::~FileEncoder() = default;

HRESULT FileEncoder::Open(const std::filesystem::path& path,
                          const WAVEFORMATEX& format, bool lame,
                          const LameEncoderOptions& options) {
    if (impl_->file != INVALID_HANDLE_VALUE || impl_->module) return E_UNEXPECTED;
    if (format.wFormatTag != WAVE_FORMAT_PCM || !format.nChannels ||
        !format.nSamplesPerSec || !format.nBlockAlign ||
        (format.wBitsPerSample != 8 && format.wBitsPerSample != 16 &&
         format.wBitsPerSample != 24 && format.wBitsPerSample != 32)) return E_INVALIDARG;
    impl_->format = format;
    impl_->path = path;
    if (lame) {
        if (format.wBitsPerSample != 16 || format.nChannels > 2) return E_INVALIDARG;
        impl_->module = LoadLame();
        if (!HasLameApi(impl_->module)) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        const auto init = reinterpret_cast<Init>(GetProcAddress(impl_->module, "beInitStream"));
        impl_->encode = reinterpret_cast<Encode>(GetProcAddress(impl_->module, "beEncodeChunk"));
        impl_->flush = reinterpret_cast<Flush>(GetProcAddress(impl_->module, "beDeinitStream"));
        impl_->close = reinterpret_cast<Close>(GetProcAddress(impl_->module, "beCloseStream"));
        impl_->info_tag = reinterpret_cast<InfoTag>(GetProcAddress(impl_->module, "beWriteInfoTag"));
        BladeConfig config;
        config.rate = format.nSamplesPerSec;
        config.mode = format.nChannels == 1 ? 3 : 1;
        config.bitrate = std::clamp(options.bitrate, 8, 320);
        config.vbr = options.mode != 0;
        config.vbr_quality = std::clamp(options.quality, 0, 9);
        config.vbr_method = options.mode == 2 ? 4 : 0;
        config.abr_bps = options.mode == 2 ? config.bitrate * 1000U : 0;
        DWORD output_size{};
        const HRESULT result = BladeError(init(&config, &impl_->samples_per_chunk,
                                               &output_size, &impl_->stream));
        if (FAILED(result)) return result;
        if (!impl_->stream || !impl_->samples_per_chunk ||
            impl_->samples_per_chunk > 1024U * 1024U ||
            output_size < 7200U || output_size > 16U * 1024U * 1024U)
            return E_UNEXPECTED;
        impl_->output.resize(output_size);
    }
    impl_->file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    return lame ? S_OK : impl_->WaveHeader();
}

HRESULT FileEncoder::Write(std::span<const std::byte> pcm) {
    if (impl_->finished || impl_->file == INVALID_HANDLE_VALUE) return E_UNEXPECTED;
    if (pcm.size() % impl_->format.nBlockAlign) return E_INVALIDARG;
    if (!impl_->module) {
        if (impl_->bytes + pcm.size() > MAXDWORD - 36U)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        const HRESULT result = impl_->WriteBytes(pcm.data(), pcm.size());
        if (SUCCEEDED(result)) impl_->bytes += pcm.size();
        return result;
    }
    const size_t prior = impl_->pending.size();
    impl_->pending.resize(prior + pcm.size()/sizeof(short));
    std::memcpy(impl_->pending.data()+prior, pcm.data(), pcm.size());
    size_t offset{};
    while (impl_->pending.size()-offset >= impl_->samples_per_chunk) {
        const HRESULT result = impl_->EncodeChunk(impl_->pending.data()+offset,
                                                  impl_->samples_per_chunk);
        if (FAILED(result)) return result;
        offset += impl_->samples_per_chunk;
    }
    impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin()+offset);
    return S_OK;
}

HRESULT FileEncoder::Finalize() {
    if (impl_->finished || impl_->file == INVALID_HANDLE_VALUE) return E_UNEXPECTED;
    impl_->finished = true;
    HRESULT result = S_OK;
    if (!impl_->module) result = impl_->WaveHeader();
    else {
        if (!impl_->pending.empty())
            result = impl_->EncodeChunk(impl_->pending.data(),
                                        static_cast<DWORD>(impl_->pending.size()));
        DWORD size{};
        const HRESULT flushed = BladeError(impl_->flush(impl_->stream,
                                                       impl_->output.data(), &size));
        if (SUCCEEDED(result)) result = flushed;
        if (SUCCEEDED(result)) result = size <= impl_->output.size()
            ? impl_->WriteBytes(impl_->output.data(), size) : E_UNEXPECTED;
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(impl_->file))
        result = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(impl_->file);
    impl_->file = INVALID_HANDLE_VALUE;
    if (impl_->module && SUCCEEDED(result)) {
        // The Blade info-tag entry takes an ANSI path. A verified short path
        // avoids corrupting non-ASCII names; never use lossy '?' conversion.
        std::wstring path = impl_->path.wstring();
        std::wstring short_path(32768, L'\0');
        const DWORD length = GetShortPathNameW(path.c_str(), short_path.data(),
                                               static_cast<DWORD>(short_path.size()));
        if (length && length < short_path.size()) path = short_path.substr(0,length);
        BOOL substituted{};
        const DWORD flags=GetACP()==CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        BOOL* used_default=GetACP()==CP_UTF8 ? nullptr : &substituted;
        const int count = WideCharToMultiByte(CP_ACP, flags,
            path.c_str(), -1, nullptr, 0, nullptr, used_default);
        std::string ansi(count > 0 ? count : 0, '\0');
        if (!count || !WideCharToMultiByte(CP_ACP, flags,
                path.c_str(), -1, ansi.data(), count, nullptr, used_default) || substituted)
            result = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
        else {
            result = BladeError(impl_->info_tag(impl_->stream, ansi.c_str()));
            // beWriteInfoTag closes its stream, including on a write error.
            impl_->stream = nullptr;
        }
    }
    return result;
}
} // namespace ttplayer::audio
