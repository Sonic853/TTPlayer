#pragma once

#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/integrations/discord_presence.h"
#include "ttplayer/lyrics/lrc_parser.h"
#include "ttplayer/playlist/playlist_store.h"
#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/skin/legacy_skin.h"
#include "ttplayer/skin/skin_package.h"
#include "ttplayer/ui/desktop_lyrics.h"
#include "ttplayer/ui/playlist_rating_input.h"
#include "ttplayer/ui/playlist_scrollbar_contract.h"
#include "ttplayer/ui/playlist_selection_policy.h"
#include "ttplayer/ui/shell_send_to.h"
#include "ttplayer/ui/taskbar_playback.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <commdlg.h>
#include <commctrl.h>
#include <windows.h>

struct ITextDocument;
struct IDataObject;

namespace ttplayer::testing { struct SkinRebindAccess; }

namespace ttplayer::ui {
class VisualRuntime;
class PlayerWindow {
public:
    explicit PlayerWindow(settings::Settings settings);
    ~PlayerWindow();
    PlayerWindow(const PlayerWindow&) = delete;
    PlayerWindow& operator=(const PlayerWindow&) = delete;

    bool Create(HINSTANCE instance, int show_command);
    void SetSkinResourceModule(HMODULE module) noexcept { skin_resources_ = module; }
    void SetSoundReaderFormats(const std::vector<plugins::ReaderFormat>& formats) {
        reader_formats_ = formats;
    }
    void SetSoundLibrary(const plugins::PluginManager* manager) {
        sound_library_ = manager;
        playlist_info_unavailable_sources_.clear();
        reader_formats_ = manager ? manager->ReaderFormats()
                                  : std::vector<plugins::ReaderFormat>{};
        audio_.SetPluginManager(manager);
    }
    void SetTtpCommModule(HMODULE module) noexcept;
    bool LoadSkinPackage(const std::filesystem::path& path, bool restore_profile = true);
    bool LoadSkinResource(HMODULE module, const wchar_t* name = L"<Default_Skin>",
                          bool restore_profile = true);
    bool OpenPath(const std::filesystem::path& path, bool start_playback);
    void OpenCommandLinePath(const std::filesystem::path& path,
                             ULONG_PTR mode);
    int ShowRegistrationOptions(HINSTANCE instance);
    bool UnregisterAllAssociations();
    void CheckStartupAssociations();
    // Called after Create has loaded the numbered playlists and shown the
    // windows. Command-line opens take precedence and skip this transaction.
    void RestoreStartupPlayback();
    [[nodiscard]] bool PreTranslateMessage(const MSG& message) const;
    [[nodiscard]] HWND Handle() const noexcept { return window_; }

private:
    friend struct ttplayer::testing::ProgressSeekAccess;
    friend struct ttplayer::testing::SkinRebindAccess;
    enum class FileDropSurface { player, playlist, lyric };
    enum class ImportPlayback { none, if_idle, force };
    class FileDropTarget;

    struct MenuVisualItem {
        std::wstring text;
        UINT type{};
        UINT state{};
        UINT command{};
        bool submenu{};
        HBITMAP checked_bitmap{};
        HBITMAP unchecked_bitmap{};
        int image{-1};
    };

    struct ToolTipTool {
        HWND tooltip{};
        HWND owner{};
        HWND control{};
        UINT_PTR identifier{};
        std::wstring text;
    };

    struct SkinMenuEntry {
        UINT command{};
        std::filesystem::path path;
        std::wstring package_name;
        skin::SkinMetadata metadata;
        bool embedded_default{};
    };

    struct AssociationOptionNode {
        std::wstring extension;
        std::wstring description;
        std::wstring icon;
        bool current{};
        bool desired{};
        bool icon_dirty{};
    };

