#include "ttplayer/ui/taskbar_icon.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#include <propsys.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>

namespace ttplayer::ui {
namespace {
template<class T> struct ReleaseCom { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;
using WindowStoreFn = HRESULT(WINAPI*)(HWND, REFIID, void**);
WindowStoreFn WindowStore() {
    // No new load-time requirement on XP/Vista (including the universal EXE).
    static const auto function = reinterpret_cast<WindowStoreFn>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetPropertyStoreForWindow"));
    return function;
}

std::wstring NormalizedPath(const std::filesystem::path& path) {
    std::array<wchar_t, 32768> text{};
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(text.size()), text.data(), nullptr);
    if (!length || length >= text.size()) return {};
    std::wstring result(text.data(), length);
    length = GetLongPathNameW(result.c_str(), text.data(), static_cast<DWORD>(text.size()));
    if (length && length < text.size()) result.assign(text.data(), length);
    LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, result.data(), static_cast<int>(result.size()),
                 result.data(), static_cast<int>(result.size()));
    return result;
}

std::wstring Hash(const void* data, size_t size) {
    // Content-addressed files also invalidate Explorer's icon cache when the
    // user overwrites an ICO in place. This is an identity, not a security hash.
    std::uint64_t value = 14695981039346656037ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { value ^= bytes[i]; value *= 1099511628211ULL; }
    wchar_t text[17]{};
    swprintf_s(text, L"%016llX", static_cast<unsigned long long>(value));
    return text;
}

HRESULT SetString(IPropertyStore* store, REFPROPERTYKEY key, const std::wstring& text) {
    PROPVARIANT previous{};
    store->GetValue(key, &previous);
    const bool unchanged = previous.vt == VT_LPWSTR && previous.pwszVal && text == previous.pwszVal;
    PropVariantClear(&previous);
    // Reassigning even the same ID makes Windows 7 recreate its taskbar
    // button, which in turn sends TaskbarButtonCreated back to the player.
    if (unchanged) return S_OK;
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<wchar_t*>(text.c_str());
    return store->SetValue(key, value); // The store copies the borrowed string.
}

std::wstring GetString(IPropertyStore* store, REFPROPERTYKEY key) {
    PROPVARIANT value{};
    store->GetValue(key, &value);
    std::wstring text = value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"";
    PropVariantClear(&value);
    return text;
}

struct IconImage { BYTE width{}, height{}; std::vector<BYTE> bytes; };
IconImage ReadIcon(HICON icon) {
    ICONINFO source{};
    if (!icon || !GetIconInfo(icon, &source)) return {};
    BITMAP bitmap{};
    GetObjectW(source.hbmColor ? source.hbmColor : source.hbmMask, sizeof(bitmap), &bitmap);
    const int width = bitmap.bmWidth;
    const int height = source.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    IconImage result;
    if (width > 0 && width <= 256 && height > 0 && height <= 256 && source.hbmColor) {
        const size_t color_size = static_cast<size_t>(width) * height * 4;
        const size_t mask_size = static_cast<size_t>((width + 31) / 32 * 4) * height;
        result.bytes.resize(sizeof(BITMAPINFOHEADER) + color_size + mask_size);
        BITMAPINFO color{};
        color.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        color.bmiHeader.biWidth = width; color.bmiHeader.biHeight = height;
        color.bmiHeader.biPlanes = 1; color.bmiHeader.biBitCount = 32;
        struct { BITMAPINFOHEADER header; RGBQUAD colors[2]; } mask{};
        mask.header = color.bmiHeader; mask.header.biBitCount = 1;
        HDC dc = GetDC(nullptr);
        const bool ok = dc &&
            GetDIBits(dc, source.hbmColor, 0, height, result.bytes.data() + sizeof(BITMAPINFOHEADER),
                      &color, DIB_RGB_COLORS) == height &&
            GetDIBits(dc, source.hbmMask, 0, height, result.bytes.data() + sizeof(BITMAPINFOHEADER) + color_size,
                      reinterpret_cast<BITMAPINFO*>(&mask), DIB_RGB_COLORS) == height;
        if (dc) ReleaseDC(nullptr, dc);
        if (ok) {
            // ICO stores bottom-up color plus an AND mask (twice the height).
            color.bmiHeader.biHeight = height * 2;
            color.bmiHeader.biSizeImage = static_cast<DWORD>(color_size + mask_size);
            std::memcpy(result.bytes.data(), &color.bmiHeader, sizeof(BITMAPINFOHEADER));
            result.width = static_cast<BYTE>(width); result.height = static_cast<BYTE>(height);
        } else result = {};
    }
    if (source.hbmColor) DeleteObject(source.hbmColor);
    if (source.hbmMask) DeleteObject(source.hbmMask);
    return result;
}

std::filesystem::path CacheIcon(HICON small_icon, HICON large_icon, const std::wstring& id) {
    std::vector<IconImage> images;
    for (HICON icon : {small_icon, large_icon}) {
        auto image = ReadIcon(icon);
        if (!image.bytes.empty()) images.push_back(std::move(image));
    }
    if (images.empty()) return {};
    // Write ICO fields individually; native struct alignment is not its format.
    std::vector<BYTE> data;
    auto append = [&data](const auto& value) {
        const auto* bytes = reinterpret_cast<const BYTE*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(value));
    };
    append(WORD{0}); append(WORD{1}); append(static_cast<WORD>(images.size()));
    DWORD offset = 6 + static_cast<DWORD>(images.size()) * 16;
    for (const auto& image : images) {
        append(image.width); append(image.height); append(WORD{0});
        append(WORD{1}); append(WORD{32});
        const auto size = static_cast<DWORD>(image.bytes.size());
        append(size); append(offset); offset += size;
    }
    for (const auto& image : images) data.insert(data.end(), image.bytes.begin(), image.bytes.end());
    wchar_t local[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) return {};
    const auto directory = std::filesystem::path(local) / L"TTPlayerRebuild" / L"TaskbarIcons" / id;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {};
    const auto path = directory / (Hash(data.data(), data.size()) + L".ico");
    // Already-published files are immutable and must survive process exit.
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
        const std::vector<BYTE> contents((std::istreambuf_iterator<char>(existing)), {});
        if (contents == data) return path;
    }
    existing.close();
    const auto temporary = directory / (std::to_wstring(GetCurrentProcessId()) + L".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str()); return {};
    }
    return path;
}

