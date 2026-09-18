#pragma once

#include <filesystem>
#include <windows.h>

namespace ttplayer::audio { struct AudioMetadata; }
namespace ttplayer::testing { struct SkinRebindAccess; }

namespace ttplayer::ui {
// UI-thread owned. All DWM entry points are optional so XP can load the EXE.
class TaskbarPreview {
public:
    // Borrowed until SetSource/Clear/Reset; UI thread only.
    [[nodiscard]] HBITMAP CoverBitmap() const noexcept { return cover_; }
    struct Api {
        HRESULT (WINAPI* composition)(BOOL*);
        HRESULT (WINAPI* attribute)(HWND, DWORD, LPCVOID, DWORD);
        HRESULT (WINAPI* invalidate)(HWND);
        HRESULT (WINAPI* thumbnail)(HWND, HBITMAP, DWORD);
        HRESULT (WINAPI* live_preview)(HWND, HBITMAP, POINT*, DWORD);
    };
    explicit TaskbarPreview(Api api = NativeApi()) : api_(api) {}
    ~TaskbarPreview();
    TaskbarPreview(const TaskbarPreview&) = delete;
    TaskbarPreview& operator=(const TaskbarPreview&) = delete;

    // Called once per accepted decoder-open, independent of visualization mode.
    void SetSource(HWND window, const std::filesystem::path& path,
                   const audio::AudioMetadata& metadata);
    void Clear();
    void Reset();
    void Refresh(HWND window, bool force = false);
    [[nodiscard]] bool HandleMessage(UINT message, LPARAM lparam);

private:
    friend struct ttplayer::testing::SkinRebindAccess;
    static Api NativeApi();
    bool Available() const;
    void Disable();
    bool UpdateGeometry();
    Api api_{};
    HWND window_{};
    HBITMAP cover_{};
    SIZE cover_size_{};
    SIZE client_size_{};
    POINT client_offset_{};
    HRGN window_region_{}; // Last non-minimized shape, in window coordinates.
    HMONITOR monitor_{};
    bool minimized_{};
    bool enabled_{};
};
} // namespace ttplayer::ui