    struct DeviceOptionEntry {
        std::wstring key;
        std::wstring name;
        std::wstring module;
        std::array<std::wstring, 4> details;
        GUID identifier{};
        bool has_identifier{};
        bool details_resolved{};
        int backend{}; // 0 waveOut, 1 DirectSound, 2 KS, 3 ASIO
        UINT wave_device_id{WAVE_MAPPER};
    };

    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PlaybackTipWindowProc(
        HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PlaylistWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK LyricWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK LyricEditorProc(HWND, UINT, WPARAM, LPARAM,
                                             UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK EqualizerWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EqualizerControlProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK VisualWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PlaylistEditProc(HWND, UINT, WPARAM, LPARAM,
                                              UINT_PTR, DWORD_PTR);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    LRESULT HandlePlaylistMessage(UINT, WPARAM, LPARAM, HWND mouse_source);
    LRESULT HandleLyricMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleLyricControlMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandlePlaylistControlMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleEqualizerMessage(UINT, WPARAM, LPARAM);
    LRESULT HandleEqualizerControlMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleVisualMessage(UINT, WPARAM, LPARAM);
    void CreateControls();
    void LayoutControls(int width, int height) const;
    void Paint(HDC dc) const;
    void PaintSkin(HDC dc) const;
    void DrawSkinElement(HDC dc, const skin::SkinElement& element, int state) const;
    [[nodiscard]] bool IsSkinElementEnabled(std::wstring_view name) const;
    [[nodiscard]] bool IsSkinElementChecked(std::wstring_view name) const noexcept;
    [[nodiscard]] std::wstring PlaybackStatusText() const;
    [[nodiscard]] std::wstring ChannelText() const;
    [[nodiscard]] HBITMAP ActiveSkinBackground() const noexcept;
    [[nodiscard]] SIZE ActiveSkinSize() const noexcept;
    [[nodiscard]] const std::vector<skin::SkinElement>& ActiveSkinElements() const noexcept;
    [[nodiscard]] const skin::SkinElement* FindActiveSkinElement(std::wstring_view name) const;
    [[nodiscard]] std::wstring HitTestSkin(POINT point) const;
    void InvokeSkinAction(std::wstring_view action);
    void ApplySkinVisualSettings();
    void StartVisualWorker();
    void StopVisualWorker();
    void UpdateVisualWindowLayout();
    void UpdateVisualFrame();
    void PaintVisualControl(HDC dc) const;
    void SetVisualType(int type);
    // FUN_0045D531/FUN_0049F4AD: one modeless 15-page options sheet.  A
    // negative page restores Histroy/LastActivePage; focus_control implements
    // the original private 0x7F4 entry used by desktop lyrics/network links.
    void ShowOptions(int page = -1, UINT focus_control = 0);
    void CloseOptions();
    static INT_PTR CALLBACK OptionsPageDialogProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK OptionsChildDialogProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK OptionsSheetSubclassProc(
        HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    INT_PTR HandleOptionsPageDialog(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleOptionsSheetMessage(HWND, UINT, WPARAM, LPARAM);
    void InitializeOptionsPage(HWND dialog, UINT template_id);
    void CommitOptionsPage(HWND dialog, UINT template_id);
    bool CommitOptionsControl(HWND dialog, UINT template_id, UINT control);
    void FlushDeferredOptionsRuntime(UINT template_id = 0);
    void ApplyOptionsPageRuntime(UINT template_id);
    void ApplyOptionsRuntime(UINT template_id = 0);
    void ApplyOptionsChangeMask(UINT mask, LPARAM source_control);
    void ReloadApplicationIcons();
    void UpdateTrayIcon();
    [[nodiscard]] TaskbarPlaybackState TaskbarState() const;
    [[nodiscard]] TaskbarPlaybackLabels TaskbarLabels() const;
    void UpdateTaskbarPlayback();
    void HandleTaskbarPlaybackClick(WPARAM wparam);
    void RemoveTrayIcon();
    void ShowPlaybackOpenTip();
    void ClosePlaybackOpenTip();
    void PaintPlaybackOpenTip(HDC dc) const;
    void RegisterConfiguredHotKeys();
    void UnregisterConfiguredHotKeys();
    void PositionOptionsPage(HWND page);
    void SelectOptionsPage(int page, UINT focus_control = 0);
    void InitializeOptionsShell();
    void PopulateOptionsSkinPage(HWND dialog);
    void UpdateOptionsSkinDetails(HWND dialog);
    void StartOptionsDspScan(HWND dialog,
                             const std::filesystem::path& folder);
    void PollOptionsDspScan(HWND dialog);
    void CancelOptionsDspScan();
    void LaunchOptionsDspConfiguration(HWND dialog,
                                       const std::filesystem::path& module);
    void UpdateOptionsDeviceDetails(HWND dialog);
    void ShowVisualOptions();
    static INT_PTR CALLBACK VisualOptionsDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR HandleVisualOptionsDialog(HWND, UINT, WPARAM, LPARAM);
    void InvokeVisualAction(POINT point);
    void ShowVisualContextMenu(POINT screen_point);
    [[nodiscard]] bool VisualFallbackHit(POINT point) const;
    void SetFullScreenMode(int mode);
    void UpdateFullScreenLayout();
    void DetachVisualWindow(const RECT& target);
    void RestoreVisualWindow();
    void DetachLyricControl(const RECT& target, HWND insert_after);
    void RestoreLyricControl();
    void LeaveFullScreen();
    void HandleFullScreenDeactivate(HWND activated_window);
    void ToggleMiniMode();
    void BeginSkinBackgroundDrag(HWND source, POINT point, unsigned int hit = 1);
    void ContinueSkinBackgroundDrag(HWND source, POINT point);
    void EndSkinMouseCapture();
    [[nodiscard]] unsigned int PlaylistDragHitTest(POINT point) const;
    [[nodiscard]] std::vector<HWND> RegisteredDragWindows() const;
    void BuildAttachedDragGroup();
    void ShowContextMenu(POINT screen_point);
    [[nodiscard]] HMENU BuildContextMenu();
    static std::vector<SkinMenuEntry> LoadSkinMenuCatalog(
        const std::filesystem::path& skin_directory,
        HMODULE skin_resources, HMODULE ttpcomm_module,
        const std::shared_ptr<std::atomic_bool>& cancel);
    void StartSkinMenuCatalogLoad();
    [[nodiscard]] bool PublishReadySkinMenuCatalog(DWORD wait_milliseconds = 0);
    void InvalidateSkinMenuCatalog() noexcept;
    void PopulateSkinMenu(HMENU menu);
    void PrepareContextMenu(HMENU menu);
    void BeginPopupMenuStyle(HMENU menu, bool hide_keyboard_cues = false);
    void ApplyPopupMenuStyle(HMENU menu);
    void EndPopupMenuStyle();
    void EnsurePopupMenuImages();
    [[nodiscard]] const MenuVisualItem* FindPopupMenuItem(ULONG_PTR data) const noexcept;
    [[nodiscard]] bool MeasurePopupMenuItem(MEASUREITEMSTRUCT& item) const;
    [[nodiscard]] bool DrawPopupMenuItem(const DRAWITEMSTRUCT& item) const;
    [[nodiscard]] std::wstring SkinMenuToolTipText(UINT command) const;
    [[nodiscard]] std::wstring MenuToolTipText(UINT command) const;
    [[nodiscard]] std::wstring ToolTipWithHotKey(
        UINT command, std::wstring text) const;
    void QueueSkinMenuToolTip(UINT command, HMENU menu);
    void ShowQueuedSkinMenuToolTip();
    void HideSkinMenuToolTip();
    bool HandleContextCommand(UINT command);
    [[nodiscard]] HMODULE ResourceModule() const noexcept;
    [[nodiscard]] std::wstring ResourceText(UINT identifier) const;
    bool ApplyLoadedSkin(bool apply_visual_settings = true, bool saved_bounds = false);
    void ApplySkinProfileWindowState();
    void ApplySkinWindowAlpha(BYTE alpha);
    void ApplySkinWindowAlpha(HWND target, BYTE alpha);
    void AnimateSkinWindowAlpha(BYTE alpha);
    void SetSkinWindowVisible(HWND target, bool visible);
    void BeginSkinWindowFade(HWND target, BYTE from, BYTE to, bool group,
                             int completion, BYTE restore_alpha = 255);
    void AdvanceSkinWindowFade();
    void FinishSkinWindowFade();
    void CompleteSkinWindowFadeForReplacement();
    void CancelSkinWindowFade() noexcept;
    void PollCloseAudioFade();
    void FinishCloseWhenFadesComplete();
    [[nodiscard]] BYTE EffectiveSkinWindowAlpha(HWND target) const noexcept;
    void ApplyWindowShadow();
    void UpdateMainWindowCaption();
    void RotateMainWindowCaption();
    void UpdateAutoShutdownTimer();
    void ShowAutoShutdownDialog();
    void ToggleMute();
    void SetSkinVolumeFromPoint(POINT point);
    void SetSkinProgressFromPoint(POINT point);
    void DrawButton(const DRAWITEMSTRUCT& item) const;
    void ChooseFiles(bool replace_and_play = true, HWND owner = nullptr);
    void ChooseFolder();
    void ShowPlayUrlDialog();
    void ShowPlayCdDialog();
    [[nodiscard]] bool CanPlayCompactDisc() const noexcept;
    void ShowPlaylistLocalSearch();
    [[nodiscard]] bool ImportFiles(
        const std::vector<std::filesystem::path>& paths,
        size_t playlist_index, std::optional<size_t> insertion,
        bool replace_current, ImportPlayback playback);
    [[nodiscard]] bool CollectImportedTracks(
        const std::filesystem::path& path,
        std::vector<playlist::Track>& tracks, bool directory_member = false,
        std::vector<std::wstring>* directory_ancestry = nullptr);
    [[nodiscard]] bool CommitImportedTracks(
        std::vector<playlist::Track> tracks, size_t playlist_index,
        size_t insertion, bool replace_current, ImportPlayback playback);
    void HandleDroppedFiles(FileDropSurface surface, IDataObject* data,
                            DWORD key_state, POINTL point, DWORD* effect);
    [[nodiscard]] DWORD FileDropEffect(FileDropSurface surface,
                                      POINTL point, DWORD source_effect);
    bool RegisterFileDropTarget(HWND window, FileDropSurface surface);
    void RevokeFileDropTarget(HWND window);
    void ClearPlaylistDropCue() noexcept;
    void LoadDroppedLyrics(const std::filesystem::path& path);
    void RefreshPlaylist();
    bool CreatePlaylistWindow();
    void TogglePlaylistWindow();
    void UpdatePlaylistWindowSkin(bool saved_bounds = false);
    void UpdatePlaylistWindowRegion();
    void CreatePlaylistListControls();
    void DestroyPlaylistListControls();
    void LayoutPlaylistListControls();
    // CPlayListWnd media-library path recovered from 0047F766 and
    // 00489ED4..0048A818. Music.library is the ordinary TTBL stream used by
    // 004AF7F4/004AF838; the UI index remains an independent identity model.
    struct MediaLibraryState;
    void InitializeMediaLibraryTree();
    void SetMediaLibraryMode(bool enabled);
    void StartMediaLibraryRefresh();
    void PollMediaLibraryWorkers();
    void StartMediaLibraryMonitoring(bool scan_new_directories = false);
    void ApplyMediaLibraryConfiguration();
    void ShutdownMediaLibrary();
    LRESULT ApplyMediaLibraryIndex(LPARAM result);
    LRESULT HandleMediaLibraryDirectoryChange(WPARAM action,
                                               LPARAM notification);
    [[nodiscard]] bool HandleMediaLibraryTreeNotification(
        const NMHDR* notification);
    [[nodiscard]] bool ShowMediaLibraryTreeContextMenu(POINT screen_point);
    [[nodiscard]] bool HandleMediaLibraryCommand(UINT command);
    [[nodiscard]] size_t VisiblePlaylistTrackCount() const noexcept;
    [[nodiscard]] const playlist::Track* VisiblePlaylistTrack(
        size_t index) const noexcept;
    bool SetVisiblePlaylistRating(size_t index, int rating);
    [[nodiscard]] std::vector<std::wstring>
    CaptureSelectedMediaLibraryIdentities() const;
    void RemoveMediaLibraryTracksByIdentity(
        const std::vector<std::wstring>& identities);
    [[nodiscard]] bool CommitMediaLibraryTracks(
        std::vector<playlist::Track> tracks, size_t insertion,
        bool start_if_idle);
    [[nodiscard]] bool UpdateMediaLibraryTrackPath(
        const playlist::Track& source,
        const std::filesystem::path& target);
    [[nodiscard]] std::optional<size_t> VisiblePlaylistPlayingRow() const;
    bool UpdateMediaLibraryTrackFromProperties(
        size_t index, const playlist::Track& updated);
    bool UpdateMediaLibraryTrackByIdentity(
        const playlist::Track& source, const playlist::Track& updated);
    [[nodiscard]] std::wstring PlaylistDisplayText(
        const playlist::Track& track) const;
    void ActivateMediaLibraryResult(size_t index, bool start_playback);
    void SelectMediaLibraryPlaybackTrack(size_t index, bool start_playback);
    void DestroyPlaylistToolControls();
    bool CreateToolTipWindow();
    bool CreatePlaylistToolTipWindow();
    void RemoveToolTipTools(HWND owner);
    void AddToolTipTool(HWND owner, UINT_PTR identifier, const RECT& bounds,
                       HWND target = nullptr);
    void AddToolTipControl(HWND control, UINT_PTR identifier);
    void AddPlaylistToolTipControl(HWND control, UINT_PTR identifier,
                                   std::wstring text);
    void UpdateMainToolRects();
    void UpdatePlaylistToolRects();
    void UpdatePlaylistItemTipRects();
    void QueuePlaylistInfoRange(size_t playlist_index, size_t first,
                                size_t count, bool priority = false);
    void RequestPlaylistTrackInfo(size_t playlist_index, size_t row,
                                  bool priority = false);
    void StartNextPlaylistInfoRead();
    LRESULT ApplyPlaylistInfoResult(LPARAM result);
    void ShutdownPlaylistInfoLoading();
    bool CreateLyricWindow();
    bool CreateDesktopLyrics();
    void CreateLyricControls();
    void DestroyLyricControls();
    void ToggleLyricWindow();
    void EnterDesktopLyricMode();
    void LeaveDesktopLyricMode();
    void UpdateLyricWindowSkin(bool saved_bounds = false);
    void RebuildLyricFont(bool repaint = true);
    void ApplyFullScreenLyricTransparency();
    void UpdateLyricWindowRegion();
    void LayoutLyricControls();
    void UpdateLyricToolRects();
    [[nodiscard]] RECT& ActiveLyricWindowBounds() noexcept;
    [[nodiscard]] const RECT& ActiveLyricWindowBounds() const noexcept;
    [[nodiscard]] bool& ActiveLyricVisible() noexcept;
    [[nodiscard]] bool ActiveLyricVisible() const noexcept;
    [[nodiscard]] bool& ActiveLyricTopMost() noexcept;
    [[nodiscard]] bool ActiveLyricTopMost() const noexcept;
    [[nodiscard]] int& ActiveLyricScrollMode() noexcept;
    [[nodiscard]] int ActiveLyricScrollMode() const noexcept;
    [[nodiscard]] int ActiveLyricTextAlign() const noexcept;
    [[nodiscard]] int ActiveLyricRowInterval() const noexcept;
    [[nodiscard]] int ActiveLyricFadeIndex() const noexcept;
    [[nodiscard]] bool& ActiveLyricFadeHighlight() noexcept;
    [[nodiscard]] bool ActiveLyricFadeHighlight() const noexcept;
    [[nodiscard]] bool& ActiveLyricKaraokeMode() noexcept;
    [[nodiscard]] bool ActiveLyricKaraokeMode() const noexcept;
    [[nodiscard]] bool ActiveLyricTransparent() const noexcept;
    [[nodiscard]] COLORREF ActiveLyricTextColor() const noexcept;
    [[nodiscard]] COLORREF ActiveLyricHighlightColor() const noexcept;
    [[nodiscard]] COLORREF ActiveLyricBackgroundColor() const noexcept;
    void UpdateLyricScrollTimer();
    void CaptureActiveLyricWindowState();
    void ApplyActiveLyricWindowState();
    void PaintLyricWindow(HDC dc) const;
    void PaintLyricControl(HWND control, HDC dc) const;
    [[nodiscard]] std::wstring LyricLineText(size_t index) const;
    [[nodiscard]] int LyricLineExtent(HDC dc, size_t index) const;
    [[nodiscard]] std::pair<size_t, int> LyricDragPosition(HDC dc,
                                                           int delta) const;
    [[nodiscard]] std::pair<size_t, int> LyricDragPosition(
        HDC dc, int delta, std::chrono::milliseconds playback_position) const;
    [[nodiscard]] std::optional<std::chrono::milliseconds> LyricDragTime(
        HDC dc, int delta) const;
    [[nodiscard]] bool LyricTextHitTest(HWND control, POINT point) const;
    [[nodiscard]] RECT LyricElementBounds(const skin::SkinElement& element) const;
    [[nodiscard]] RECT LyricTextBounds() const;
    [[nodiscard]] unsigned int LyricDragHitTest(POINT point) const;
    void ShowLyricContextMenu(POINT screen_point);
    void ShowFullScreenLyricContextMenu(POINT screen_point);
    void PrepareLyricMenu(HMENU menu) const;
    void PrepareFullScreenLyricMenu(HMENU menu) const;
    void PrepareLyricEditorMenu(HMENU menu) const;
    bool HandleLyricCommand(UINT command);
    bool EnterLyricEditor();
    void LeaveLyricEditor(bool prompt_to_save);
    void DestroyLyricEditor();
    void LayoutLyricEditor();
    [[nodiscard]] std::filesystem::path DefaultLyricEditorPath() const;
    [[nodiscard]] std::wstring LyricEditorText() const;
    void SetLyricEditorText(std::wstring_view text, bool modified);
    void InitializeLyricEditorRichText();
    void SetLyricEditorFont(const LOGFONTW& font);
    void FormatLyricEditorAll();
    void FormatLyricEditorLines(LONG begin, LONG end);
    void FormatLyricEditorRange(LONG begin, LONG end);
    void HandleLyricEditorChange();
    void RecordLyricEditorEdit(UINT message, WPARAM wparam,
                               LONG begin, LONG end);
    void OpenLyricEditorLink(UINT message, LONG begin, LONG end);
    [[nodiscard]] LONG FindLyricEditorCharacter(LONG begin, LONG end,
                                                wchar_t character) const;
    [[nodiscard]] std::wstring LyricEditorRangeText(LONG begin,
                                                    LONG end) const;
    void ReplaceLyricEditorRange(LONG begin, LONG end,
                                 std::wstring_view replacement,
                                 bool can_undo = true,
                                 bool reformat = true);
    bool SaveLyricEditor(bool save_as);
    void EditLyricTimestamp(UINT command);
    void ShiftLyricEditorTimestamps(std::chrono::milliseconds delta);
    void ReflowLyricEditor(bool expand);
    void ConvertLyricEditorText(DWORD mapping);
    void ShowLyricFindDialog(bool replace);
    void HandleLyricFindRequest(const FINDREPLACEW& request);
    [[nodiscard]] std::optional<std::wstring> ReadEmbeddedLyrics() const;
    bool WriteEmbeddedLyrics(std::wstring_view text, bool deleting);
    void LoadCurrentLyrics(bool force = false);
    void LoadLyricsFrom(const std::filesystem::path& path, bool associated);
    void ClearLyrics();
    void ApplyAutoLyricVisibility();
    void CopyLyricsToClipboard() const;
    void SeekLyricLine(std::ptrdiff_t delta);
    [[nodiscard]] bool HandleToolTipNotification(HWND owner, LPARAM notification);
    [[nodiscard]] std::wstring ToolTipText(HWND owner, UINT_PTR tool) const;
    bool CreateEqualizerWindow();
    void CreateEqualizerControls();
    void DestroyEqualizerControls();
    void UpdateEqualizerControlState();
    void ToggleEqualizerWindow();
    void UpdateEqualizerWindowSkin(bool saved_bounds = false);
    void UpdateEqualizerWindowRegion();
    void UpdateEqualizerToolRects();
    [[nodiscard]] std::wstring EqualizerToolText(UINT_PTR tool) const;
    void SetEqualizerTrackingStatus(int slider, bool tracking);
    void PaintEqualizer(HDC dc) const;
    [[nodiscard]] int EqualizerHitTest(POINT point) const;
    [[nodiscard]] RECT EqualizerElementBounds(const skin::SkinElement& element) const;
    [[nodiscard]] RECT EqualizerSliderThumbRect(int slider) const;
    [[nodiscard]] int EqualizerSliderValue(int slider) const;
    [[nodiscard]] HWND EqualizerControlWindow(int control) const;
    [[nodiscard]] bool EqualizerOwnsCapture() const;
    [[nodiscard]] bool EqualizerHasCapture(int control) const;
    void CaptureEqualizerControl(int control);
    void ReleaseEqualizerCapture();
    void SetEqualizerSliderValue(int slider, int value, bool user_change);
    void SetEqualizerSliderFromPoint(int slider, POINT point, bool tracking);
    void ApplyEqualizer();
    void InvokeEqualizerControl(int control, POINT screen_point);
    void ShowEqualizerProfileMenu(POINT screen_point);
    bool HandleEqualizerCommand(UINT command);
    void PaintPlaylist(HDC dc) const;
    [[nodiscard]] std::optional<size_t> PlaylistTrackAt(POINT point) const;
    [[nodiscard]] std::optional<size_t> PlaylistListAt(POINT point) const;
    [[nodiscard]] std::optional<size_t> PlaylistListInsertionAt(
        POINT point) const;
    [[nodiscard]] std::optional<size_t> PlaylistToolbarButtonAt(POINT point) const;
    void RememberPlaylistRow(size_t playlist_index,
                             std::optional<size_t> row);
    void SelectPlaylistRow(
        size_t index, bool toggle = false, bool extend = false,
        PlaylistSelectionTrigger trigger =
            PlaylistSelectionTrigger::selection_changed);
    void RestorePlaylistRowSelection();
    void EnsurePlaylistSelectionVisible();
    void SwitchPlaylist(size_t index);
    void ActivatePlaylistCatalogueRow(size_t index);
    void InvokePlaylistToolbar(size_t button, POINT screen_point);
    void ShowPlaylistContextMenu(POINT screen_point, POINT client_point);
    void PreparePlaylistMenu(HMENU menu) const;
    void PopulatePlaylistSendToMenu(HMENU menu);
    bool HandlePlaylistSendToCommand(UINT command);
    bool HandlePlaylistCommand(UINT command);
    bool HandleLegacyPlaylistNetworkCommand(UINT command);
    void ShowPlaylistProperties(
        const playlist::Track* explicit_playback_track = nullptr);
    void BeginPlaylistListEdit(size_t index);
    void LayoutPlaylistListEdit();
    void FinishPlaylistListEdit(bool commit);
    void DeleteSelectedPlaylistRows();
    void RemovePlaylistRowsPreservingView(const std::set<size_t>& rows);
    void DetachPlayingPlaylistItem();
    void ClearActivePlaylist();
    void ShowPlaylistFindDialog(bool quick);
    bool FindNextPlaylistTrack(DWORD flags);
    bool HandlePlaylistTypeToSelect(bool catalogue, wchar_t character);
    [[nodiscard]] int FindPlaylistControlPrefix(
        bool catalogue, int start, const LVFINDINFOW& find) const;
    void FinishPlaylistTrackDrag(POINT point);
    void FinishPlaylistListDrag();
    void BeginPlaylistOleDrag();
    void ChoosePlaylistFile(bool replace_active);
    void ScrollPlaylist(int rows);
    void CancelPlaylistScrollbarInteraction(bool release_capture);
    void LoadStoredPlaylist();
    void SaveStoredPlaylist();
    void CaptureWindowState();
    void SaveCurrentSkinProfile();
    [[nodiscard]] std::filesystem::path CurrentSkinProfilePath() const;
    void PersistWindowState();
    void RefreshPlaybackUi();
    void UpdateDiscordPresence();
    void RebuildSkinInfoItems(bool include_audio_details);
    void ResetSkinInfoScroll();
    void AdvanceSkinInfoScroll(UINT_PTR timer);
    void StartSkinInfoItem();
    bool PlayCurrent(bool report_error = true);
    void ClearPersistedPlaybackIdentity() noexcept;
    void SelectTrack(size_t index, bool start_playback);
    void SelectTrackFrom(size_t playlist_index, size_t index, bool start_playback);
    void SelectRelative(bool next);
    void AdvanceAfterNaturalEnd();
    void Stop();
    void ShowAudioError() const;
    [[nodiscard]] const playlist::Track* OpenedTrack() const noexcept {
        return opened_track_ ? &*opened_track_ : nullptr;
    }
    [[nodiscard]] const playlist::Track* PlaybackTrackForUi() const noexcept;
    bool LoadSkin(skin::SkinPackage package, const std::filesystem::path& cache,
                  const std::wstring& selector, const std::filesystem::path& profile,
                  bool restore_profile);
    [[nodiscard]] playlist::Playlist& ActivePlaylist() noexcept {
        return playlists_.Active();
    }
    [[nodiscard]] const playlist::Playlist& ActivePlaylist() const noexcept {
        return playlists_.Active();
    }
    [[nodiscard]] playlist::Playlist& PlaybackPlaylist() noexcept {
        if (media_library_playback_active_) return media_library_playback_;
        return playing_playlist_index_ && *playing_playlist_index_ < playlists_.Size()
            ? playlists_.At(*playing_playlist_index_) : playlists_.Active();
    }
    [[nodiscard]] const playlist::Playlist& PlaybackPlaylist() const noexcept {
        if (media_library_playback_active_) return media_library_playback_;
        return playing_playlist_index_ && *playing_playlist_index_ < playlists_.Size()
            ? playlists_.At(*playing_playlist_index_) : playlists_.Active();
    }
    // Strict list-index predicate.  UI code which can represent the decoder's
    // detached CPlayItem must use PlaybackTrackForUi() instead.
    [[nodiscard]] bool HasPlaybackTrack() const noexcept {
        if (!current_) return false;
        if (media_library_playback_active_)
            return *current_ < media_library_playback_.Tracks().size();
        return playing_playlist_index_ &&
            *playing_playlist_index_ < playlists_.Size() &&
            *current_ < playlists_.At(*playing_playlist_index_).Tracks().size();
    }

    HINSTANCE instance_{};
    HWND window_{};
    HWND title_{};
    HWND artist_{};
    HWND status_{};
    HWND playlist_view_{};
    HWND playlist_window_{};
    HWND playlist_tree_control_{};
    HWND playlist_list_control_{};
    HWND playlist_track_control_{};
    HWND lyric_window_{};
    HWND lyric_control_{};
    HWND lyric_editor_{};
    HWND lyric_editor_toolbar_{};
    HWND lyric_close_{};
    HWND lyric_ontop_{};
    HWND lyric_desklrc_{};
    HWND lyric_hidden_button_{};
    HWND equalizer_window_{};
    HWND visual_window_{};
    HWND options_window_{};
    HWND options_navigation_{};
    HWND options_header_{};
    std::array<HWND, 15> options_pages_{};
    HWND options_lyric_child_{};
    HWND options_network_child_{};
    int options_page_index_{};
    UINT options_focus_control_{};
    UINT options_deferred_apply_mask_{};
    int options_hotkey_selection_{-1};
    bool options_resetting_{};
    std::vector<SkinMenuEntry> options_skin_entries_;
    HBITMAP options_skin_preview_{};
    COLORREF options_skin_preview_transparent_{CLR_INVALID};
    std::vector<std::filesystem::path> options_dsp_paths_;
    HANDLE options_dsp_scan_process_{};
    HANDLE options_dsp_scan_job_{};
    std::filesystem::path options_dsp_scan_request_;
    std::filesystem::path options_dsp_scan_output_;
    ULONGLONG options_dsp_scan_started_{};
    bool options_dsp_scan_complete_{};
    std::vector<DeviceOptionEntry> options_device_entries_;
    HIMAGELIST options_device_images_{};
    std::vector<std::unique_ptr<AssociationOptionNode>>
        options_association_nodes_;
    HIMAGELIST options_association_images_{};
    std::array<HIMAGELIST, 4> options_association_button_images_{};
    HWND tooltip_{};
    HWND playlist_tooltip_{};
    HWND playlist_item_tooltip_{};
    HWND playlist_list_edit_{};
    HWND progress_{};
    HWND volume_{};
    HFONT ui_font_{};
    HFONT title_font_{};
    HFONT playlist_list_edit_font_{};
    HFONT lyric_font_{};
    HIMAGELIST lyric_editor_images_{};
    HACCEL lyric_editor_accelerators_{};
    HMODULE lyric_editor_module_{};
    ITextDocument* lyric_editor_document_{};
    HBRUSH background_brush_{};
    HBRUSH panel_brush_{};
    mutable HMODULE menu_resources_{};
    HMODULE skin_resources_{}; // borrowed from app::ResourceRuntime
    HMODULE ttpcomm_module_{}; // borrowed from app::RuntimeModules
    HWND skin_menu_tooltip_{};
    HFONT popup_menu_font_{};
    HFONT popup_menu_bold_font_{};
    HIMAGELIST popup_menu_images_{};
    std::vector<std::pair<UINT, int>> popup_menu_image_commands_;
    std::vector<std::array<unsigned char, 16 * 16>> popup_menu_image_masks_;
    std::list<MenuVisualItem> popup_menu_items_;
    bool popup_menu_hide_keyboard_cues_{};
    HICON window_icon_small_{};
    HICON window_icon_big_{};
    bool tray_icon_added_{};
    TaskbarPlaybackControls taskbar_playback_;
    HWND playback_tip_window_{};
    std::wstring playback_tip_title_;
    std::wstring playback_tip_body_;
    std::vector<int> registered_hotkey_ids_;
    settings::Settings settings_;
    integrations::DiscordPresence discord_presence_;
    DesktopLyricsWindow desktop_lyrics_;
    std::optional<skin::LegacySkin> skin_;
    std::shared_ptr<VisualRuntime> visual_runtime_;
    std::jthread visual_worker_;
    std::condition_variable_any visual_worker_condition_;
    std::mutex visual_worker_mutex_;
    std::atomic_uint visual_interval_ms_{50};
    std::atomic_bool visual_worker_enabled_{};
    int fullscreen_mode_{}; // 0 normal, 1 lyric, 2 visual, 3 lyric+visual
    int fullscreen_saved_visual_type_{};
    bool fullscreen_main_was_iconic_{};
    bool fullscreen_main_was_visible_{};
    bool fullscreen_visual_detached_{};
    HWND fullscreen_visual_parent_{};
    RECT fullscreen_visual_saved_rect_{};
    LONG_PTR fullscreen_visual_saved_exstyle_{};
    bool fullscreen_visual_was_empty_{};
    bool fullscreen_lyric_detached_{};
    bool fullscreen_lyric_window_was_visible_{};
    bool fullscreen_saved_lyric_transparent_{};
    bool fullscreen_lyric_desktop_mode_{};
    HWND fullscreen_lyric_parent_{};
    RECT fullscreen_lyric_saved_rect_{};
    LONG_PTR fullscreen_lyric_saved_style_{};
    LONG_PTR fullscreen_lyric_saved_exstyle_{};
    bool fullscreen_lyric_was_empty_{};
    bool fullscreen_desktop_lyric_was_visible_{};
    audio::AudioEngine audio_;
    playlist::PlaylistStore playlists_;
    std::vector<plugins::ReaderFormat> reader_formats_;
    const plugins::PluginManager* sound_library_{};
    // shared_ptr deliberately keeps the opaque type legal at constructor
    // exception-cleanup sites in player_window.cpp; creation/destruction is
    // still confined to player_window_library.cpp where the type is complete.
    std::shared_ptr<MediaLibraryState> media_library_;
    // Directory-only Music.library rows must never be inserted into a numbered
    // TTBL merely to satisfy the playback engine.  This transient query copy
    // owns their playback order but is intentionally outside PlaylistStore.
    playlist::Playlist media_library_playback_;
    bool media_library_playback_active_{};
    // The decoder retains the opened CPlayItem independently of its source
    // list.  This full copy backs UI and replay after that row is detached;
    // current_ remains strictly a list index and is never forged as zero.
    std::optional<playlist::Track> opened_track_;
    std::optional<size_t> current_;
    std::optional<size_t> playing_playlist_index_;
    std::optional<size_t> playlist_selection_;
    std::optional<size_t> playlist_selection_anchor_;
    std::optional<size_t> playlist_list_selection_;
    std::optional<size_t> playlist_list_focus_;
    std::set<size_t> playlist_selected_rows_;
    std::vector<SkinMenuEntry> skin_commands_;
    std::vector<SkinMenuEntry> skin_catalog_cache_;
    std::future<std::vector<SkinMenuEntry>> skin_catalog_future_;
    std::shared_ptr<std::atomic_bool> skin_catalog_cancel_;
    bool skin_catalog_result_stale_{};
    HMENU pending_skin_tooltip_menu_{};
    UINT pending_skin_tooltip_command_{};
    std::wstring skin_menu_tooltip_text_;
    std::wstring display_title_;
    std::wstring display_artist_;
    std::wstring equalizer_tracking_status_;
    std::wstring window_caption_source_;
    bool window_caption_scrolling_{};
    std::vector<std::wstring> info_items_;
    size_t info_item_index_{};
    int info_scroll_offset_{};
    int info_scroll_maximum_{};
    int info_scroll_direction_{};
    int info_scroll_hold_ticks_{};
    int info_vertical_offset_{};
    std::wstring hover_skin_element_;
    std::wstring pressed_skin_element_;
    std::optional<std::chrono::milliseconds> progress_tracking_position_;
    bool dragging_skin_background_{};
    HWND skin_drag_window_{};
    POINT skin_drag_anchor_{};
    POINT skin_drag_screen_anchor_{};
    RECT skin_drag_initial_rect_{};
    unsigned int skin_drag_hit_{};
    std::vector<HWND> attached_drag_windows_;
    bool mini_mode_{};
    RECT normal_window_bounds_{};
    RECT mini_window_bounds_{};
    bool have_normal_window_bounds_{};
    bool have_mini_window_bounds_{};
    bool playback_was_active_{};
    bool playback_source_open_{};
    bool natural_completion_dispatch_{};
    bool pending_natural_play_{};
    bool pending_failed_advance_{};
    ULONGLONG pending_natural_play_tick_{};
    DWORD last_command_line_tick_{};
    UINT_PTR auto_shutdown_timer_{};
    bool context_menu_open_{};
    bool window_state_saved_{};
    int volume_before_mute_{100};
    int transparency_percent_{};
    BYTE skin_window_alpha_{255};
    BYTE rendered_skin_window_alpha_{255};
    struct SkinWindowFadeState {
        HWND target{};
        BYTE from{};
        BYTE to{};
        BYTE restore_alpha{255};
        BYTE last{};
        ULONGLONG started{};
        DWORD duration_ms{};
        bool group{};
        int completion{};
    };
    std::optional<SkinWindowFadeState> skin_window_fade_;
    bool suppress_skin_window_activation_fade_{};
    bool startup_skin_window_fade_pending_{};
    bool close_after_skin_window_fade_{};
    bool close_waiting_for_audio_fade_{};
    bool close_skin_window_fade_finished_{};
    ULONGLONG close_audio_fade_deadline_{};
    bool mini_mode_fade_pending_{};
    bool mini_mode_fade_continuation_{};
    bool mini_mode_fade_queued_{};
    size_t playlist_scroll_{};
    size_t playlist_list_scroll_{};
    DWORD playlist_list_extended_style_{};
    DWORD playlist_track_extended_style_{};
    std::optional<size_t> playlist_hover_;
    std::optional<size_t> playlist_list_hover_;
    std::optional<size_t> playlist_toolbar_hover_;
    HWND playlist_mouse_tracking_window_{};
    std::optional<POINT> playlist_toolbar_menu_return_point_;
    bool playlist_close_hover_{};
    bool playlist_close_pressed_{};
    int equalizer_hover_{};
    int equalizer_pressed_{};
    int equalizer_active_slider_{};
    int equalizer_track_capture_slider_{};
    int equalizer_focused_slider_{};
    int equalizer_slider_drag_offset_{};
    int equalizer_slider_initial_value_{};
    int lyric_hover_command_{};
    int lyric_pressed_command_{};
    bool desktop_lyric_mode_{};
    bool lyric_line_dragging_{};
    POINT lyric_line_drag_origin_{};
    int lyric_line_drag_offset_{};
    lyrics::Lyrics lyrics_;
    std::filesystem::path lyric_path_;
    std::filesystem::path associated_lyric_path_;
    std::filesystem::path lyric_editor_path_;
    int lyric_editor_encoding_{};
    bool lyric_editor_bom_{};
    bool lyric_editor_new_line_{};
    bool lyric_editor_internal_change_{};
    int lyric_editor_edit_kind_{};
    LONG lyric_editor_edit_begin_{};
    LONG lyric_editor_edit_end_{};
    bool lyrics_embedded_{};
    int lyric_adjustment_ms_{};
    FINDREPLACEW lyric_find_{};
    wchar_t lyric_find_text_[256]{};
    wchar_t lyric_replace_text_[256]{};
    HWND lyric_find_dialog_{};
    bool lyric_find_replace_{};
    std::wstring tooltip_text_;
    std::string tooltip_text_ansi_;
    std::list<ToolTipTool> tooltip_tools_;
    std::vector<std::pair<int, HWND>> equalizer_controls_;
    std::vector<std::pair<UINT_PTR, HWND>> playlist_tool_controls_;
    bool playlist_scrollbar_dragging_{};
    PlaylistScrollbarPart playlist_scrollbar_hover_{};
    PlaylistScrollbarPart playlist_scrollbar_pressed_{};
    bool playlist_scrollbar_repeat_fast_{};
    bool playlist_splitter_dragging_{};
    bool playlist_track_drag_pending_{};
    bool playlist_track_dragging_{};
    bool playlist_list_drag_pending_{};
    bool playlist_list_dragging_{};
    bool playlist_external_dragging_{};
    bool playlist_track_press_was_selected_{};
    bool playlist_track_press_ctrl_{};
    bool playlist_track_press_shift_{};
    PlaylistRatingGesture playlist_rating_gesture_;
    POINT playlist_track_drag_origin_{};
    POINT playlist_list_drag_origin_{};
    std::optional<size_t> playlist_track_drag_row_;
    std::optional<size_t> playlist_track_drop_row_;
    std::optional<size_t> playlist_list_drag_row_;
    std::optional<size_t> playlist_list_drop_index_;
    std::optional<size_t> playlist_context_list_;
    std::optional<size_t> playlist_list_edit_index_;
    bool playlist_list_edit_finishing_{};
    UINT playlist_last_sort_command_{};
    bool playlist_sort_ascending_{true};
    bool playlist_list_sort_ascending_{true};
    std::vector<playlist::Track> playlist_clipboard_tracks_;
    DWORD playlist_clipboard_sequence_{};
    std::wstring playlist_rename_pattern_{L"%(Artist) - %(Title)"};
    ShellSendToCatalog playlist_send_to_catalog_;
    struct PlaylistInfoRequest {
        // Catalogue rows and track rows can both move while the helper is
        // reading.  The numbered TTBL slot plus source identity survive both
        // operations; row_hint is only a fast-path for the common case.
        size_t playlist_slot{};
        size_t row_hint{};
        std::filesystem::path path;
        int subtrack{};
    };
    struct PlaylistInfoReceiver;
    std::deque<PlaylistInfoRequest> playlist_info_pending_;
    std::optional<PlaylistInfoRequest> playlist_info_active_;
    std::set<std::wstring> playlist_info_queued_keys_;
    std::set<std::wstring> playlist_info_unavailable_sources_;
    std::shared_ptr<PlaylistInfoReceiver> playlist_info_receiver_;
    bool playlist_info_working_{};
    std::shared_ptr<std::atomic_bool> playlist_metadata_working_;
    std::shared_ptr<std::atomic_bool> playlist_metadata_cancel_;
    int playlist_scrollbar_drag_anchor_y_{};
    int playlist_scrollbar_drag_anchor_thumb_top_{};
    std::wstring playlist_list_type_text_;
    std::wstring playlist_track_type_text_;
    DWORD playlist_list_type_tick_{};
    DWORD playlist_track_type_tick_{};
    FINDREPLACEW playlist_find_{};
    wchar_t playlist_find_text_[128]{};
    HWND playlist_find_dialog_{};
    bool playlist_find_quick_{};
    FileDropTarget* player_drop_target_{};
    FileDropTarget* playlist_drop_target_{};
    FileDropTarget* lyric_drop_target_{};
    std::filesystem::path file_dialog_initial_directory_;
    bool file_dialog_active_{};
};
} // namespace ttplayer::ui