bool UpdatePinnedIcons(const std::filesystem::path& executable,
                       const std::filesystem::path& icon, const std::wstring& id, bool& changed) {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata))) return false;
    const auto directory = std::filesystem::path(appdata) /
        L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned";
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return !error;
    bool ok = true;
    // Windows 7 keeps window-created relaunch links under ImplicitAppShortcuts,
    // while executable/shortcut pins normally live under TaskBar.
    for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (_wcsicmp(it->path().extension().c_str(), L".lnk") != 0) continue;
        const auto result = UpdatePlayerShortcutIcon(it->path(), executable, icon, id);
        if (result == ShortcutIconUpdate::failed) ok = false;
        if (result == ShortcutIconUpdate::updated) changed = true;
    }
    return ok && !error;
}
} // namespace

bool HasTaskbarIconProperties() { return WindowStore() != nullptr; }

std::wstring PlayerAppUserModelId(const std::filesystem::path& executable) {
    const auto path = NormalizedPath(executable);
    return path.empty() ? L"" : L"Sonic853.TTPlayerRebuild." + Hash(path.data(), path.size() * sizeof(wchar_t));
}

HRESULT InitializePlayerTaskbarIdentity() {
    using SetProcessIdFn = HRESULT(WINAPI*)(PCWSTR);
    const auto set_id = reinterpret_cast<SetProcessIdFn>(GetProcAddress(
        GetModuleHandleW(L"shell32.dll"), "SetCurrentProcessExplicitAppUserModelID"));
    if (!set_id) return S_FALSE; // XP/Vista: retain the system's legacy grouping.
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return E_FAIL;
    const auto id = PlayerAppUserModelId(path.data());
    if (id.empty()) return E_FAIL;
    // A window-level ID overrides this. Publishing it only on the player
    // splits unowned plug-in windows (e.g. Ozone) into an implicit-ID group.
    return set_id(id.c_str());
}

