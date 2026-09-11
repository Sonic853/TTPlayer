#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/core/text.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <objbase.h>
#include <objidl.h>
#include <shlwapi.h>
#include <system_error>

namespace ttplayer::plugins {
namespace {

// DAT_00518848..DAT_00518878, consumed by FUN_004CABC0 after each
// IPlayerSoundAddIn::EnumInterface call. The first category is the reader
// creator registry used later by FUN_004CB1DB to build the file filter.
constexpr GUID kReaderCreatorCategory{
    0x476d15a5, 0xd863, 0x416a, {0x8a, 0x59, 0xa9, 0xc7, 0xd7, 0x2c, 0xe0, 0x4e}};
// RTTI in the shipped add-ins names these IDecoderCreator and
// IEncoderCreator.  The first reconstruction's writer/processor/encoder
// labels were placeholders and left every non-reader category non-executable.
constexpr GUID kDecoderCreatorCategory{
    0x686d6670, 0xfdaf, 0x4fbd, {0xa8, 0x1a, 0xfb, 0x05, 0xd6, 0xf9, 0xef, 0xb1}};
constexpr GUID kEncoderCreatorCategory{
    0x5954a909, 0x9e02, 0x4650, {0x99, 0x5e, 0xf7, 0x53, 0x5e, 0xe2, 0xbd, 0x8b}};
constexpr GUID kLyricSearchCategory{
    0x9d5ae963, 0x7df6, 0x4323, {0xb4, 0xcf, 0xfb, 0xda, 0x15, 0x9b, 0xff, 0x15}};
constexpr std::array<const GUID*, 4> kRecognizedCategories{
    &kReaderCreatorCategory, &kDecoderCreatorCategory,
    &kEncoderCreatorCategory, &kLyricSearchCategory};

using GetSoundAddIn = HRESULT(WINAPI*)(void**);
using EnumInterface = HRESULT(WINAPI*)(void*, DWORD, GUID*, void**);
using AddRefInterface = ULONG(WINAPI*)(void*);
using ReleaseInterface = ULONG(WINAPI*)(void*);
using GetInterfaceString = HRESULT(WINAPI*)(void*, wchar_t**);
using CreateReader = HRESULT(WINAPI*)(void*, void**);
using SupportsSubtype = HRESULT(WINAPI*)(void*, const GUID*);
using InitializeDecoder = HRESULT(WINAPI*)(
    void*, const WAVEFORMATEX*, WAVEFORMATEX*);
using DecoderBufferSizes = HRESULT(WINAPI*)(void*, DWORD*, DWORD*);
using DecoderBufferMethod = HRESULT(WINAPI*)(void*, void*);
using ConfigureEncoder = HRESULT(WINAPI*)(void*, HWND);
using OpenEncoderPath = HRESULT(WINAPI*)(void*, const wchar_t*, WAVEFORMATEX*);
using OpenEncoderStream = HRESULT(WINAPI*)(void*, IStream*, WAVEFORMATEX*);
using EncodeSoundBuffer = HRESULT(WINAPI*)(void*, void*, DWORD*);
using InitializeLyricSearch = HRESULT(WINAPI*)(void*, void*, void*);
using NoArgumentMethod = HRESULT(WINAPI*)(void*);
using OpenReader = HRESULT(WINAPI*)(void*, IStream*, DWORD);
using GetReaderDword = HRESULT(WINAPI*)(void*, DWORD*);
using GetReaderFormat = HRESULT(WINAPI*)(void*, WAVEFORMATEX**);
using ReadReader = HRESULT(WINAPI*)(void*, void*);
using SeekReader = HRESULT(WINAPI*)(void*, DWORD*);
using QueryInterface = HRESULT(WINAPI*)(void*, REFIID, void**);
using MetadataGetCount = HRESULT(WINAPI*)(void*, DWORD*);
using MetadataGetAt = HRESULT(WINAPI*)(void*, DWORD, wchar_t**, wchar_t**);
using MetadataGetValue = HRESULT(WINAPI*)(void*, const char*, wchar_t**);
using MetadataSetValue = HRESULT(WINAPI*)(void*, const char*, const wchar_t*);
using ThumbnailGetDword = HRESULT(WINAPI*)(void*, DWORD*);
using ThumbnailGetAt = HRESULT(WINAPI*)(void*, DWORD, const void**);
using ThumbnailAdd = HRESULT(WINAPI*)(void*, const void*);
using ThumbnailRemove = HRESULT(WINAPI*)(void*, DWORD);

// DAT_005187C8. Every shipped reader creator (including APE, TAK and VQF)
// constructs its private object and queries this IID before returning slot 3.
[[maybe_unused]] constexpr GUID kSoundReaderInterface{
    0x30c7c165, 0xc0a9, 0x4204, {0x99, 0x5d, 0x45, 0x6a, 0x76, 0x49, 0x98, 0xfb}};

// DAT_0051AB04. FUN_004139B5 queries this interface from every opened
// reader. Slots 3/4/5/6 are GetCount/GetAt/GetValue/SetValue; strings returned
// by slots 4 and 5 are owned by the caller and released with CoTaskMemFree.
constexpr GUID kSoundMetadataInterface{
    0x7ad84e00, 0x5fef, 0x4481, {0xb5, 0x32, 0xfb, 0xbd, 0x67, 0x7e, 0x67, 0xc2}};

// GUID bytes at 0x00520600 (duplicated at 0x005187B8), named
// ISoundThumbnail by the shipped reader RTTI.
constexpr GUID kSoundThumbnailInterface{
    0xb5e770af, 0xdfb0, 0x43e5, {0x9b, 0x0c, 0x3e, 0xe9, 0x8e, 0x7b, 0x62, 0x48}};

// DAT_00522EF8, queried by FUN_004CD30C.  Encoders exposing this extension
// receive an IStream through slot 8; older encoders receive a path through
// their ordinary slot 3.
constexpr GUID kStreamEncoderInterface{
    0xffb27df8, 0x77ef, 0x4ba7,
    {0x89, 0x45, 0x7e, 0x14, 0x4c, 0x38, 0x1c, 0xbe}};

#pragma pack(push, 4)
struct LegacyThumbnailEntry {
    DWORD size;
    const wchar_t* mime;
    const wchar_t* description;
    DWORD data_size;
    const BYTE* data;
    DWORD picture_type;
};
#pragma pack(pop)
#if defined(_WIN32) && !defined(_WIN64)
static_assert(sizeof(LegacyThumbnailEntry) == 0x18);
#endif

struct LegacyBuffer;
using BufferQueryInterface = HRESULT(WINAPI*)(LegacyBuffer*, REFIID, void**);
using BufferAddRef = ULONG(WINAPI*)(LegacyBuffer*);
using BufferRelease = ULONG(WINAPI*)(LegacyBuffer*);
using BufferSetLength = HRESULT(WINAPI*)(LegacyBuffer*, DWORD);
using BufferGetCapacity = HRESULT(WINAPI*)(LegacyBuffer*, DWORD*);
using BufferGetData = HRESULT(WINAPI*)(LegacyBuffer*, BYTE**, DWORD*);

struct LegacyBufferVtable {
    BufferQueryInterface query_interface;
    BufferAddRef add_ref;
    BufferRelease release;
    BufferSetLength set_length;
    BufferGetCapacity get_capacity;
    BufferGetData get_data;
};

struct LegacyBuffer {
    const LegacyBufferVtable* vtable{};
    LONG references{1};
    BYTE* bytes{};
    DWORD capacity{};
    DWORD length{};
};

// The original exported CreateStreamOnFile (004C51D3) returns an IStream
// whose Stat name is the physical path.  SHCreateStreamOnFileEx leaves that
// field empty on this host; the FLAC metadata implementation consequently
// cannot run GetFileAttributesW and reports capabilities 0x2 instead of 0x6,
// making metadata slot 6 return E_ACCESSDENIED.  Preserve the ordinary shell
// stream behavior while restoring the named-Stat contract used by the add-ins.
class NamedFileStream final : public IStream {
public:
    NamedFileStream(IStream* inner, std::filesystem::path path,
                    DWORD mode) noexcept
        : inner_(inner), path_(std::move(path)), mode_(mode) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (InlineIsEqualGUID(iid, IID_IUnknown) ||
            InlineIsEqualGUID(iid, IID_ISequentialStream) ||
            InlineIsEqualGUID(iid, IID_IStream)) {
            *value = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(iid, value) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = static_cast<ULONG>(
            InterlockedDecrement(&references_));
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Read(void* value, ULONG size,
                                   ULONG* read) override {
        return inner_->Read(value, size, read);
    }
    HRESULT STDMETHODCALLTYPE Write(const void* value, ULONG size,
                                    ULONG* written) override {
        return inner_->Write(value, size, written);
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* position) override {
        return inner_->Seek(move, origin, position);
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        return inner_->SetSize(size);
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER size,
        ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        return inner_->CopyTo(target, size, read, written);
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override {
        return inner_->Commit(flags);
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return inner_->Revert(); }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset,
        ULARGE_INTEGER size, DWORD type) override {
        return inner_->LockRegion(offset, size, type);
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset,
        ULARGE_INTEGER size, DWORD type) override {
        return inner_->UnlockRegion(offset, size, type);
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        const HRESULT result = inner_->Stat(stat, flags);
        if (FAILED(result)) return result;
        stat->grfMode = mode_;
        if ((flags & STATFLAG_NONAME) == 0) {
            if (stat->pwcsName) CoTaskMemFree(stat->pwcsName);
            stat->pwcsName = nullptr;
            const auto& value = path_.native();
            const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
            stat->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
            if (!stat->pwcsName) return E_OUTOFMEMORY;
            std::memcpy(stat->pwcsName, value.c_str(), bytes);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        IStream* clone{};
        const HRESULT result = inner_->Clone(&clone);
        if (FAILED(result) || !clone) return FAILED(result) ? result : E_FAIL;
        auto* named = new (std::nothrow) NamedFileStream(clone, path_, mode_);
        if (!named) {
            clone->Release();
            return E_OUTOFMEMORY;
        }
        *value = named;
        return S_OK;
    }

private:
    ~NamedFileStream() {
        if (inner_) inner_->Release();
    }
    LONG references_{1};
    IStream* inner_{};
    std::filesystem::path path_;
    DWORD mode_{};
};

HRESULT CreateNamedFileStream(const std::filesystem::path& path, DWORD mode,
                              IStream** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    IStream* inner{};
    const HRESULT opened = SHCreateStreamOnFileEx(
        path.c_str(), mode, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &inner);
    if (FAILED(opened) || !inner)
        return FAILED(opened) ? opened : E_NOINTERFACE;
    try {
        auto* named = new (std::nothrow) NamedFileStream(inner, path, mode);
        if (!named) {
            inner->Release();
            return E_OUTOFMEMORY;
        }
        *output = named;
        return S_OK;
    } catch (...) {
        inner->Release();
        return E_OUTOFMEMORY;
    }
}

HRESULT WINAPI BufferQuery(LegacyBuffer* self, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!self) return E_POINTER;
    // None of the recovered APE/TAK/VQF Read methods queries the buffer, but
    // preserving IUnknown makes the host object valid for readers that do.
    if (!InlineIsEqualGUID(iid, IID_IUnknown)) return E_NOINTERFACE;
    *result = self;
    InterlockedIncrement(&self->references);
    return S_OK;
}

ULONG WINAPI BufferRetain(LegacyBuffer* self) {
    return self ? static_cast<ULONG>(InterlockedIncrement(&self->references)) : 0;
}

ULONG WINAPI BufferDrop(LegacyBuffer* self) {
    if (!self) return 0;
    // The object is owned by the synchronous Read call. A conforming reader
    // balances any temporary QueryInterface/AddRef before returning.
    return static_cast<ULONG>(InterlockedDecrement(&self->references));
}

HRESULT WINAPI BufferSetValidLength(LegacyBuffer* self, DWORD length) {
    if (!self) return E_POINTER;
    if (length > self->capacity) return E_INVALIDARG;
    self->length = length;
    return S_OK;
}

HRESULT WINAPI BufferCapacity(LegacyBuffer* self, DWORD* capacity) {
    if (!self || !capacity) return E_POINTER;
    *capacity = self->capacity;
    return S_OK;
}

HRESULT WINAPI BufferData(LegacyBuffer* self, BYTE** bytes, DWORD* length) {
    if (!self || !bytes || !length) return E_POINTER;
    *bytes = self->bytes;
    *length = self->length;
    return S_OK;
}

constexpr LegacyBufferVtable kLegacyBufferVtable{
    &BufferQuery, &BufferRetain, &BufferDrop, &BufferSetValidLength,
    &BufferCapacity, &BufferData};

void** Vtable(void* object) noexcept {
    return object ? *static_cast<void***>(object) : nullptr;
}

HRESULT InvokeFactory(FARPROC entry, void** result) noexcept {
    if (!entry || !result) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<GetSoundAddIn>(entry)(result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *result = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<GetSoundAddIn>(entry)(result);
#endif
}

HRESULT InvokeEnum(void* addin, DWORD index, GUID* category, void** provider) noexcept {
    auto table = Vtable(addin);
    if (!table || !table[3] || !category || !provider) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<EnumInterface>(table[3])(
            addin, index, category, provider);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *provider = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<EnumInterface>(table[3])(addin, index, category, provider);
#endif
}

HRESULT InvokeInterfaceString(void* provider, size_t slot,
                              wchar_t** description) noexcept {
    auto table = Vtable(provider);
    if (!table || slot >= 16 || !table[slot] || !description) return E_POINTER;
    *description = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<GetInterfaceString>(table[slot])(
            provider, description);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *description = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<GetInterfaceString>(table[slot])(
        provider, description);
#endif
}

HRESULT InvokeDescription(void* provider, wchar_t** description) noexcept {
    return InvokeInterfaceString(provider, 5, description);
}

HRESULT InvokeReaderCodecName(void* reader, wchar_t** description) noexcept {
    auto table = Vtable(reader);
    if (!table || !table[8] || !description) return E_POINTER;
    *description = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<GetInterfaceString>(table[8])(reader, description);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *description = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<GetInterfaceString>(table[8])(reader, description);
#endif
}

HRESULT InvokeCreateReader(void* provider, void** reader) noexcept {
    auto table = Vtable(provider);
    if (!table || !table[3] || !reader) return E_POINTER;
    *reader = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<CreateReader>(table[3])(provider, reader);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *reader = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<CreateReader>(table[3])(provider, reader);
#endif
}

HRESULT InvokeSubtypeSupport(void* provider, const GUID* subtype) noexcept {
    auto table = Vtable(provider);
    if (!table || !table[5] || !subtype) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<SupportsSubtype>(table[5])(provider, subtype);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<SupportsSubtype>(table[5])(provider, subtype);
#endif
}

HRESULT InvokeDecoderInitialize(void* decoder,
                                const WAVEFORMATEX* encoded_format,
                                WAVEFORMATEX* output_format) noexcept {
    auto table = Vtable(decoder);
    if (!table || !table[3] || !encoded_format || !output_format)
        return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<InitializeDecoder>(table[3])(
            decoder, encoded_format, output_format);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<InitializeDecoder>(table[3])(
        decoder, encoded_format, output_format);
#endif
}

HRESULT InvokeDecoderBufferSizes(void* decoder, DWORD* input_bytes,
                                 DWORD* output_bytes) noexcept {
    auto table = Vtable(decoder);
    if (!table || !table[4] || !input_bytes || !output_bytes) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<DecoderBufferSizes>(table[4])(
            decoder, input_bytes, output_bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<DecoderBufferSizes>(table[4])(
        decoder, input_bytes, output_bytes);
#endif
}

HRESULT InvokeDecoderBuffer(void* decoder, size_t slot,
                            LegacyBuffer* buffer) noexcept {
    auto table = Vtable(decoder);
    if (!table || slot < 7 || slot > 8 || !table[slot] || !buffer)
        return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<DecoderBufferMethod>(table[slot])(
            decoder, buffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<DecoderBufferMethod>(table[slot])(
        decoder, buffer);
#endif
}

HRESULT InvokeNoArgument(void* object, size_t slot) noexcept {
    auto table = Vtable(object);
    if (!table || slot >= 16 || !table[slot]) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<NoArgumentMethod>(table[slot])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<NoArgumentMethod>(table[slot])(object);
#endif
}

HRESULT InvokeEncoderConfigure(void* creator, HWND parent) noexcept {
    auto table = Vtable(creator);
    if (!table || !table[7]) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ConfigureEncoder>(table[7])(creator, parent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ConfigureEncoder>(table[7])(creator, parent);
#endif
}

HRESULT InvokeEncoderPathOpen(void* encoder, const wchar_t* path,
                              WAVEFORMATEX* format) noexcept {
    auto table = Vtable(encoder);
    if (!table || !table[3] || !path) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<OpenEncoderPath>(table[3])(
            encoder, path, format);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<OpenEncoderPath>(table[3])(
        encoder, path, format);
#endif
}

HRESULT InvokeEncoderStreamOpen(void* encoder, IStream* stream,
                                WAVEFORMATEX* format) noexcept {
    auto table = Vtable(encoder);
    if (!table || !table[8] || !stream) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<OpenEncoderStream>(table[8])(
            encoder, stream, format);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<OpenEncoderStream>(table[8])(
        encoder, stream, format);
#endif
}

HRESULT InvokeEncoderWrite(void* encoder, LegacyBuffer* buffer,
                           DWORD* encoded_bytes) noexcept {
    auto table = Vtable(encoder);
    if (!table || !table[6] || !buffer || !encoded_bytes) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<EncodeSoundBuffer>(table[6])(
            encoder, buffer, encoded_bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *encoded_bytes = 0;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<EncodeSoundBuffer>(table[6])(
        encoder, buffer, encoded_bytes);
#endif
}

HRESULT InvokeLyricSearchInitialize(void* search, void* context,
                                    void* sound_library_host) noexcept {
    auto table = Vtable(search);
    if (!table || !table[3]) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<InitializeLyricSearch>(table[3])(
            search, context, sound_library_host);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<InitializeLyricSearch>(table[3])(
        search, context, sound_library_host);
#endif
}

HRESULT InvokeReaderOpen(void* reader, IStream* stream, DWORD flags) noexcept {
    auto table = Vtable(reader);
    if (!table || !table[3] || !stream) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<OpenReader>(table[3])(reader, stream, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<OpenReader>(table[3])(reader, stream, flags);
#endif
}

HRESULT InvokeReaderDword(void* reader, size_t slot, DWORD* value) noexcept {
    auto table = Vtable(reader);
    if (!table || slot >= 16 || !table[slot] || !value) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<GetReaderDword>(table[slot])(reader, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<GetReaderDword>(table[slot])(reader, value);
#endif
}

HRESULT InvokeReaderFormat(void* reader, WAVEFORMATEX** format) noexcept {
    auto table = Vtable(reader);
    if (!table || !table[6] || !format) return E_POINTER;
    *format = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<GetReaderFormat>(table[6])(reader, format);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *format = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<GetReaderFormat>(table[6])(reader, format);
#endif
}

HRESULT InvokeReaderRead(void* reader, LegacyBuffer* buffer) noexcept {
    auto table = Vtable(reader);
    if (!table || !table[14] || !buffer) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ReadReader>(table[14])(reader, buffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ReadReader>(table[14])(reader, buffer);
#endif
}

HRESULT InvokeReaderSeek(void* reader, DWORD* position) noexcept {
    auto table = Vtable(reader);
    if (!table || !table[15] || !position) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<SeekReader>(table[15])(reader, position);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<SeekReader>(table[15])(reader, position);
#endif
}

HRESULT InvokeQueryInterface(void* object, REFIID iid, void** result) noexcept {
    auto table = Vtable(object);
    if (!table || !table[0] || !result) return E_POINTER;
    *result = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<QueryInterface>(table[0])(object, iid, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *result = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<QueryInterface>(table[0])(object, iid, result);
#endif
}

HRESULT InvokeMetadataCount(void* metadata, DWORD* count) noexcept {
    auto table = Vtable(metadata);
    if (!table || !table[3] || !count) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<MetadataGetCount>(table[3])(metadata, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
#else
    return reinterpret_cast<MetadataGetCount>(table[3])(metadata, count);
#endif
}

HRESULT InvokeMetadataAt(void* metadata, DWORD index, wchar_t** name,
                         wchar_t** value) noexcept {
    auto table = Vtable(metadata);
    if (!table || !table[4] || !name || !value) return E_POINTER;
    *name = nullptr;
    *value = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<MetadataGetAt>(table[4])(
            metadata, index, name, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *name = nullptr;
        *value = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<MetadataGetAt>(table[4])(metadata, index, name, value);
#endif
}

HRESULT InvokeMetadataValue(void* metadata, const char* name,
                            wchar_t** value) noexcept {
    auto table = Vtable(metadata);
    if (!table || !table[5] || !name || !value) return E_POINTER;
    *value = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<MetadataGetValue>(table[5])(
            metadata, name, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<MetadataGetValue>(table[5])(metadata, name, value);
#endif
}

HRESULT InvokeMetadataSet(void* metadata, const char* name,
                          const wchar_t* value) noexcept {
    auto table = Vtable(metadata);
    if (!table || !table[6] || !name || !value) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<MetadataSetValue>(table[6])(
            metadata, name, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return E_UNEXPECTED; }
#else
    return reinterpret_cast<MetadataSetValue>(table[6])(metadata, name, value);
#endif
}

HRESULT InvokeThumbnailAt(void* thumbnail, DWORD index,
                          const LegacyThumbnailEntry** entry) noexcept {
    auto table = Vtable(thumbnail);
    if (!table || !table[6] || !entry) return E_POINTER;
    *entry = nullptr;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ThumbnailGetAt>(table[6])(
            thumbnail, index, reinterpret_cast<const void**>(entry));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *entry = nullptr;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ThumbnailGetAt>(table[6])(
        thumbnail, index, reinterpret_cast<const void**>(entry));
#endif
}

HRESULT InvokeThumbnailDword(void* thumbnail, size_t slot,
                             DWORD* value) noexcept {
    auto table = Vtable(thumbnail);
    if (!table || slot < 3 || slot > 5 || !table[slot] || !value)
        return E_POINTER;
    *value = 0;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ThumbnailGetDword>(table[slot])(
            thumbnail, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *value = 0;
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ThumbnailGetDword>(table[slot])(
        thumbnail, value);
#endif
}

HRESULT InvokeThumbnailAdd(void* thumbnail,
                           const LegacyThumbnailEntry* entry) noexcept {
    auto table = Vtable(thumbnail);
    if (!table || !table[8] || !entry) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ThumbnailAdd>(table[8])(
            thumbnail, entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ThumbnailAdd>(table[8])(thumbnail, entry);
#endif
}

HRESULT InvokeThumbnailRemove(void* thumbnail, DWORD index) noexcept {
    auto table = Vtable(thumbnail);
    if (!table || !table[9]) return E_POINTER;
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<ThumbnailRemove>(table[9])(
            thumbnail, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
#else
    return reinterpret_cast<ThumbnailRemove>(table[9])(thumbnail, index);
#endif
}

struct ThumbnailSnapshot {
    DWORD data_size{};
    const BYTE* data{};
    wchar_t mime[32]{};
};

bool SnapshotThumbnail(const LegacyThumbnailEntry* entry,
                       ThumbnailSnapshot* snapshot) noexcept {
    if (!entry || !snapshot) return false;
#if defined(_MSC_VER)
    __try {
        snapshot->data_size = entry->data_size;
        snapshot->data = entry->data;
        if (entry->mime) {
            size_t index{};
            for (; index + 1 < std::size(snapshot->mime) &&
                   entry->mime[index] != L'\0';
                 ++index)
                snapshot->mime[index] = entry->mime[index];
            if (index + 1 == std::size(snapshot->mime) &&
                entry->mime[index] != L'\0')
                return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    snapshot->data_size = entry->data_size;
    snapshot->data = entry->data;
    if (entry->mime)
        wcsncpy_s(snapshot->mime, std::size(snapshot->mime),
                  entry->mime, _TRUNCATE);
#endif
    return true;
}

bool ThumbnailSignatureMatches(const ThumbnailSnapshot& snapshot) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        const auto* data = snapshot.data;
        const DWORD size = snapshot.data_size;
        return (size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
                data[size - 2] == 0xff && data[size - 1] == 0xd9) ||
               (size >= 2 && data[0] == 'B' && data[1] == 'M') ||
               (size >= 3 && data[0] == 'G' && data[1] == 'I' &&
                data[2] == 'F');
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool CopyThumbnailBytes(const ThumbnailSnapshot& snapshot,
                        unsigned char* destination) noexcept {
    if (!destination) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, snapshot.data, snapshot.data_size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, snapshot.data, snapshot.data_size);
    return true;
#endif
}

bool CopyThumbnail(const LegacyThumbnailEntry* entry,
                   std::vector<unsigned char>& bytes) noexcept {
    bytes.clear();
    ThumbnailSnapshot snapshot;
    if (!SnapshotThumbnail(entry, &snapshot)) return false;
    // FUN_004AD0AD performs a case-sensitive MIME gate.  Only an absent or
    // wildcard image MIME asks it to sniff the four legacy byte signatures.
    const std::wstring_view type(snapshot.mime);
    bool accepted = type == L"image/jpeg" || type == L"image/jpg" ||
                    type == L"image/bmp" || type == L"image/gif";
    const bool sniff = type.empty() || type == L"image/" ||
                       type == L"image/*";
    // FUN_004AD83E copies the reader-reported DWORD byte count without adding
    // a host-side 60000-byte ceiling.  The shipped readers impose their own
    // parser limits; allocation failure is handled below as in malloc-null.
    if (snapshot.data_size == 0 || !snapshot.data) return false;
    if (sniff) accepted = ThumbnailSignatureMatches(snapshot);
    if (!accepted) return false;
    try {
        bytes.resize(snapshot.data_size);
    } catch (...) {
        bytes.clear();
        return false;
    }
    if (!CopyThumbnailBytes(snapshot, bytes.data())) {
        bytes.clear();
        return false;
    }
    return true;
}

bool ReadWaveExtraSize(const WAVEFORMATEX* source, WORD* extra) noexcept {
    if (!source || !extra) return false;
#if defined(_MSC_VER)
    __try {
        *extra = source->cbSize;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *extra = source->cbSize;
    return true;
#endif
}

bool CopyWaveBytes(const WAVEFORMATEX* source, void* destination,
                   size_t bytes) noexcept {
    if (!source || !destination) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, bytes);
    return true;
#endif
}

bool CopyWaveFormat(const WAVEFORMATEX* source,
                    std::vector<std::byte>& destination) noexcept {
    destination.clear();
    WORD extra{};
    if (!ReadWaveExtraSize(source, &extra)) return false;
    constexpr size_t kMaximumWaveFormatBytes = 64U * 1024U;
    const size_t bytes = sizeof(WAVEFORMATEX) + extra;
    if (bytes > kMaximumWaveFormatBytes) return false;
    try {
        destination.resize(bytes);
    } catch (...) {
        return false;
    }
    if (!CopyWaveBytes(source, destination.data(), bytes)) {
        destination.clear();
        return false;
    }
    return true;
}

bool SafeWideLength(const wchar_t* value, size_t maximum, size_t* length) noexcept {
    if (!value || !length) return false;
#if defined(_MSC_VER)
    __try {
        size_t count{};
        while (count < maximum && value[count] != L'\0') ++count;
        if (count == maximum) return false;
        *length = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    const size_t count = wcsnlen(value, maximum);
    if (count == maximum) return false;
    *length = count;
    return true;
#endif
}

std::wstring ReadInterfaceString(void* object, size_t slot) {
    wchar_t* allocated{};
    std::wstring value;
    if (SUCCEEDED(InvokeInterfaceString(object, slot, &allocated)) && allocated) {
        size_t length{};
        if (SafeWideLength(allocated, 32768, &length))
            value.assign(allocated, length);
    }
    if (allocated) CoTaskMemFree(allocated);
    return value;
}

void Release(void* object) noexcept {
    auto table = Vtable(object);
    if (!table || !table[2]) return;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<ReleaseInterface>(table[2])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    reinterpret_cast<ReleaseInterface>(table[2])(object);
#endif
}

bool AddRef(void* object) noexcept {
    auto table = Vtable(object);
    if (!table || !table[1]) return false;
#if defined(_MSC_VER)
    __try {
        reinterpret_cast<AddRefInterface>(table[1])(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    reinterpret_cast<AddRefInterface>(table[1])(object);
    return true;
#endif
}

HMODULE RetainModule(HMODULE module) noexcept {
    if (!module) return nullptr;
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return nullptr;
    path.resize(length);
    return LoadLibraryW(path.c_str());
}

const GUID* CategoryInterface(const GUID& value) noexcept {
    const auto found = std::ranges::find_if(
        kRecognizedCategories, [&](const GUID* category) {
            return InlineIsEqualGUID(value, *category) != FALSE;
        });
    return found == kRecognizedCategories.end() ? nullptr : *found;
}

GUID WaveSubtype(const WAVEFORMATEX& format) noexcept {
    // FUN_004C8492 builds the standard wave-format GUID and replaces it with
    // WAVEFORMATEXTENSIBLE::SubFormat when cbSize is at least 22 bytes.
    GUID subtype{format.wFormatTag, 0x0000, 0x0010,
                 {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    if (format.wFormatTag == 0xfffe && format.cbSize >= 22)
        std::memcpy(&subtype,
                    reinterpret_cast<const std::byte*>(&format) + 24,
                    sizeof(subtype));
    return subtype;
}

bool IsDecodedPcmFormat(const WAVEFORMATEX& format) noexcept {
    // FUN_004B0947 accepts PCM and IEEE float, including their extensible
    // subtypes, and routes every other WAVE subtype through FUN_004CD045.
    const GUID subtype = WaveSubtype(format);
    constexpr GUID pcm{WAVE_FORMAT_PCM, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    constexpr GUID ieee_float{3, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return InlineIsEqualGUID(subtype, pcm) ||
           InlineIsEqualGUID(subtype, ieee_float);
}

std::wstring PatternFromDescription(std::wstring_view description) {
    // FUN_004CB18D extracts precisely the first parenthesized substring.
    const size_t begin = description.find(L'(');
    if (begin == std::wstring_view::npos) return {};
    const size_t end = description.find(L')', begin + 1);
    if (end == std::wstring_view::npos || end == begin + 1) return {};
    return std::wstring(description.substr(begin + 1, end - begin - 1));
}

std::string Win32ErrorText(DWORD error) {
    return "Win32 error " + std::to_string(error);
}

bool IsSoundAddInName(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    return name.size() > 8 && _wcsnicmp(name.c_str(), L"ttp_", 4) == 0 &&
           _wcsicmp(path.extension().c_str(), L".dll") == 0;
}

bool LessPath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
    auto a = left.filename().wstring();
    auto b = right.filename().wstring();
    std::ranges::transform(a, a.begin(), towlower);
    std::ranges::transform(b, b.begin(), towlower);
    return a < b;
}

bool PatternMatchesPath(std::wstring_view pattern,
                        const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    if (extension.empty()) return false;
    const std::wstring needle = L"*" + extension;
    size_t begin{};
    while (begin <= pattern.size()) {
        const size_t end = pattern.find(L';', begin);
        auto item = pattern.substr(begin, end == std::wstring_view::npos
            ? pattern.size() - begin : end - begin);
        while (!item.empty() && iswspace(item.front())) item.remove_prefix(1);
        while (!item.empty() && iswspace(item.back())) item.remove_suffix(1);
        if (_wcsicmp(std::wstring(item).c_str(), needle.c_str()) == 0 ||
            _wcsicmp(std::wstring(item).c_str(), L"*.*") == 0) return true;
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return false;
}

} // namespace

HRESULT CreateLegacyFileStream(const wchar_t* path, DWORD mode,
                              IStream** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!path || !*path) return E_INVALIDARG;
    try {
        return CreateNamedFileStream(std::filesystem::path(path), mode, output);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

struct PluginManager::EncoderDependencyState {
    std::filesystem::path directory;
    std::mutex mutex;
    std::vector<HMODULE> modules;

    explicit EncoderDependencyState(std::filesystem::path value)
        : directory(std::move(value)) { modules.reserve(3); }
    ~EncoderDependencyState() {
        for (auto it = modules.rbegin(); it != modules.rend(); ++it)
            FreeLibrary(*it);
    }
};

HRESULT PluginManager::PrepareEncoderDependencies(
    size_t module_index, std::wstring* diagnostic) const noexcept {
    if (module_index >= loaded_modules_.size()) return E_INVALIDARG;
    const auto state = loaded_modules_[module_index].encoder_dependencies;
    if (!state) return S_OK;
    try {
        std::lock_guard lock(state->mutex);
        // ttp_aac!60003CC7 loads Aac.dll by name; aacenc32 subsequently
        // delay-loads NeroIPP. Its Nero-6 registry fallback does not reliably
        // find a portable AddIn install. Resolve the existing local chain
        // before invoking the original creator (including its Configure UI).
        // Shared by retained library snapshots; unloaded after all add-in
        // interfaces/modules are released, never after a single conversion.
        constexpr const wchar_t* names[]{L"NeroIPP.dll", L"aacenc32.dll", L"Aac.dll"};
        for (size_t index = state->modules.size(); index < std::size(names); ++index) {
            const auto path = std::filesystem::absolute(state->directory / names[index]);
            const HMODULE module = LoadLibraryExW(
                path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module) {
                const DWORD error = GetLastError();
                if (diagnostic) *diagnostic = L"Unable to load Nero component: " + path.wstring();
                return HRESULT_FROM_WIN32(error ? error : ERROR_MOD_NOT_FOUND);
            }
            state->modules.push_back(module);
        }
        if (!GetProcAddress(state->modules.back(), "NERO_PLUGIN_GetPrimaryAudioObject")) {
            if (diagnostic) *diagnostic = L"Aac.dll has no NERO_PLUGIN_GetPrimaryAudioObject export";
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

struct LegacyReaderSession::Impl {
    void* reader{};
    void* metadata{};
    void* thumbnail_object{};
    std::unique_ptr<LegacyDecoderSession> decoder;
    std::vector<std::byte> encoded_format;
    // Keep the complete cbSize-qualified format alive.  A by-value
    // WAVEFORMATEX loses WAVEFORMATEXTENSIBLE's valid-bit count, channel mask
    // and SubFormat, while callers such as PcmOutputTransform intentionally
    // inspect those 22 trailing bytes (FUN_004C8492/FUN_004B0947).
    std::vector<std::byte> format;
    DWORD encoded_suggested_buffer_bytes{};
    DWORD duration_ms{};
    DWORD suggested_buffer_bytes{};
    DWORD encoded_bits_per_second{};
    DWORD capabilities{};
    std::wstring codec_name;
    std::filesystem::path module_path;
    std::vector<MetadataEntry> metadata_entries;
    std::vector<unsigned char> thumbnail;

    ~Impl() {
        // The decoder can still hold a temporary reference to the staging
        // buffer fed from this reader, so tear it down before its source.
        decoder.reset();
        if (thumbnail_object) Release(thumbnail_object);
        if (metadata) Release(metadata);
        if (reader) Release(reader);
    }
};

LegacyReaderSession::LegacyReaderSession(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LegacyReaderSession::~LegacyReaderSession() = default;

HRESULT LegacyReaderSession::Read(size_t requested_bytes,
                                  std::vector<std::byte>& output,
                                  bool& end_of_stream) noexcept {
    output.clear();
    end_of_stream = false;
    if (!impl_ || !impl_->reader) return E_UNEXPECTED;

    constexpr size_t maximum = 64U * 1024U * 1024U;
    if (impl_->decoder) {
        // FUN_004E3CF3 performs one state-machine turn: ask slot 5 whether
        // encoded input is required, fill the persistent input buffer through
        // reader slot 14 and submit it to decoder slot 7; then ask slot 6
        // whether PCM is available and drain it through slot 8.
        const HRESULT needs_input = impl_->decoder->NeedsInput();
        if (needs_input == S_OK) {
            DWORD decoder_input{}, decoder_output{};
            HRESULT result = impl_->decoder->BufferSizes(
                decoder_input, decoder_output);
            if (FAILED(result)) return result;
            size_t encoded_capacity = std::max<size_t>(
                impl_->encoded_suggested_buffer_bytes, decoder_input);
            encoded_capacity = std::clamp<size_t>(
                encoded_capacity, 1U, maximum);
            try {
                output.resize(encoded_capacity);
            } catch (...) {
                output.clear();
                return E_OUTOFMEMORY;
            }
            LegacyBuffer encoded_buffer{
                &kLegacyBufferVtable, 1,
                reinterpret_cast<BYTE*>(output.data()),
                static_cast<DWORD>(encoded_capacity), 0};
            result = InvokeReaderRead(impl_->reader, &encoded_buffer);
            if (result != S_OK) {
                output.clear();
                // ASF's shipped reader returns the positive status 0x26 on
                // exhaustion rather than S_FALSE.  FUN_004E3D60 treats every
                // non-zero result as terminal.
                end_of_stream = SUCCEEDED(result);
                return result;
            }
            if (encoded_buffer.length > encoded_buffer.capacity) {
                output.clear();
                return E_UNEXPECTED;
            }
            const auto encoded = std::span<const std::byte>(
                output.data(), encoded_buffer.length);
            result = impl_->decoder->PushInput(encoded);
            if (result != S_OK) {
                output.clear();
                return result;
            }
        } else if (FAILED(needs_input)) {
            return needs_input;
        }

        const HRESULT available = impl_->decoder->OutputAvailable();
        if (available == S_OK)
            return impl_->decoder->ReadOutput(
                requested_bytes, output, end_of_stream);
        output.clear();
        return FAILED(available) ? available : S_OK;
    }

    // The caller's capacity is a hard boundary, not merely a hint.  The
    // original CBuffer passed to reader slot 14 is sized by the active CUE
    // segment's remaining byte count; silently inflating a short final read
    // to the reader's preferred size consumes PCM belonging to the next
    // sub-track.  A zero request retains the creator's suggested size for
    // callers which explicitly ask for the default.
    const WAVEFORMATEX& format = Format();
    size_t capacity = requested_bytes != 0
        ? requested_bytes : impl_->suggested_buffer_bytes;
    capacity = std::clamp<size_t>(capacity, format.nBlockAlign, maximum);
    if (format.nBlockAlign > 1) {
        const size_t remainder = capacity % format.nBlockAlign;
        if (remainder != 0) capacity += format.nBlockAlign - remainder;
    }
    if (capacity > maximum || capacity > std::numeric_limits<DWORD>::max())
        return E_INVALIDARG;
    try {
        output.resize(capacity);
    } catch (...) {
        output.clear();
        return E_OUTOFMEMORY;
    }

    LegacyBuffer buffer{&kLegacyBufferVtable, 1,
        reinterpret_cast<BYTE*>(output.data()), static_cast<DWORD>(capacity), 0};
    const HRESULT result = InvokeReaderRead(impl_->reader, &buffer);
    if (FAILED(result)) {
        output.clear();
        return result;
    }
    if (buffer.length > buffer.capacity) {
        output.clear();
        return E_UNEXPECTED;
    }
    output.resize(buffer.length);
    // Reader slot 14 uses non-zero success codes as EOF for some shipped
    // sources (notably ASF/WMA returns 0x26).
    end_of_stream = result != S_OK;
    return result;
}

HRESULT LegacyReaderSession::Seek(DWORD& position_ms) noexcept {
    if (!impl_ || !impl_->reader) return E_UNEXPECTED;
    const HRESULT result = InvokeReaderSeek(impl_->reader, &position_ms);
    if (FAILED(result) || !impl_->decoder) return result;
    return impl_->decoder->Reset();
}

const WAVEFORMATEX& LegacyReaderSession::Format() const noexcept {
    static constexpr WAVEFORMATEX empty{};
    return impl_ && impl_->format.size() >= sizeof(WAVEFORMATEX)
        ? *reinterpret_cast<const WAVEFORMATEX*>(impl_->format.data())
        : empty;
}

DWORD LegacyReaderSession::DurationMilliseconds() const noexcept {
    return impl_ ? impl_->duration_ms : 0;
}

DWORD LegacyReaderSession::SuggestedBufferBytes() const noexcept {
    return impl_ ? impl_->suggested_buffer_bytes : 0;
}

DWORD LegacyReaderSession::EncodedBitsPerSecond() const noexcept {
    return impl_ ? impl_->encoded_bits_per_second : 0;
}

DWORD LegacyReaderSession::Capabilities() const noexcept {
    return impl_ ? impl_->capabilities : 0;
}

const std::wstring& LegacyReaderSession::CodecName() const noexcept {
    static const std::wstring empty;
    return impl_ ? impl_->codec_name : empty;
}

const std::filesystem::path& LegacyReaderSession::ModulePath() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->module_path : empty;
}

const std::vector<MetadataEntry>& LegacyReaderSession::Metadata() const noexcept {
    static const std::vector<MetadataEntry> empty;
    return impl_ ? impl_->metadata_entries : empty;
}

bool LegacyReaderSession::HasThumbnailInterface() const noexcept {
    return impl_ && impl_->thumbnail_object;
}

const std::vector<unsigned char>& LegacyReaderSession::Thumbnail() const noexcept {
    static const std::vector<unsigned char> empty;
    return impl_ ? impl_->thumbnail : empty;
}

HRESULT LegacyReaderSession::ReplaceThumbnails(
    std::span<const ThumbnailData> thumbnails) noexcept {
    if (!impl_ || !impl_->thumbnail_object) return E_NOINTERFACE;
    // Reader slot 4 capability bit 2 is the metadata-write flag; bit 4 is the
    // independent ISoundThumbnail write flag tested at 004AD696.
    if ((impl_->capabilities & 4U) == 0) return E_ACCESSDENIED;

    DWORD maximum_bytes{};
    HRESULT result = InvokeThumbnailDword(
        impl_->thumbnail_object, 4, &maximum_bytes);
    if (FAILED(result)) return result;
    DWORD maximum_items{};
    result = InvokeThumbnailDword(
        impl_->thumbnail_object, 5, &maximum_items);
    if (FAILED(result)) return result;
    if (thumbnails.size() > maximum_items) return E_INVALIDARG;
    for (const auto& thumbnail : thumbnails) {
        if (thumbnail.bytes.empty() ||
            thumbnail.bytes.size() > maximum_bytes ||
            thumbnail.bytes.size() > std::numeric_limits<DWORD>::max())
            return E_INVALIDARG;
    }

    DWORD current_count{};
    result = InvokeThumbnailDword(
        impl_->thumbnail_object, 3, &current_count);
    if (FAILED(result)) return result;
    // 004AD6E4 removes from count-1 down to zero so every index stays valid.
    while (current_count != 0) {
        result = InvokeThumbnailRemove(
            impl_->thumbnail_object, --current_count);
        if (FAILED(result)) return result;
    }

    for (const auto& thumbnail : thumbnails) {
        const LegacyThumbnailEntry entry{
            sizeof(LegacyThumbnailEntry),
            thumbnail.mime.c_str(), thumbnail.description.c_str(),
            static_cast<DWORD>(thumbnail.bytes.size()),
            thumbnail.bytes.data(), thumbnail.picture_type};
        result = InvokeThumbnailAdd(impl_->thumbnail_object, &entry);
        if (FAILED(result)) {
            // The original transaction is destructive and does not roll back
            // already-removed/added items when a private writer rejects one.
            impl_->thumbnail.clear();
            return result;
        }
    }
    try {
        impl_->thumbnail = thumbnails.empty()
            ? std::vector<unsigned char>{} : thumbnails.front().bytes;
    } catch (...) {
        // The add-in has already committed successfully.  Report cache
        // allocation failure without attempting an ABI-level rollback.
        impl_->thumbnail.clear();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT LegacyReaderSession::ClearThumbnails() noexcept {
    return ReplaceThumbnails({});
}

std::optional<std::wstring> LegacyReaderSession::MetadataValue(
    std::string_view name) const {
    if (!impl_ || !impl_->metadata || name.empty()) return std::nullopt;
    std::string terminated(name);
    wchar_t* allocated{};
    const HRESULT result = InvokeMetadataValue(
        impl_->metadata, terminated.c_str(), &allocated);
    if (FAILED(result) || !allocated) {
        if (allocated) CoTaskMemFree(allocated);
        return std::nullopt;
    }
    size_t length{};
    std::optional<std::wstring> value;
    if (SafeWideLength(allocated, 1024U * 1024U, &length))
        value.emplace(allocated, length);
    CoTaskMemFree(allocated);
    return value;
}

HRESULT LegacyReaderSession::SetMetadataValue(
    std::string_view name, std::wstring_view value) noexcept {
    if (!impl_ || !impl_->metadata) return E_NOINTERFACE;
    // Reader slot 4 capability bit 2 is the exact write guard used by
    // FUN_004ADD9C before metadata slot 6.
    if ((impl_->capabilities & 4U) == 0) return E_ACCESSDENIED;
    try {
        const std::string terminated_name(name);
        const std::wstring terminated_value(value);
        return InvokeMetadataSet(impl_->metadata, terminated_name.c_str(),
                                 terminated_value.c_str());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT LegacyReaderSession::SetMetadataValueDirect(
    std::string_view name, std::wstring_view value) noexcept {
    if (!impl_ || !impl_->metadata) return E_NOINTERFACE;
    try {
        const std::string terminated_name(name);
        const std::wstring terminated_value(value);
        return InvokeMetadataSet(impl_->metadata, terminated_name.c_str(),
                                 terminated_value.c_str());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

struct LegacyDecoderSession::Impl {
    void* decoder{};
    alignas(WAVEFORMATEX) std::array<std::byte, 0x28> output_format{};
    DWORD input_bytes{};
    DWORD output_bytes{};
    std::vector<std::byte> input_storage;
    // FUN_004E3CF3 passes the persistent CBuffer stored at source +0x60 to
    // decoder slot 7.  Shipped DMO decoders retain that object until their
    // pending output is drained; a stack-local wrapper becomes dangling even
    // though its byte storage remains allocated.
    LegacyBuffer input_buffer{&kLegacyBufferVtable, 1, nullptr, 0, 0};
    std::filesystem::path module_path;

    ~Impl() {
        if (decoder) Release(decoder);
    }
};

LegacyDecoderSession::LegacyDecoderSession(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LegacyDecoderSession::~LegacyDecoderSession() = default;

HRESULT LegacyDecoderSession::BufferSizes(
    DWORD& input_bytes, DWORD& output_bytes) const noexcept {
    input_bytes = 0;
    output_bytes = 0;
    if (!impl_ || !impl_->decoder) return E_UNEXPECTED;
    input_bytes = impl_->input_bytes;
    output_bytes = impl_->output_bytes;
    return S_OK;
}

HRESULT LegacyDecoderSession::NeedsInput() const noexcept {
    return !impl_ || !impl_->decoder
        ? E_UNEXPECTED : InvokeNoArgument(impl_->decoder, 5);
}

HRESULT LegacyDecoderSession::OutputAvailable() const noexcept {
    return !impl_ || !impl_->decoder
        ? E_UNEXPECTED : InvokeNoArgument(impl_->decoder, 6);
}

HRESULT LegacyDecoderSession::PushInput(
    std::span<const std::byte> encoded) noexcept {
    if (!impl_ || !impl_->decoder) return E_UNEXPECTED;
    if (encoded.empty()) return E_INVALIDARG;
    if (encoded.size() > std::numeric_limits<DWORD>::max())
        return E_INVALIDARG;
    try {
        impl_->input_storage.assign(encoded.begin(), encoded.end());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    impl_->input_buffer.bytes =
        reinterpret_cast<BYTE*>(impl_->input_storage.data());
    impl_->input_buffer.capacity =
        static_cast<DWORD>(impl_->input_storage.size());
    impl_->input_buffer.length =
        static_cast<DWORD>(impl_->input_storage.size());
    return InvokeDecoderBuffer(
        impl_->decoder, 7, &impl_->input_buffer);
}

HRESULT LegacyDecoderSession::ReadOutput(
    size_t requested_bytes, std::vector<std::byte>& pcm,
    bool& end_of_stream) noexcept {
    pcm.clear();
    end_of_stream = false;
    if (!impl_ || !impl_->decoder) return E_UNEXPECTED;
    constexpr size_t maximum = 64U * 1024U * 1024U;
    size_t capacity = requested_bytes != 0
        ? requested_bytes : impl_->output_bytes;
    const auto& output_format = *reinterpret_cast<const WAVEFORMATEX*>(
        impl_->output_format.data());
    capacity = std::clamp<size_t>(
        capacity, std::max<WORD>(output_format.nBlockAlign, 1),
        maximum);
    if (output_format.nBlockAlign > 1) {
        const size_t remainder = capacity % output_format.nBlockAlign;
        if (remainder != 0)
            capacity += output_format.nBlockAlign - remainder;
    }
    if (capacity > maximum || capacity > std::numeric_limits<DWORD>::max())
        return E_INVALIDARG;
    try {
        pcm.resize(capacity);
    } catch (...) {
        pcm.clear();
        return E_OUTOFMEMORY;
    }
    LegacyBuffer buffer{
        &kLegacyBufferVtable, 1, reinterpret_cast<BYTE*>(pcm.data()),
        static_cast<DWORD>(capacity), 0};
    const HRESULT result = InvokeDecoderBuffer(impl_->decoder, 8, &buffer);
    if (FAILED(result) || buffer.length > buffer.capacity) {
        pcm.clear();
        return FAILED(result) ? result : E_UNEXPECTED;
    }
    pcm.resize(buffer.length);
    end_of_stream = result != S_OK;
    return result;
}

HRESULT LegacyDecoderSession::Reset() noexcept {
    if (!impl_ || !impl_->decoder) return E_UNEXPECTED;
    const HRESULT result = InvokeNoArgument(impl_->decoder, 9);
    if (SUCCEEDED(result)) {
        impl_->input_storage.clear();
        impl_->input_buffer.bytes = nullptr;
        impl_->input_buffer.capacity = 0;
        impl_->input_buffer.length = 0;
    }
    return result;
}

HRESULT LegacyDecoderSession::QueryInterface(
    REFIID iid, void** result) const noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    return !impl_ || !impl_->decoder
        ? E_UNEXPECTED
        : InvokeQueryInterface(impl_->decoder, iid, result);
}

const WAVEFORMATEX& LegacyDecoderSession::OutputFormat() const noexcept {
    static constexpr WAVEFORMATEX empty{};
    return impl_ ? *reinterpret_cast<const WAVEFORMATEX*>(
                       impl_->output_format.data()) : empty;
}

const std::filesystem::path& LegacyDecoderSession::ModulePath() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->module_path : empty;
}

struct LegacyEncoderSession::Impl {
    void* encoder{};
    std::filesystem::path module_path;
    bool opened{};
    bool started{};

    ~Impl() {
        // 00412891 executes slot 5 on cancellation and encode failures as
        // long as slot 4 succeeded.  Preserve that guarantee for callers
        // unwinding a session without an explicit Finalize call.
        if (encoder && started) InvokeNoArgument(encoder, 5);
        if (encoder) Release(encoder);
    }
};

LegacyEncoderSession::LegacyEncoderSession(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LegacyEncoderSession::~LegacyEncoderSession() = default;

HRESULT LegacyEncoderSession::Open(const std::filesystem::path& destination,
                                   const WAVEFORMATEX& pcm_format) noexcept {
    if (!impl_ || !impl_->encoder) return E_UNEXPECTED;
    if (destination.empty()) return E_INVALIDARG;
    if (impl_->opened || impl_->started) return E_UNEXPECTED;

    // FUN_004CD30C first asks for the stream-aware extension.  Failure is not
    // fatal: old encoders implement the direct path overload on slot 3.
    void* stream_encoder{};
    const HRESULT query = InvokeQueryInterface(
        impl_->encoder, kStreamEncoderInterface, &stream_encoder);
    if (FAILED(query) || !stream_encoder) {
        if (stream_encoder) Release(stream_encoder);
        const HRESULT result = InvokeEncoderPathOpen(
            impl_->encoder, destination.c_str(),
            const_cast<WAVEFORMATEX*>(&pcm_format));
        if (SUCCEEDED(result)) impl_->opened = true;
        return result;
    }

    IStream* stream{};
    constexpr DWORD kEncoderStreamMode = STGM_READWRITE |
        STGM_SHARE_DENY_WRITE | STGM_CREATE; // literal 0x1022 in 004CD30C
    HRESULT result = CreateNamedFileStream(
        destination, kEncoderStreamMode, &stream);
    if (SUCCEEDED(result) && stream)
        result = InvokeEncoderStreamOpen(
            stream_encoder, stream, const_cast<WAVEFORMATEX*>(&pcm_format));
    if (stream) stream->Release();
    Release(stream_encoder);
    if (SUCCEEDED(result)) impl_->opened = true;
    return result;
}

HRESULT LegacyEncoderSession::Start() noexcept {
    if (!impl_ || !impl_->encoder) return E_UNEXPECTED;
    if (!impl_->opened || impl_->started) return E_UNEXPECTED;
    const HRESULT result = InvokeNoArgument(impl_->encoder, 4);
    if (SUCCEEDED(result)) impl_->started = true;
    return result;
}

HRESULT LegacyEncoderSession::WritePcm(
    std::span<const std::byte> pcm, DWORD* encoded_bytes) noexcept {
    if (encoded_bytes) *encoded_bytes = 0;
    if (!impl_ || !impl_->encoder || !impl_->started) return E_UNEXPECTED;
    if (pcm.size() > std::numeric_limits<DWORD>::max()) return E_INVALIDARG;
    if (pcm.empty()) return S_OK;

    DWORD local_encoded{};
    LegacyBuffer buffer{
        &kLegacyBufferVtable, 1,
        reinterpret_cast<BYTE*>(const_cast<std::byte*>(pcm.data())),
        static_cast<DWORD>(pcm.size()), static_cast<DWORD>(pcm.size())};
    const HRESULT result = InvokeEncoderWrite(
        impl_->encoder, &buffer,
        encoded_bytes ? encoded_bytes : &local_encoded);
    return result;
}

HRESULT LegacyEncoderSession::Finalize() noexcept {
    if (!impl_ || !impl_->encoder) return E_UNEXPECTED;
    if (!impl_->started) return S_FALSE;
    // Do not retry from Impl::~Impl after a failed finalization: the original
    // conversion worker calls slot 5 exactly once on every started encoder.
    impl_->started = false;
    return InvokeNoArgument(impl_->encoder, 5);
}

HRESULT LegacyEncoderSession::QueryInterface(
    REFIID iid, void** result) const noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    return !impl_ || !impl_->encoder
        ? E_UNEXPECTED
        : InvokeQueryInterface(impl_->encoder, iid, result);
}

std::wstring LegacyEncoderSession::FileExtension() const {
    return impl_ && impl_->encoder
        ? ReadInterfaceString(impl_->encoder, 7) : std::wstring{};
}

HRESULT LegacyEncoderSession::SetMetadata(
    std::span<const MetadataEntry> entries) noexcept {
    void* metadata{};
    HRESULT result = QueryInterface(kSoundMetadataInterface, &metadata);
    if (FAILED(result) || !metadata) return FAILED(result) ? result : E_NOINTERFACE;
    try {
        result = S_OK;
        for (const auto& entry : entries) {
            if (entry.name.empty() || entry.value.empty() ||
                _wcsnicmp(entry.name.c_str(), L"replaygain_", 11) == 0) continue;
            const auto name = core::WideToUtf8(entry.name);
            const HRESULT written = InvokeMetadataSet(
                metadata, name.c_str(), entry.value.c_str());
            if (FAILED(written) && SUCCEEDED(result)) result = written;
        }
    } catch (...) { result = E_OUTOFMEMORY; }
    Release(metadata);
    return result;
}

const std::filesystem::path& LegacyEncoderSession::ModulePath() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->module_path : empty;
}

struct LegacyLyricSearchSession::Impl {
    void* search{};
    std::filesystem::path module_path;

    ~Impl() {
        if (search) Release(search);
    }
};

LegacyLyricSearchSession::LegacyLyricSearchSession(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LegacyLyricSearchSession::~LegacyLyricSearchSession() = default;

HRESULT LegacyLyricSearchSession::QueryInterface(
    REFIID iid, void** result) const noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    return !impl_ || !impl_->search
        ? E_UNEXPECTED
        : InvokeQueryInterface(impl_->search, iid, result);
}

const std::filesystem::path&
LegacyLyricSearchSession::ModulePath() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->module_path : empty;
}

PluginManager::~PluginManager() { Shutdown(); }

std::shared_ptr<PluginManager>
PluginManager::RetainForBackground() const noexcept {
    std::shared_ptr<PluginManager> retained;
    try {
        retained = std::make_shared<PluginManager>();
        retained->plugins_ = plugins_;
        retained->reader_formats_ = reader_formats_;
        retained->decoder_factory_info_ = decoder_factory_info_;
        retained->encoder_factory_info_ = encoder_factory_info_;
        retained->lyric_provider_info_ = lyric_provider_info_;
        retained->loaded_modules_.reserve(loaded_modules_.size());
        retained->reader_factories_.reserve(reader_factories_.size());
        retained->decoder_factories_.reserve(decoder_factories_.size());
        retained->encoder_factories_.reserve(encoder_factories_.size());
        retained->lyric_providers_.reserve(lyric_providers_.size());
    } catch (...) {
        return {};
    }

    // Retain the module before AddRef: even a malformed add-in cannot leave a
    // newly retained interface pointing at code whose original DLL is about
    // to be unloaded by application teardown.
    for (const auto& loaded : loaded_modules_) {
        HMODULE module = RetainModule(loaded.module);
        if (!module) return {};
        if (loaded.addin && !AddRef(loaded.addin)) {
            FreeLibrary(module);
            return {};
        }
        try {
            retained->loaded_modules_.push_back(
                LoadedModule{module, loaded.addin, loaded.encoder_dependencies});
        } catch (...) {
            if (loaded.addin) Release(loaded.addin);
            FreeLibrary(module);
            return {};
        }
    }

    // FUN_004CD67C/79E/865/92C cache category interfaces lazily.  A retained
    // sound-library snapshot therefore copies registry identity but starts
    // with empty caches of its own; it never borrows a provider owned by the
    // primary manager.
    try {
        for (const auto& factory : reader_factories_)
            retained->reader_factories_.push_back(ReaderFactory{
                factory.format, factory.module_index,
                factory.enumeration_index, nullptr});
        for (const auto& factory : decoder_factories_)
            retained->decoder_factories_.push_back(DecoderFactory{
                factory.info, factory.module_index,
                factory.enumeration_index, nullptr});
        for (const auto& factory : encoder_factories_)
            retained->encoder_factories_.push_back(EncoderFactory{
                factory.info, factory.module_index,
                factory.enumeration_index, nullptr});
        for (const auto& provider : lyric_providers_)
            retained->lyric_providers_.push_back(LyricProvider{
                provider.info, provider.module_index,
                provider.enumeration_index, nullptr});
    } catch (...) {
        return {};
    }
    return retained;
}

HRESULT PluginManager::Load(const std::filesystem::path& directory) {
    Shutdown();
    if (directory.empty()) return S_OK;

    // FUN_004C8421 appends the AddIn directory to PATH before loading any
    // module. Several supplied add-ins resolve companion DLLs this way.
    const DWORD required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (required != 0) {
        std::wstring path_value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            L"PATH", path_value.data(), static_cast<DWORD>(path_value.size()));
        if (copied != 0 && copied < path_value.size()) {
            path_value.resize(copied);
            path_value.push_back(L';');
            path_value += directory.native();
            SetEnvironmentVariableW(L"PATH", path_value.c_str());
        }
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return S_OK;

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error) && !entry_error &&
            IsSoundAddInName(iterator->path())) {
            candidates.push_back(iterator->path());
        }
    }
    // FUN_004CABC0 treats FindFirst/FindNext failures as an empty add-in set;
    // a codec-directory scan failure does not abort the built-in sound library.
    if (error) return S_OK;
    std::ranges::sort(candidates, LessPath);

    for (const auto& path : candidates) {
        PluginInfo info;
        info.path = path;

        // CSoundAddInModule_CreateInstance (004C88D9) uses an ordinary
        // LoadLibrary call. LOAD_LIBRARY_AS_DATAFILE would incorrectly make
        // unloaded/invalid codecs appear supported merely because resources
        // can be read from their files.
        const HMODULE module = LoadLibraryW(path.c_str());
        if (!module) {
            const DWORD code = GetLastError();
            info.result = HRESULT_FROM_WIN32(code);
            info.error = Win32ErrorText(code);
            plugins_.push_back(std::move(info));
            continue;
        }

        const FARPROC factory = GetProcAddress(module, "ttpGetSoundAddIn");
        info.has_legacy_entry = factory != nullptr;
        if (!factory) {
            info.result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            info.error = "ttpGetSoundAddIn not found";
            FreeLibrary(module);
            plugins_.push_back(std::move(info));
            continue;
        }

        void* addin{};
        HRESULT result = InvokeFactory(factory, &addin);
        info.result = result;
        info.instance_created = SUCCEEDED(result) && addin != nullptr;
        if (!info.instance_created) {
            info.error = "ttpGetSoundAddIn failed";
            if (addin) Release(addin);
            FreeLibrary(module);
            plugins_.push_back(std::move(info));
            continue;
        }

        const size_t module_index = loaded_modules_.size();
        bool recognized{};
        for (DWORD index = 0;; ++index) {
            GUID category{};
            void* provider{};
            result = InvokeEnum(addin, index, &category, &provider);
            if (FAILED(result)) {
                if (provider) Release(provider);
                break;
            }

            const GUID* interface_id = CategoryInterface(category);
            recognized = recognized || interface_id != nullptr;

            // FUN_004CABC0 does not insert the raw object returned by
            // IPlayerSoundAddIn::EnumInterface into a sound registry.  Each
            // recognized branch first calls QueryInterface through
            // FUN_004CD58E/5B0/5D2/5F4 with the corresponding category IID.
            // Some add-ins enumerate a category for a newer/incompatible
            // provider; treating that raw pointer as the requested interface
            // makes a DLL's mere presence incorrectly advertise its formats.
            void* category_interface{};
            if (provider && interface_id) {
                const HRESULT query_result = InvokeQueryInterface(
                    provider, *interface_id, &category_interface);
                if (FAILED(query_result) && category_interface) {
                    Release(category_interface);
                    category_interface = nullptr;
                }
            }

            if (category_interface &&
                InlineIsEqualGUID(category, kReaderCreatorCategory)) {
                wchar_t* description{};
                if (SUCCEEDED(InvokeDescription(category_interface, &description)) &&
                    description) {
                    std::wstring text(description);
                    auto pattern = PatternFromDescription(text);
                    if (!text.empty() && !pattern.empty()) {
                        ReaderFormat format{
                            std::move(text), std::move(pattern), path};
                        reader_formats_.push_back(format);
                        reader_factories_.push_back(
                            ReaderFactory{std::move(format), module_index,
                                          index, nullptr});
                        ++info.reader_count;
                    }
                }
                if (description) CoTaskMemFree(description);
            }
            if (category_interface &&
                InlineIsEqualGUID(category, kDecoderCreatorCategory)) {
                DecoderFactoryInfo decoder{
                    ReadInterfaceString(category_interface, 4), path};
                decoder_factory_info_.push_back(decoder);
                decoder_factories_.push_back(DecoderFactory{
                    std::move(decoder), module_index, index, nullptr});
                ++info.decoder_creator_count;
            }
            if (category_interface &&
                InlineIsEqualGUID(category, kEncoderCreatorCategory)) {
                EncoderFactoryInfo encoder{
                    ReadInterfaceString(category_interface, 4),
                    ReadInterfaceString(category_interface, 5),
                    InvokeNoArgument(category_interface, 6) == S_OK, path};
                encoder_factory_info_.push_back(encoder);
                encoder_factories_.push_back(EncoderFactory{
                    std::move(encoder), module_index, index, nullptr});
                ++info.encoder_creator_count;
            }
            if (category_interface &&
                InlineIsEqualGUID(category, kLyricSearchCategory)) {
                LyricSearchProviderInfo lyric{
                    ReadInterfaceString(category_interface, 4), path};
                lyric_provider_info_.push_back(lyric);
                lyric_providers_.push_back(LyricProvider{
                    std::move(lyric), module_index, index, nullptr});
                ++info.lyric_search_provider_count;
            }
            if (category_interface)
                Release(category_interface);
            if (provider) Release(provider);
        }

        info.registered = recognized;
        if (recognized) {
            info.result = S_OK;
            std::shared_ptr<EncoderDependencyState> dependencies;
            if (_wcsicmp(path.filename().c_str(), L"ttp_aac.dll") == 0)
                dependencies = std::make_shared<EncoderDependencyState>(path.parent_path());
            loaded_modules_.push_back(LoadedModule{module, addin, std::move(dependencies)});
        } else {
            info.result = E_NOTIMPL;
            info.error = "no recognized sound interfaces";
            Release(addin);
            FreeLibrary(module);
        }
        plugins_.push_back(std::move(info));
    }
    return S_OK;
}

void* PluginManager::ResolveCategoryInterface(
    size_t module_index, DWORD enumeration_index,
    REFIID category, void*& cached) const noexcept {
    std::lock_guard lock(registry_mutex_);
    if (cached) {
        return AddRef(cached) ? cached : nullptr;
    }
    if (module_index >= loaded_modules_.size()) return nullptr;
    const auto& loaded = loaded_modules_[module_index];
    if (!loaded.addin) return nullptr;

    GUID enumerated_category{};
    void* enumerated{};
    HRESULT result = InvokeEnum(loaded.addin, enumeration_index,
                                &enumerated_category, &enumerated);
    if (FAILED(result) || !enumerated ||
        !InlineIsEqualGUID(enumerated_category, category)) {
        if (enumerated) Release(enumerated);
        return nullptr;
    }

    void* resolved{};
    result = InvokeQueryInterface(enumerated, category, &resolved);
    Release(enumerated);
    if (FAILED(result) || !resolved) {
        if (resolved) Release(resolved);
        return nullptr;
    }

    // One reference belongs to the registry cache; the second is returned to
    // the caller.  CSoundLibrary_Shutdown releases caches before DLL modules.
    cached = resolved;
    if (!AddRef(cached)) {
        Release(cached);
        cached = nullptr;
        return nullptr;
    }
    return cached;
}

bool PluginManager::HasReaderForPath(const std::filesystem::path& path) const {
    return std::ranges::any_of(reader_factories_, [&](const ReaderFactory& factory) {
        return PatternMatchesPath(factory.format.pattern, path);
    });
}

std::unique_ptr<LegacyReaderSession> PluginManager::OpenReader(
    const std::filesystem::path& path, HRESULT* result,
    std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (!HasReaderForPath(path)) {
        if (result) *result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (diagnostic) *diagnostic = L"no registered reader matched the extension";
        return {};
    }
    IStream* stream{};
    const HRESULT opened = CreateNamedFileStream(
        path, STGM_READ | STGM_SHARE_DENY_WRITE, &stream);
    if (FAILED(opened) || !stream) {
        if (result) *result = FAILED(opened) ? opened : E_NOINTERFACE;
        if (diagnostic) *diagnostic = L"creating file IStream";
        if (stream) stream->Release();
        return {};
    }
    auto session = OpenReader(path, stream, result, diagnostic);
    stream->Release();
    return session;
}

std::unique_ptr<LegacyReaderSession> PluginManager::OpenReaderForMetadata(
    const std::filesystem::path& path, HRESULT* result,
    std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (!HasReaderForPath(path)) {
        if (result) *result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (diagnostic)
            *diagnostic = L"no registered reader matched the extension";
        return {};
    }
    IStream* stream{};
    const HRESULT opened = CreateNamedFileStream(
        path, STGM_READWRITE | STGM_SHARE_DENY_WRITE, &stream);
    if (FAILED(opened) || !stream) {
        if (result) *result = FAILED(opened) ? opened : E_NOINTERFACE;
        if (diagnostic) *diagnostic = L"creating writable file IStream";
        if (stream) stream->Release();
        return {};
    }
    auto session = OpenReaderWithFlags(path, stream, 0, result, diagnostic);
    stream->Release();
    return session;
}

std::unique_ptr<LegacyReaderSession> PluginManager::OpenReader(
    const std::filesystem::path& logical_path, IStream* stream,
    HRESULT* result, std::wstring* diagnostic) const {
    // 004B0D80 passes 3 to FUN_004b0b51 -> FUN_004e323b for playback.
    // Bit 0 enables decoding; bit 1 requests IEEE float ((flags & 2) | 1).
    // The shipped ttp_aac MP4 decoder writes float samples even with tag 1,
    // so the integer request mislabels its samples and produces loud noise.
    // Metadata-only opens deliberately keep flags 0 in the separate path.
    return OpenReaderWithFlags(logical_path, stream, 3, result, diagnostic);
}

std::unique_ptr<LegacyReaderSession> PluginManager::OpenReaderWithFlags(
    const std::filesystem::path& logical_path, IStream* stream,
    DWORD open_flags, HRESULT* result, std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (!stream) {
        if (result) *result = E_POINTER;
        if (diagnostic) *diagnostic = L"reader input IStream is null";
        return {};
    }
    HRESULT last_result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    bool matched{};
    for (const auto& factory : reader_factories_) {
        if (!PatternMatchesPath(factory.format.pattern, logical_path)) continue;
        matched = true;

        void* creator = ResolveCategoryInterface(
            factory.module_index, factory.enumeration_index,
            kReaderCreatorCategory, factory.provider);
        if (!creator) {
            last_result = E_NOINTERFACE;
            if (diagnostic) *diagnostic = L"reader creator QueryInterface";
            continue;
        }
        void* reader{};
        HRESULT current = InvokeCreateReader(creator, &reader);
        Release(creator);
        if (FAILED(current) || !reader) {
            if (SUCCEEDED(current)) current = E_NOINTERFACE;
            if (reader) Release(reader);
            last_result = current;
            if (diagnostic) *diagnostic = L"reader creator slot 3";
            continue;
        }

        // A preceding creator may have inspected the same logical stream.
        // FUN_004E323B hands each decoder the archive member at offset zero.
        LARGE_INTEGER beginning{};
        current = stream->Seek(beginning, STREAM_SEEK_SET, nullptr);
        if (SUCCEEDED(current))
            current = InvokeReaderOpen(reader, stream, open_flags);
        if (FAILED(current)) {
            Release(reader);
            last_result = current;
            if (diagnostic) *diagnostic = L"reader slot 3 Open(IStream, flags)";
            continue;
        }

        WAVEFORMATEX* allocated_format{};
        current = InvokeReaderFormat(reader, &allocated_format);
        std::vector<std::byte> encoded_format;
        WAVEFORMATEX format{};
        const bool copied = SUCCEEDED(current) &&
            CopyWaveFormat(allocated_format, encoded_format);
        if (copied)
            std::memcpy(&format, encoded_format.data(), sizeof(format));
        if (allocated_format) CoTaskMemFree(allocated_format);
        if (!copied || format.nChannels == 0 || format.nSamplesPerSec == 0 ||
            format.nAvgBytesPerSec == 0 || format.nBlockAlign == 0) {
            Release(reader);
            last_result = FAILED(current) ? current : E_UNEXPECTED;
            if (diagnostic) *diagnostic = L"reader slot 6 GetFormat";
            continue;
        }

        DWORD suggested{};
        current = InvokeReaderDword(reader, 7, &suggested);
        if (FAILED(current)) {
            Release(reader);
            last_result = current;
            if (diagnostic) *diagnostic = L"reader slot 7 GetBufferSize";
            continue;
        }
        if (suggested == 0) {
            const uint64_t fallback = static_cast<uint64_t>(format.nBlockAlign) * 1024U;
            suggested = static_cast<DWORD>(std::min<uint64_t>(
                fallback, std::numeric_limits<DWORD>::max()));
        }
        const DWORD encoded_suggested_buffer_bytes = suggested;

        std::unique_ptr<LegacyDecoderSession> decoder;
        if ((open_flags & 1U) != 0 && !IsDecodedPcmFormat(
                *reinterpret_cast<const WAVEFORMATEX*>(
                    encoded_format.data()))) {
            // 004E3A3D allocates 0x28 bytes, requests PCM (or IEEE float when
            // open flag bit 1 is present) and lets decoder slot 3 fill the
            // negotiated output structure.
            decoder = OpenDecoder(
                *reinterpret_cast<const WAVEFORMATEX*>(encoded_format.data()),
                static_cast<WORD>((open_flags & 2U) | WAVE_FORMAT_PCM),
                &current, diagnostic);
            if (!decoder) {
                Release(reader);
                last_result = FAILED(current) ? current : E_NOINTERFACE;
                continue;
            }
            DWORD decoder_input{}, decoder_output{};
            current = decoder->BufferSizes(decoder_input, decoder_output);
            if (FAILED(current) || decoder_input == 0 || decoder_output == 0) {
                Release(reader);
                last_result = FAILED(current) ? current : E_UNEXPECTED;
                if (diagnostic)
                    *diagnostic = L"decoder slot 4 buffer sizes";
                continue;
            }
            const WAVEFORMATEX& decoded_format = decoder->OutputFormat();
            suggested = decoder_output;
            // The original decoder wrapper owns a fixed 0x28-byte output
            // structure.  Reject an impossible cbSize before copying it into
            // the reader session; otherwise a corrupt/private decoder could
            // make the host read beyond that recovered allocation.
            const size_t decoded_format_bytes =
                sizeof(WAVEFORMATEX) + decoded_format.cbSize;
            if (decoded_format_bytes > 0x28 ||
                !IsDecodedPcmFormat(decoded_format) ||
                decoded_format.nChannels == 0 ||
                decoded_format.nSamplesPerSec == 0 ||
                decoded_format.nAvgBytesPerSec == 0 ||
                decoded_format.nBlockAlign == 0) {
                Release(reader);
                last_result = E_UNEXPECTED;
                if (diagnostic)
                    *diagnostic = L"decoder returned an invalid PCM format";
                continue;
            }
        }

        DWORD duration{};
        // The original queries duration through slot 5 but does not reject an
        // otherwise valid reader when that optional information is absent.
        if (FAILED(InvokeReaderDword(reader, 5, &duration))) duration = 0;
        DWORD encoded_bits_per_second{};
        if (FAILED(InvokeReaderDword(reader, 9, &encoded_bits_per_second)))
            encoded_bits_per_second = 0;
        DWORD capabilities{};
        if (FAILED(InvokeReaderDword(reader, 4, &capabilities))) capabilities = 0;

        void* metadata{};
        std::vector<MetadataEntry> metadata_entries;
        if (SUCCEEDED(InvokeQueryInterface(
                reader, kSoundMetadataInterface, &metadata)) && metadata) {
            DWORD count{};
            if (SUCCEEDED(InvokeMetadataCount(metadata, &count))) {
                count = std::min<DWORD>(count, 65536);
                try { metadata_entries.reserve(count); }
                catch (...) { count = 0; }
                for (DWORD index = 0; index < count; ++index) {
                    wchar_t* allocated_name{};
                    wchar_t* allocated_value{};
                    const HRESULT metadata_result = InvokeMetadataAt(
                        metadata, index, &allocated_name, &allocated_value);
                    size_t name_length{}, value_length{};
                    if (SUCCEEDED(metadata_result) && allocated_name &&
                        allocated_value &&
                        SafeWideLength(allocated_name, 32768, &name_length) &&
                        SafeWideLength(allocated_value, 1024U * 1024U,
                                       &value_length)) {
                        try {
                            metadata_entries.push_back({
                                std::wstring(allocated_name, name_length),
                                std::wstring(allocated_value, value_length)});
                        } catch (...) {
                        }
                    }
                    if (allocated_value) CoTaskMemFree(allocated_value);
                    if (allocated_name) CoTaskMemFree(allocated_name);
                }
            }
        }
        std::unique_ptr<void, decltype(&Release)> metadata_owner(
            metadata, &Release);

        std::vector<unsigned char> thumbnail_bytes;
        void* thumbnail{};
        if (SUCCEEDED(InvokeQueryInterface(
                reader, kSoundThumbnailInterface, &thumbnail)) && thumbnail) {
            const LegacyThumbnailEntry* entry{};
            if (InvokeThumbnailAt(thumbnail, 0, &entry) == S_OK)
                static_cast<void>(CopyThumbnail(entry, thumbnail_bytes));
        }
        std::unique_ptr<void, decltype(&Release)> thumbnail_owner(
            thumbnail, &Release);

        std::wstring codec;
        wchar_t* allocated_codec{};
        if (SUCCEEDED(InvokeReaderCodecName(reader, &allocated_codec)) && allocated_codec) {
            size_t length{};
            if (SafeWideLength(allocated_codec, 32768, &length))
                codec.assign(allocated_codec, length);
        }
        if (allocated_codec) CoTaskMemFree(allocated_codec);
        if (codec.empty()) {
            codec = factory.format.description;
            const size_t parenthesis = codec.find(L'(');
            if (parenthesis != std::wstring::npos) codec.resize(parenthesis);
            while (!codec.empty() && iswspace(codec.back())) codec.pop_back();
        }

        try {
            auto implementation = std::make_unique<LegacyReaderSession::Impl>();
            // Transfer ownership immediately: any later string/path allocation
            // failure now releases the private reader through Impl::~Impl.
            implementation->reader = reader;
            reader = nullptr;
            implementation->metadata = metadata_owner.release();
            implementation->thumbnail_object = thumbnail_owner.release();
            implementation->decoder = std::move(decoder);
            if (implementation->decoder) {
                const WAVEFORMATEX& decoded_format =
                    implementation->decoder->OutputFormat();
                const size_t decoded_format_bytes =
                    sizeof(WAVEFORMATEX) + decoded_format.cbSize;
                implementation->format.resize(decoded_format_bytes);
                std::memcpy(implementation->format.data(), &decoded_format,
                            decoded_format_bytes);
                implementation->encoded_format = std::move(encoded_format);
            } else {
                // The reader-allocated object was already copied as
                // sizeof(WAVEFORMATEX)+cbSize, so transfer it directly and
                // preserve an extensible PCM reader's complete format too.
                implementation->format = std::move(encoded_format);
            }
            implementation->encoded_suggested_buffer_bytes =
                encoded_suggested_buffer_bytes;
            implementation->duration_ms = duration;
            implementation->suggested_buffer_bytes = suggested;
            implementation->encoded_bits_per_second = encoded_bits_per_second;
            implementation->capabilities = capabilities;
            implementation->codec_name = std::move(codec);
            implementation->module_path = factory.format.module_path;
            implementation->metadata_entries = std::move(metadata_entries);
            implementation->thumbnail = std::move(thumbnail_bytes);
            auto session = std::unique_ptr<LegacyReaderSession>(
                new LegacyReaderSession(std::move(implementation)));
            if (result) *result = S_OK;
            if (diagnostic) diagnostic->clear();
            return session;
        } catch (...) {
            // make_unique may fail before the reader is transferred.
            if (reader) Release(reader);
            if (result) *result = E_OUTOFMEMORY;
            if (diagnostic) *diagnostic = L"allocating reader session";
            return {};
        }
    }
    if (result) *result = matched ? last_result
                                  : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!matched && diagnostic) *diagnostic = L"no registered reader matched the extension";
    return {};
}

bool PluginManager::HasDecoderForFormat(
    const WAVEFORMATEX& format) const noexcept {
    const GUID subtype = WaveSubtype(format);
    for (const auto& factory : decoder_factories_) {
        void* creator = ResolveCategoryInterface(
            factory.module_index, factory.enumeration_index,
            kDecoderCreatorCategory, factory.provider);
        if (!creator) continue;
        const HRESULT supported = InvokeSubtypeSupport(creator, &subtype);
        Release(creator);
        // 004CD0ED branches only on FAILED.  The shipped DMO catch-all
        // creator intentionally returns S_FALSE from slot 5 and is therefore
        // still a valid candidate.
        if (SUCCEEDED(supported)) return true;
    }
    return false;
}

std::unique_ptr<LegacyDecoderSession> PluginManager::OpenDecoder(
    const WAVEFORMATEX& encoded_format, WORD requested_output_tag,
    HRESULT* result,
    std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (requested_output_tag != WAVE_FORMAT_PCM &&
        requested_output_tag != 3) {
        if (result) *result = E_INVALIDARG;
        if (diagnostic) *diagnostic = L"unsupported decoder output format";
        return {};
    }
    const GUID subtype = WaveSubtype(encoded_format);
    HRESULT last_result = E_NOTIMPL;
    bool matched{};
    for (const auto& factory : decoder_factories_) {
        void* creator = ResolveCategoryInterface(
            factory.module_index, factory.enumeration_index,
            kDecoderCreatorCategory, factory.provider);
        if (!creator) {
            last_result = E_NOINTERFACE;
            continue;
        }
        const HRESULT supported = InvokeSubtypeSupport(creator, &subtype);
        if (FAILED(supported)) {
            Release(creator);
            continue;
        }
        matched = true;

        void* decoder{};
        HRESULT current = InvokeCreateReader(creator, &decoder);
        Release(creator);
        if (FAILED(current) || !decoder) {
            if (SUCCEEDED(current)) current = E_NOINTERFACE;
            if (decoder) Release(decoder);
            last_result = current;
            if (diagnostic) *diagnostic = L"decoder creator slot 3";
            continue;
        }

        alignas(WAVEFORMATEX) std::array<std::byte, 0x28> output_format{};
        reinterpret_cast<WAVEFORMATEX*>(output_format.data())->wFormatTag =
            requested_output_tag;
        current = InvokeDecoderInitialize(
            decoder, &encoded_format,
            reinterpret_cast<WAVEFORMATEX*>(output_format.data()));
        if (FAILED(current)) {
            Release(decoder);
            last_result = current;
            if (diagnostic) *diagnostic = L"decoder slot 3 initialize";
            continue;
        }

        DWORD input_bytes{};
        DWORD output_bytes{};
        current = InvokeDecoderBufferSizes(
            decoder, &input_bytes, &output_bytes);
        if (FAILED(current) || input_bytes == 0 || output_bytes == 0) {
            Release(decoder);
            last_result = FAILED(current) ? current : E_UNEXPECTED;
            if (diagnostic) *diagnostic = L"decoder slot 4 buffer sizes";
            continue;
        }

        try {
            auto implementation = std::make_unique<LegacyDecoderSession::Impl>();
            implementation->decoder = decoder;
            decoder = nullptr;
            implementation->output_format = output_format;
            implementation->input_bytes = input_bytes;
            implementation->output_bytes = output_bytes;
            implementation->module_path = factory.info.module_path;
            auto session = std::unique_ptr<LegacyDecoderSession>(
                new LegacyDecoderSession(std::move(implementation)));
            // FUN_004CD045 returns the successful creator slot-5 status;
            // notably the DMO creator returns S_FALSE here.
            if (result) *result = supported;
            if (diagnostic) diagnostic->clear();
            return session;
        } catch (...) {
            if (decoder) Release(decoder);
            if (result) *result = E_OUTOFMEMORY;
            if (diagnostic) *diagnostic = L"allocating decoder session";
            return {};
        }
    }

    if (result) *result = matched ? last_result : E_NOTIMPL;
    if (diagnostic && diagnostic->empty())
        *diagnostic = matched ? L"no decoder instance initialized"
                              : L"no decoder accepted the wave subtype";
    return {};
}

HRESULT PluginManager::ConfigureEncoder(size_t index, HWND parent,
                                        std::wstring* diagnostic) const noexcept {
    if (diagnostic) diagnostic->clear();
    if (index >= encoder_factories_.size()) return E_INVALIDARG;
    const auto& factory = encoder_factories_[index];
    const HRESULT prepared = PrepareEncoderDependencies(factory.module_index, diagnostic);
    if (FAILED(prepared)) return prepared;
    void* creator = ResolveCategoryInterface(
        factory.module_index, factory.enumeration_index,
        kEncoderCreatorCategory, factory.provider);
    if (!creator) return E_NOINTERFACE;
    const HRESULT result = InvokeEncoderConfigure(creator, parent);
    Release(creator);
    return result;
}

std::unique_ptr<LegacyEncoderSession> PluginManager::CreateEncoder(
    size_t index, HRESULT* result, std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (index >= encoder_factories_.size()) {
        if (result) *result = E_INVALIDARG;
        if (diagnostic) *diagnostic = L"encoder index is out of range";
        return {};
    }

    const auto& factory = encoder_factories_[index];
    const HRESULT prepared = PrepareEncoderDependencies(factory.module_index, diagnostic);
    if (FAILED(prepared)) {
        if (result) *result = prepared;
        return {};
    }
    void* creator = ResolveCategoryInterface(
        factory.module_index, factory.enumeration_index,
        kEncoderCreatorCategory, factory.provider);
    if (!creator) {
        if (result) *result = E_NOINTERFACE;
        if (diagnostic) *diagnostic = L"encoder creator QueryInterface";
        return {};
    }

    void* encoder{};
    HRESULT current = InvokeCreateReader(creator, &encoder);
    Release(creator);
    if (FAILED(current) || !encoder) {
        if (SUCCEEDED(current)) current = E_NOINTERFACE;
        if (encoder) Release(encoder);
        if (result) *result = current;
        if (diagnostic) *diagnostic = L"encoder creator slot 3";
        return {};
    }

    try {
        auto implementation = std::make_unique<LegacyEncoderSession::Impl>();
        implementation->encoder = encoder;
        encoder = nullptr;
        implementation->module_path = factory.info.module_path;
        auto session = std::unique_ptr<LegacyEncoderSession>(
            new LegacyEncoderSession(std::move(implementation)));
        if (result) *result = S_OK;
        return session;
    } catch (...) {
        if (encoder) Release(encoder);
        if (result) *result = E_OUTOFMEMORY;
        if (diagnostic) *diagnostic = L"allocating encoder session";
        return {};
    }
}

std::unique_ptr<LegacyLyricSearchSession> PluginManager::CreateLyricSearch(
    size_t index, void* context, void* sound_library_host,
    HRESULT* result, std::wstring* diagnostic) const {
    if (diagnostic) diagnostic->clear();
    if (index >= lyric_providers_.size()) {
        if (result) *result = E_INVALIDARG;
        if (diagnostic) *diagnostic = L"lyric provider index is out of range";
        return {};
    }

    const auto& provider_info = lyric_providers_[index];
    void* provider = ResolveCategoryInterface(
        provider_info.module_index, provider_info.enumeration_index,
        kLyricSearchCategory, provider_info.provider);
    if (!provider) {
        if (result) *result = E_NOINTERFACE;
        if (diagnostic) *diagnostic = L"lyric provider QueryInterface";
        return {};
    }

    void* search{};
    HRESULT current = InvokeCreateReader(provider, &search);
    Release(provider);
    if (FAILED(current) || !search) {
        if (SUCCEEDED(current)) current = E_NOINTERFACE;
        if (search) Release(search);
        if (result) *result = current;
        if (diagnostic) *diagnostic = L"lyric provider slot 3";
        return {};
    }
    current = InvokeLyricSearchInitialize(
        search, context, sound_library_host);
    if (FAILED(current)) {
        Release(search);
        if (result) *result = current;
        if (diagnostic) *diagnostic = L"lyric search slot 3 initialize";
        return {};
    }

    try {
        auto implementation =
            std::make_unique<LegacyLyricSearchSession::Impl>();
        implementation->search = search;
        search = nullptr;
        implementation->module_path = provider_info.info.module_path;
        auto session = std::unique_ptr<LegacyLyricSearchSession>(
            new LegacyLyricSearchSession(std::move(implementation)));
        if (result) *result = S_OK;
        return session;
    } catch (...) {
        if (search) Release(search);
        if (result) *result = E_OUTOFMEMORY;
        if (diagnostic) *diagnostic = L"allocating lyric search session";
        return {};
    }
}

void PluginManager::Shutdown() noexcept {
    // FUN_004CB0F1 releases the four registry caches in table order.  Only
    // after that does CSoundLibrary_Shutdown (004CA828) destroy the module
    // wrappers, so no vtable is ever invoked after FreeLibrary.
    std::lock_guard lock(registry_mutex_);
    for (auto& factory : reader_factories_)
        if (factory.provider) Release(factory.provider);
    for (auto& factory : decoder_factories_)
        if (factory.provider) Release(factory.provider);
    for (auto& factory : encoder_factories_)
        if (factory.provider) Release(factory.provider);
    for (auto& provider : lyric_providers_)
        if (provider.provider) Release(provider.provider);
    reader_factories_.clear();
    decoder_factories_.clear();
    encoder_factories_.clear();
    lyric_providers_.clear();
    for (auto& loaded : loaded_modules_) {
        if (loaded.addin) Release(loaded.addin);
        if (loaded.module) FreeLibrary(loaded.module);
    }
    loaded_modules_.clear();
    reader_formats_.clear();
    decoder_factory_info_.clear();
    encoder_factory_info_.clear();
    lyric_provider_info_.clear();
    plugins_.clear();
}

} // namespace ttplayer::plugins
