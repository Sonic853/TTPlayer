#include "ttplayer/platform/optional_windows_api.h"

#include <string>
#include <cstring>

namespace ttplayer::platform {
namespace {
HMODULE LoadSystemComponent(const wchar_t* name) {
#ifdef TTPLAYER_TEST_NO_OPTIONAL_WINDOWS_API
    (void)name;
    return nullptr;
#else
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return nullptr;
    // An absolute system path works on unpatched XP, without depending on
    // LOAD_LIBRARY_SEARCH_SYSTEM32 or searching the player/plugin directory.
    return LoadLibraryW((std::wstring(directory, length) + L"\\" + name).c_str());
#endif
}
HMODULE MediaPlatform() {
    static const HMODULE module = LoadSystemComponent(L"mfplat.dll");
    return module;
}
HMODULE MediaReader() {
    static const HMODULE module = LoadSystemComponent(L"mfreadwrite.dll");
    return module;
}
HMODULE PropertySystem() {
    static const HMODULE module = LoadSystemComponent(L"propsys.dll");
    return module;
}
HMODULE Shell() {
    static const HMODULE module = LoadSystemComponent(L"shell32.dll");
    return module;
}
HMODULE CommonControls() {
    // Resolve the activation-context-selected v6 module, not System32's v5.
    static const HMODULE module = LoadSystemComponent(L"comctl32.dll");
    return module;
}
HMODULE WindowsMedia() {
    // The fallback decoder remains available in the test that disables MF.
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return nullptr;
    static const HMODULE module = LoadLibraryW((std::wstring(directory) + L"\\wmvcore.dll").c_str());
    return module;
}
// Keep modules loaded for the lifetime of their COM objects and worker threads.
template<class Function> Function Resolve(HMODULE module, const char* name) {
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}
} // namespace

bool HasMediaFoundation() {
    return MediaPlatform() && MediaReader() &&
        GetProcAddress(MediaPlatform(), "MFStartup") &&
        GetProcAddress(MediaReader(), "MFCreateSourceReaderFromURL");
}

IStream* SHCreateMemStream(const BYTE* bytes, UINT size) {
    if (!bytes && size) return nullptr;
    IStream* stream{};
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return nullptr;
    ULONG written{};
    const HRESULT result = size ? stream->Write(bytes, size, &written) : S_OK;
    LARGE_INTEGER start{};
    if (FAILED(result) || written != size || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
        stream->Release();
        return nullptr;
    }
    return stream;
}

#define OPTIONAL_API(module, name, parameters, arguments, reset) \
    HRESULT name parameters { \
        using Function = HRESULT (WINAPI*) parameters; \
        static const auto function = Resolve<Function>(module(), #name); \
        if (function) return function arguments; \
        reset; \
        return E_NOTIMPL; \
    }

OPTIONAL_API(MediaPlatform, MFStartup, (ULONG version, DWORD flags), (version, flags), (void)0)
OPTIONAL_API(MediaPlatform, MFShutdown, (), (), (void)0)
OPTIONAL_API(WindowsMedia, WMCreateSyncReader,
    (IUnknown* certificate, DWORD rights, IWMSyncReader** reader),
    (certificate, rights, reader), if (reader) *reader = nullptr)
OPTIONAL_API(Shell, SHGetPropertyStoreFromParsingName,
    (PCWSTR path, IBindCtx* context, GETPROPERTYSTOREFLAGS flags, REFIID iid, void** store),
    (path, context, flags, iid, store), if (store) *store = nullptr)
OPTIONAL_API(CommonControls, TaskDialogIndirect,
    (const TASKDIALOGCONFIG* config, int* button, int* radio, BOOL* checked),
    (config, button, radio, checked), (void)0)
OPTIONAL_API(MediaPlatform, MFCreateAttributes, (IMFAttributes** attributes, UINT32 size),
    (attributes, size), if (attributes) *attributes = nullptr)
OPTIONAL_API(MediaPlatform, MFCreateMediaType, (IMFMediaType** type), (type), if (type) *type = nullptr)
OPTIONAL_API(MediaPlatform, MFCreateMFByteStreamOnStream, (IStream* stream, IMFByteStream** result),
    (stream, result), if (result) *result = nullptr)
OPTIONAL_API(MediaReader, MFCreateSourceReaderFromByteStream,
    (IMFByteStream* stream, IMFAttributes* attributes, IMFSourceReader** reader),
    (stream, attributes, reader), if (reader) *reader = nullptr)
OPTIONAL_API(MediaReader, MFCreateSourceReaderFromURL,
    (LPCWSTR url, IMFAttributes* attributes, IMFSourceReader** reader),
    (url, attributes, reader), if (reader) *reader = nullptr)
OPTIONAL_API(PropertySystem, PropVariantToUInt32, (REFPROPVARIANT value, ULONG* result),
    (value, result), if (result) *result = 0)
OPTIONAL_API(PropertySystem, PropVariantToUInt64, (REFPROPVARIANT value, ULONGLONG* result),
    (value, result), if (result) *result = 0)
OPTIONAL_API(PropertySystem, PropVariantToString, (REFPROPVARIANT value, PWSTR result, UINT size),
    (value, result, size), if (result && size) *result = L'\0')
OPTIONAL_API(PropertySystem, PropVariantToStringAlloc, (REFPROPVARIANT value, PWSTR* result),
    (value, result), if (result) *result = nullptr)
#undef OPTIONAL_API
} // namespace ttplayer::platform