ShortcutIconUpdate UpdatePlayerShortcutIcon(const std::filesystem::path& shortcut,
    const std::filesystem::path& executable, const std::filesystem::path& icon, const std::wstring& id) {
    IShellLinkW* raw_link{};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_link))))
        return ShortcutIconUpdate::unrelated;
    ComPtr<IShellLinkW> link(raw_link);
    IPersistFile* raw_file{};
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&raw_file)))) return ShortcutIconUpdate::unrelated;
    ComPtr<IPersistFile> file(raw_file);
    if (FAILED(file->Load(shortcut.c_str(), STGM_READ))) return ShortcutIconUpdate::unrelated;
    std::array<wchar_t, 32768> target{}, expanded{};
    if (FAILED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) || !target[0])
        return ShortcutIconUpdate::unrelated;
    const DWORD count = ExpandEnvironmentStringsW(target.data(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (!count || count > expanded.size()) return ShortcutIconUpdate::unrelated;
    std::error_code error;
    if (NormalizedPath(expanded.data()) != NormalizedPath(executable) &&
        !std::filesystem::equivalent(expanded.data(), executable, error)) return ShortcutIconUpdate::unrelated;
    if (FAILED(file->Load(shortcut.c_str(), STGM_READWRITE))) return ShortcutIconUpdate::failed;
    IPropertyStore* raw_store{};
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&raw_store)))) return ShortcutIconUpdate::failed;
    ComPtr<IPropertyStore> store(raw_store);
    int index{};
    link->GetIconLocation(target.data(), static_cast<int>(target.size()), &index);
    if (index == 0 && icon == target.data() && GetString(store.get(), PKEY_AppUserModel_ID) == id)
        return ShortcutIconUpdate::unchanged;
    if (FAILED(link->SetIconLocation(icon.c_str(), 0)) || FAILED(SetString(store.get(), PKEY_AppUserModel_ID, id)) ||
        FAILED(store->Commit()) || FAILED(file->Save(shortcut.c_str(), TRUE))) return ShortcutIconUpdate::failed;
    auto notification_path = shortcut;
    notification_path.make_preferred();
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, notification_path.c_str(), nullptr);
    return ShortcutIconUpdate::updated;
}

std::filesystem::path PublishTaskbarIcon(HWND window, HICON small_icon, HICON large_icon, const std::wstring& title) {
    if (!WindowStore()) return {};
    IPropertyStore* raw_store{};
    if (FAILED(WindowStore()(window, IID_PPV_ARGS(&raw_store)))) return {};
    ComPtr<IPropertyStore> store(raw_store);
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    const std::filesystem::path executable(path.data());
    const auto id = PlayerAppUserModelId(executable);
    const auto icon = CacheIcon(small_icon, large_icon, id);
    if (icon.empty()) return {};
    // Migrate legacy implicit-ID pins as well, so they match the startup
    // process identity and do not retain a separate group after an upgrade.
    bool changed{};
    const bool links_ready = UpdatePinnedIcons(executable, icon, id, changed);
    if (!links_ready) return {};
    SetString(store.get(), PKEY_AppUserModel_RelaunchCommand, L"\"" + executable.wstring() + L"\"");
    SetString(store.get(), PKEY_AppUserModel_RelaunchDisplayNameResource, title);
    SetString(store.get(), PKEY_AppUserModel_RelaunchIconResource, icon.wstring() + L",0");
    SetString(store.get(), PKEY_AppUserModel_ID, id);
    // Win7's pinned group caches the link's icon/identity separately from the
    // ordinary folder icon cache. UPDATEITEM alone leaves the previous image
    // visible. Invalidate that shell cache once, only after a link changed and
    // the window metadata is ready. Never restart Explorer or rewrite Taskband.
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return icon;
}

void ClearTaskbarIconProperties(HWND window) {
    if (!WindowStore()) return;
    IPropertyStore* raw_store{};
    if (FAILED(WindowStore()(window, IID_PPV_ARGS(&raw_store)))) return;
    ComPtr<IPropertyStore> store(raw_store);
    const PROPVARIANT empty{};
    for (const auto& key : {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
                           PKEY_AppUserModel_RelaunchDisplayNameResource, PKEY_AppUserModel_RelaunchIconResource})
        store->SetValue(key, empty);
}
} // namespace ttplayer::ui
