#pragma once

// Shared implementation details for the split PlayerWindow translation units.
// These declarations do not alter the public PlayerWindow ABI.

#include "ttplayer/ui/player_window.h"

#include <chrono>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace ttplayer::ui::detail {
// The native skin rebinders (0046D0C1/0044E5CE/0046ACE2/0046BCBE)
// suppress redraw on already visible windows while rebinding their controls.
// WM_SETREDRAW(TRUE) would show an originally hidden window, so only pair it
// for windows whose own WS_VISIBLE bit was initially present.
class ScopedSkinRedraw {
public:
    explicit ScopedSkinRedraw(HWND window) : window_(window),
        suspended_(window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0) {
        if (suspended_) SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    }
    ~ScopedSkinRedraw() { Resume(); }
    void Resume() {
        if (suspended_ && IsWindow(window_)) {
            SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        suspended_ = false;
    }
    ScopedSkinRedraw(const ScopedSkinRedraw&) = delete;
    ScopedSkinRedraw& operator=(const ScopedSkinRedraw&) = delete;
private:
    HWND window_{};
    bool suspended_{};
};

constexpr wchar_t kWindowClass[] = L"TTPlayer_PlayerWnd";
constexpr wchar_t kPlaybackTipWindowClass[] = L"TTPlayer_MessageTipWnd";
constexpr wchar_t kPlaylistWindowClass[] = L"TTPlayer_PlayListWnd";
constexpr wchar_t kPlaylistTreeClass[] = L"TreeCtrl";
constexpr wchar_t kPlaylistListClass[] = L"ListCtrl";
constexpr wchar_t kLyricWindowClass[] = L"TTPlayer_LyricWnd";
constexpr wchar_t kLyricControlClass[] = L"LyricCtrl";
constexpr wchar_t kLyricEditorToolbarClass[] = L"WTL_ToolBar";
constexpr wchar_t kEqualizerWindowClass[] = L"TTPlayer_EqualizerWnd";
constexpr wchar_t kEqualizerButtonClass[] = L"SkinButton";
constexpr wchar_t kEqualizerSliderClass[] = L"SkinSlider";
constexpr wchar_t kVisualWindowClass[] = L"VisualCtrl";
constexpr COLORREF kBackground = RGB(24, 29, 36);
constexpr COLORREF kPanel = RGB(34, 41, 50);
constexpr COLORREF kAccent = RGB(48, 184, 136);
constexpr COLORREF kText = RGB(232, 238, 242);
constexpr COLORREF kMutedText = RGB(149, 162, 173);
constexpr int kOpen = 1001;
constexpr int kPrevious = 1002;
constexpr int kPlayPause = 1003;
constexpr int kNext = 1004;
constexpr int kStop = 1005;
constexpr int kPlaylist = 1101;
constexpr int kProgress = 1102;
constexpr int kVolume = 1103;
constexpr UINT_PTR kUiTimer = 1;
constexpr UINT kUiRefreshIntervalMs = 250;
// One completed private-reader metadata probe is marshalled back to the UI
// thread with this message.  The original drains one queued CPlayItem from
// its WM_NULL path at 00481759; a private message keeps the rebuilt decoder
// work off that thread while preserving the one-item-at-a-time transaction.
constexpr UINT kMsgPlaylistInfoReady = WM_APP + 0x313;
// A directory walk never calls back into PlayerWindow.  Its immutable result
// is handed to the UI thread with this message and discarded when a newer
// refresh generation has superseded it.
constexpr UINT kMsgMediaLibraryReady = WM_APP + 0x314;
// CDirectoryWatcher posts 0x7F5 to CPlayListWnd in 0041AE8D/0047B52E,
// carrying FILE_ACTION_* in wParam and a CoTaskMem path in lParam.
// Keep the rebuilt message private so it cannot collide with resource/control
// identifiers, while preserving the same asynchronous window-thread handoff.
constexpr UINT kMsgMediaLibraryChanged = WM_APP + 0x315;
constexpr int kFullscreenEscapeHotkey = 0x7f0;
// CPlayerWnd::OnCreate writes 0x7e8 into NOTIFYICONDATA::uCallbackMessage.
constexpr UINT kTrayCallbackMessage = 0x7e8;
constexpr UINT kTrayIconIdentifier = 1;
// CLyricCtrl uses timer 0x7b independently from CPlayerWnd's timer 10.
// FUN_0043EDE6 selects 20 ms for the horizontal stream and 50 ms for the
// vertical row layout.
constexpr UINT_PTR kLyricAnimationTimer = 0x7b;
constexpr UINT kLyricHorizontalIntervalMs = 20;
constexpr UINT kLyricVerticalIntervalMs = 50;
// CScrollingStatic's three timers recovered from FUN_004092FC/FUN_0040989E:
// 7 waits between queue entries, 8 moves the next line vertically, and 9
// performs the one-pixel horizontal overflow animation.
constexpr UINT_PTR kInfoItemTimer = 7;
constexpr UINT_PTR kInfoTransitionTimer = 8;
constexpr UINT_PTR kInfoScrollTimer = 9;
// FUN_0046228D arms the daily auto-shutdown check with timer identifier 0x0c.
constexpr UINT_PTR kAutoShutdownTimer = 0x0c;
// WTL's popup-menu tooltip filter uses this exact timer identifier in
// FUN_0046F80C and asks the common-control tooltip for TTDT_INITIAL.
constexpr UINT_PTR kSkinMenuToolTipTimer = 0x215;
// The skinned ListCtrl scrollbar uses the usual immediate action, delayed
// first repeat and 50 ms steady repeat.  It owns a timer on CPlayListWnd,
// separate from every main-window animation timer above.
constexpr UINT_PTR kPlaylistScrollbarRepeatTimer = 0x216;
constexpr UINT kPlaylistScrollbarRepeatIntervalMs = 50;
// FUN_0040C0C9/0040C0F6 advances the original alpha clock in roughly
// five-alpha-unit samples. A window timer keeps the recovered transition on
// the UI message pump instead of blocking it with Sleep.
constexpr UINT_PTR kSkinWindowFadeTimer = 0x217;
constexpr UINT kSkinWindowFadeIntervalMs = 5;
// 00461ADB used a message-pumping wait for CSoundFadeOut. The reconstruction
// polls the fade's lock-free completion flag from the ordinary window loop so
// paint/input dispatch never enters Sleep or a nested message pump.
constexpr UINT_PTR kCloseAudioFadeTimer = 0x218;
constexpr UINT kCloseAudioFadeIntervalMs = 10;
constexpr UINT kInfoAnimationIntervalMs = 40;
// CPlayerWnd overwrites CScrollingStatic's constructor default (3) with
// DAT_005478AC.  The original 5.7.9 process initializes that WORD to 5.
constexpr UINT kInfoItemIntervalMs = 5000;
constexpr int kInfoEndpointHoldTicks = 25;

// Original TTPlayer 5.7.9 command and menu resource identifiers recovered from
// ttpres.dll and CPlayerWnd's WM_COMMAND dispatcher.
constexpr UINT kMenuMain = 138;             // 0x8a
constexpr UINT kMenuPlaylistToolbar = 139;  // 0x8b
constexpr UINT kMenuPlayList = 151;         // 0x97
constexpr UINT kMenuPlaylistItem = 152;     // 0x98
constexpr UINT kMenuPlaylistItems = 153;    // 0x99
constexpr UINT kMenuPlaylistLists = 156;    // 0x9c
constexpr UINT kMenuLibrary = 159;          // 0x9f
constexpr UINT kMenuSkin = 155;             // 0x9b
constexpr UINT kMenuTransparency = 147;     // 0x93
constexpr UINT kMenuLyricEditor = 148;       // 0x94
constexpr UINT kMenuLyricDisplay = 143;      // 0x8f
constexpr UINT kMenuEqualizer = 144;         // 0x90
constexpr UINT kMenuVisual = 145;            // 0x91
constexpr UINT kMenuFullscreen = 390;        // 0x186
constexpr UINT kCmdPlay = 32000;
constexpr UINT kCmdPause = 32001;
constexpr UINT kCmdStopPlayback = 32002;
constexpr UINT kCmdSeekBack = 32003;
constexpr UINT kCmdSeekForward = 32004;
constexpr UINT kCmdPrevious = 32005;
constexpr UINT kCmdNext = 32006;
constexpr UINT kCmdMute = 32007;
constexpr UINT kCmdPlayCd = 32008;
constexpr UINT kCmdPlayUrl = 32009;
constexpr UINT kCmdVolumeUp = 32010;
constexpr UINT kCmdVolumeDown = 32011;
// The main-window File information command.  FUN_00464A94 maps this to the
// playlist/library property-sheet command 0x7EF6 after leaving full screen.
constexpr UINT kCmdFileProperties = 0x7d67;
constexpr UINT kCmdOpenFile = 57601;
constexpr UINT kCmdCloseFile = 57602;
constexpr UINT kCmdOptions = 57664;
constexpr UINT kCmdExit = 57665;
// CPlayerWnd routes each surface's local Options item to the same modeless
// property sheet while selecting a different page (0045886D..004588A6).
constexpr UINT kCmdSkinOptions = 31000;
constexpr UINT kCmdPlaylistOptions = 0x7efd;
constexpr UINT kCmdLibraryOptions = 0x7fef;
constexpr UINT kCmdDesktopLyricOptions = 0x8046;
constexpr UINT kCmdLibraryDownloadOptions = 0x0903;
// CPlayerWnd's private settings-change dispatcher.  Option pages send a
// subsystem mask in wParam and the originating control (or a negative
// lifecycle sentinel) in lParam.
constexpr UINT kMsgApplyOptions = 0x7f0;
constexpr UINT kMsgShowOptionsControl = 0x7f4;
constexpr UINT kCmdMiniMode = 0x7dd4;
constexpr UINT kCmdShowElapsedTime = 0x7ddd;
constexpr int kVisualControlId = 0x7ddc;
constexpr UINT kCmdVisualNone = 0x8085;
constexpr UINT kCmdVisualDream = 0x8086;
constexpr UINT kCmdVisualSpectrum = 0x8087;
constexpr UINT kCmdVisualScope = 0x8088;
constexpr UINT kCmdVisualCover = 0x8089;
constexpr UINT kCmdVisualOptions = 0x8084;
constexpr UINT kCmdVisualFirst = kCmdVisualNone;
constexpr UINT kCmdVisualLast = kCmdVisualCover;
constexpr UINT kCmdFullscreenToggle = 0x7de6;
constexpr UINT kCmdFullscreenExit = 0x7de7;
constexpr UINT kCmdFullscreenLyrics = 0x7de8;
constexpr UINT kCmdFullscreenVisual = 0x7de9;
constexpr UINT kCmdFullscreenAll = 0x7dea;
// Separate from native commands and dynamic track/skin menu IDs.
constexpr UINT kMenuFullscreenMonitor = 0xb600;
constexpr UINT kCmdFullscreenMonitorFirst = 0xb610;
constexpr UINT kCmdFullscreenMonitorLast = 0xb64f;
constexpr UINT kCmdMinimize = 32213;
constexpr UINT kCmdAlwaysOnTop = 32215;
constexpr UINT kCmdShowLyrics = 0x7d64;
constexpr UINT kCmdShowEqualizer = 0x7d65;
constexpr UINT kCmdShowPlaylist = 0x7d66;
constexpr UINT kCmdShowBrowser = 0x7d68;
constexpr UINT kCmdLyricReload = 0x8024;
constexpr UINT kCmdLyricOptions = 0x8023;
constexpr UINT kCmdLyricReturn = 0x8020;
constexpr UINT kCmdLyricSaveAs = 0x8021;
constexpr UINT kCmdLyricSave = 0x8022;
constexpr UINT kCmdLyricAdjustCurrentEarlier = 0x8025;
constexpr UINT kCmdLyricAdjustCurrentLater = 0x8026;
constexpr UINT kCmdLyricAdjustFollowingEarlier = 0x8027;
constexpr UINT kCmdLyricAdjustFollowingLater = 0x8028;
constexpr UINT kCmdLyricAdjustAllEarlier = 0x8029;
constexpr UINT kCmdLyricAdjustAllLater = 0x802a;
constexpr UINT kCmdLyricAdjustAllDialog = 0x802b;
constexpr UINT kCmdLyricEdit = 0x802c;
constexpr UINT kCmdLyricAssociate = 0x802d;
constexpr UINT kCmdLyricUnassociate = 0x802e;
constexpr UINT kCmdLyricDownload = 0x802f;
constexpr UINT kCmdLyricEmbeddedRead = 0x8030;
constexpr UINT kCmdLyricEmbeddedWrite = 0x8031;
constexpr UINT kCmdLyricEmbeddedDelete = 0x8032;
constexpr UINT kCmdLyricTopMost = 0x8033;
constexpr UINT kCmdLyricCopy = 0x8034;
constexpr UINT kCmdLyricUpload = 0x8035;
constexpr UINT kCmdDesktopLyricReturn = 0x8038;
constexpr UINT kCmdDesktopLyrics = 0x8039;
// CPlayerWnd_ShowMainContextMenu (0045DFBA) replaces 0x8039 with one of
// these two commands while the independent desktop-lyric surface is active.
// Keeping separate action IDs is important once the locked surface itself is
// mouse-transparent and can no longer expose its own toolbar.
constexpr UINT kCmdDesktopLyricLock = 0x8040;
constexpr UINT kCmdDesktopLyricUnlock = 0x8041;
constexpr UINT kCmdLyricScrollMode = 0x409;
constexpr UINT kCmdLyricFadeHighlight = 0x861;
constexpr UINT kCmdLyricKaraoke = 0x866;
constexpr UINT kCmdLyricTransparent = 0x867;
constexpr UINT kCmdLyricMouseWheel = 0x8036;
constexpr UINT kCmdLyricEditorInsertTag = 0x8048;
constexpr UINT kCmdLyricEditorReplaceTag = 0x8049;
constexpr UINT kCmdLyricEditorDeleteTag = 0x804a;
constexpr UINT kCmdLyricEditorEarlier = 0x804b;
constexpr UINT kCmdLyricEditorLater = 0x804c;
constexpr UINT kCmdLyricEditorExpand = 0x804d;
constexpr UINT kCmdLyricEditorCompress = 0x804e;
constexpr UINT kCmdLyricEditorNewLine = 0x804f;
constexpr UINT kCmdChineseTraditional = 0x86d;
constexpr UINT kCmdChineseSimplified = 0x86e;
constexpr UINT kCmdEditorCut = 0xe123;
constexpr UINT kCmdEditorCopy = 0xe122;
constexpr UINT kCmdEditorPaste = 0xe125;
constexpr UINT kCmdEditorFind = 0xe124;
constexpr UINT kCmdEditorReplace = 0xe129;
constexpr UINT kCmdEditorUndo = 0xe12b;
constexpr UINT kCmdEditorRedo = 0xe12c;
constexpr UINT kCmdEditorSelectAll = 0xe12a;
constexpr int kLyricEditorToolbarId = 0xe800;
constexpr int kLyricControlId = 0x2800;
constexpr UINT kEqCommandEnable = 0x7e2c;
constexpr UINT kEqCommandFlat = 0x7e2d;
constexpr UINT kEqCommandCustom = 0x7e2e;
constexpr UINT kEqCommandLoad = 0x7e2f;
constexpr UINT kEqCommandSave = 0x7e30;
constexpr UINT kEqCommandPresetFirst = 0x7e90;
constexpr UINT kEqCommandSurround = 0x7dfa;
constexpr int kEqHitClose = 1;
constexpr int kEqHitEnabled = 2;
constexpr int kEqHitProfile = 3;
constexpr int kEqHitReset = 4;
constexpr int kEqControlProfile = 0x64;
constexpr int kEqControlReset = 0x65;
constexpr int kEqSliderBalance = 0x66;
constexpr int kEqSliderSurround = 0x67;
constexpr int kEqSliderPreamp = 0xc8;
constexpr int kEqSliderFirstBand = 0xc9;
constexpr int kEqSliderLastBand = 0xd2;
constexpr UINT_PTR kPlaylistToolFirst = 0x10000;
constexpr int kPlaylistTreeId = 0x2800;
constexpr int kPlaylistListId = 0x2801;
constexpr int kPlaylistTrackId = 0x2802;
constexpr UINT kCmdPlayModeFirst = 32581;
constexpr UINT kCmdPlayModeLast = 32585;
constexpr UINT kCmdDefaultSkin = 31001;
constexpr UINT kCmdFirstSkin = 31002;
constexpr UINT kCmdFirstTrack = 10000;
constexpr UINT kCmdFirstAlpha = 32200;
constexpr UINT kCmdLastAlpha = 32209;
constexpr UINT kPlaylistPlay = 0x7ef5;
constexpr UINT kPlaylistProperties = 0x7ef6;
constexpr UINT kPlaylistConvert = 0x7ef7;
constexpr UINT kPlaylistReplayGainScan = 0x7ef8;
constexpr UINT kPlaylistReplayGainRemove = 0x7ef9;
constexpr UINT kPlaylistFreeDb = 0x7efa;
constexpr UINT kPlaylistBrowseFile = 0x7efc;
constexpr UINT kPlaylistOpenList = 0x7eff;
constexpr UINT kPlaylistSaveList = 0x7f00;
constexpr UINT kPlaylistNewList = 0x7f01;
constexpr UINT kPlaylistDeleteList = 0x7f02;
constexpr UINT kPlaylistAddList = 0x7f03;
constexpr UINT kPlaylistRenameList = 0x7f04;
constexpr UINT kPlaylistActivateList = 0x7f05;
constexpr UINT kPlaylistSaveAllLists = 0x7f06;
constexpr UINT kPlaylistAddFile = 0x7f09;
constexpr UINT kPlaylistAddFolder = 0x7f0a;
constexpr UINT kPlaylistLocalSearch = 0x7f0b;
constexpr UINT kPlaylistAddUrl = 0x7f0c;
constexpr UINT kPlaylistOnlineSearch = 0x7f0d;
constexpr UINT kPlaylistDeleteSelected = 0x7f13;
constexpr UINT kPlaylistClear = 0x7f14;
constexpr UINT kPlaylistDeleteDuplicates = 0x7f15;
constexpr UINT kPlaylistDeleteInvalid = 0x7f16;
constexpr UINT kPlaylistDeleteFiles = 0x7f17;
constexpr UINT kPlaylistSortTitle = 0x7f1d;
constexpr UINT kPlaylistSortFile = 0x7f1e;
constexpr UINT kPlaylistSortPath = 0x7f1f;
constexpr UINT kPlaylistSortAlbum = 0x7f20;
constexpr UINT kPlaylistSortRating = 0x7f21;
constexpr UINT kPlaylistSortFileTime = 0x7f22;
constexpr UINT kPlaylistSortTrackNumber = 0x7f23;
constexpr UINT kPlaylistSortDuration = 0x7f24;
constexpr UINT kPlaylistShuffle = 0x7f25;
constexpr UINT kPlaylistSortLists = 0x7f26;
constexpr UINT kPlaylistCut = 0x7f30;
constexpr UINT kPlaylistCopy = 0x7f31;
constexpr UINT kPlaylistPaste = 0x7f32;
constexpr UINT kPlaylistMoveToList = 0x7f33;
constexpr UINT kPlaylistCopyToList = 0x7f34;
constexpr UINT kPlaylistSelectAll = 0x7f35;
constexpr UINT kPlaylistSelectNone = 0x7f36;
constexpr UINT kPlaylistSelectInvert = 0x7f37;
constexpr UINT kPlaylistFind = 0x7f39;
constexpr UINT kPlaylistFindNext = 0x7f3a;
constexpr UINT kPlaylistQuickFind = 0x7f3b;
constexpr UINT kPlaylistModeSingle = 0x7f45;
constexpr UINT kPlaylistModeRepeatOne = 0x7f46;
constexpr UINT kPlaylistModeSequential = 0x7f47;
constexpr UINT kPlaylistModeRepeatAll = 0x7f48;
constexpr UINT kPlaylistModeShuffle = 0x7f49;
constexpr UINT kPlaylistAutoSwitchList = 0x7f4a;
constexpr UINT kPlaylistPlayFollowCursor = 0x7f4b;
constexpr UINT kPlaylistRenameTitle = 0x7f4e;
constexpr UINT kPlaylistRenameArtistTitle = 0x7f4f;
constexpr UINT kPlaylistRenameTitleArtist = 0x7f50;
constexpr UINT kPlaylistRenameCustom = 0x7f51;
constexpr UINT kPlaylistLibraryMode = 0x7f4c;
constexpr UINT kPlaylistSendToFolder = 0x7f58;
constexpr UINT kPlaylistSendToFirst = 0x7f59;
constexpr UINT kPlaylistSendToLast = 0x7fbb;
constexpr UINT kPlaylistRatingFirst = 0x7fe5;
constexpr UINT kPlaylistRatingLast = 0x7fe9;
constexpr UINT kPlaylistClickRating = 0x7fea;
constexpr UINT kPlaylistDownload = 0x7feb;
constexpr UINT kPlaylistReportOnline = 0x80d1;
constexpr unsigned int kDragMove = 0x01;
constexpr unsigned int kDragRight = 0x10;
constexpr unsigned int kDragBottom = 0x20;
constexpr unsigned int kDragLeft = 0x40;
constexpr unsigned int kDragTop = 0x80;

struct EqualizerPreset {
    const wchar_t* name;
    std::array<int, 10> bands;
};

// Table at TTPlayer.exe VA 0053B264.  The ninth label is intentionally kept
// as the original executable spells it.
constexpr EqualizerPreset kEqualizerPresets[] = {
    {L"Default", {4, 2, 0, -3, -6, -6, -3, 0, 3, 5}},
    {L"Pop", {3, 1, 0, -2, -4, -4, -2, 0, 1, 2}},
    {L"Rock", {-2, 0, 2, 4, -2, -2, 0, 0, 4, 4}},
    {L"Metal", {-6, 0, 0, 0, 0, 0, 4, 0, 4, 0}},
    {L"Dance", {-2, 3, 4, 1, -2, -2, 0, 0, 4, 4}},
    {L"Electron", {-6, 1, 4, -2, -2, -4, 0, 0, 6, 6}},
    {L"Country", {-2, 0, 0, 2, 2, 0, 0, 0, 4, 4}},
    {L"Jazz", {0, 0, 0, 4, 4, 4, 0, 2, 3, 4}},
    {L"Classical", {0, 8, 8, 4, 0, 0, 0, 0, 2, 2}},
    {L"Bruce", {-2, 0, 2, 1, 0, 0, 0, 0, -2, -4}},
    {L"Reminiscence", {-4, 0, 2, 1, 0, 0, 0, 0, -4, -6}},
    {L"Opera", {0, 0, 0, 4, 5, 3, 6, 3, 0, 0}},
    {L"Voice", {-4, 0, 2, 1, 0, 0, 0, 0, -4, -6}},
};
static_assert(std::size(kEqualizerPresets) == 13);

struct PlaylistGeometry {
    RECT list{};
    RECT list_titles{};
    RECT splitter{};
    RECT tracks{};
    RECT scrollbar{};
    RECT toolbar{};
    RECT close{};
    RECT title{};
    int row_height{19};
    int visible_rows{1};
    int scrollbar_width{};
};

std::filesystem::path PlayerRuntimeDirectory();
std::filesystem::path FindRuntimePath(const std::filesystem::path& relative);
HMENU DetachFirstPopup(HMENU menu);
HMENU DetachPopup(HMENU menu, int position);
HMENU ConvertMenuBarToPopup(HMENU menu);
std::wstring LoadResourceText(HMODULE module, UINT identifier);
std::wstring ResourceCommandLabel(HMODULE module, UINT identifier);
std::wstring ResourceListItem(HMODULE module, UINT identifier, size_t index);
std::wstring MenuPositionText(HMODULE module, UINT identifier, UINT position);
std::vector<wchar_t> BuildDialogFilter(
    HMODULE module,
    std::initializer_list<std::pair<UINT, std::wstring_view>> entries);
void AppendDialogFilter(std::vector<wchar_t>& result, std::wstring_view label,
                        std::wstring_view pattern);
std::wstring PatternFromDescription(std::wstring_view description);
std::wstring DialogDescription(std::wstring description);
std::vector<wchar_t> BuildAudioDialogFilter(
    HMODULE resources,
    const std::vector<plugins::ReaderFormat>& registered_readers);
void ReplaceAll(std::wstring& value, std::wstring_view needle,
                std::wstring_view replacement);
HMENU FindCommandMenu(HMENU menu, UINT command);
void EnableCommand(HMENU root, UINT command, bool enabled);
void CheckCommand(HMENU root, UINT command, bool checked);
COLORREF InterpolateMenuColor(COLORREF first, COLORREF second, int numerator,
                              int denominator);
void DrawVerticalGradient(HDC target, const RECT& bounds,
                          COLORREF first, COLORREF second);
void FillPopupMenuBackground(HDC dc, const RECT& bounds);
void TrimSingleTrackMenu(HMENU menu, const std::filesystem::path& path);
std::wstring FromUtf8OrFallback(const std::string& value,
                                const std::filesystem::path& fallback);
std::wstring DisplayName(const playlist::Track& track);
std::wstring ArtistName(const playlist::Track& track, std::wstring fallback);
bool IsPlaylistFile(const std::filesystem::path& path);
RECT PlaylistToolbarItemBounds(const skin::PlaylistSkin& layout, RECT toolbar,
                               size_t index);
void DrawPlaylistToolbarBitmap(
    HDC target, const skin::SkinBitmap& bitmap, RECT bounds,
    COLORREF transparent, std::optional<size_t> only_button = std::nullopt,
    BYTE opacity = 255, const skin::PlaylistSkin* layout = nullptr);
inline constexpr UINT_PTR kSkinControlAnimationTimer = 0x6120;
void TileBitmap(HDC target, const skin::SkinBitmap& bitmap, const RECT& bounds);
COLORREF InterpolateColor(COLORREF first, COLORREF second,
                          int numerator, int denominator);
void DrawHorizontalGradient(HDC target, const RECT& bounds,
                            COLORREF first, COLORREF second);
void DrawSolidFrame(HDC target, const RECT& bounds, COLORREF color);
void DrawBitmapPatch(HDC target, const skin::SkinImage& source, const RECT& destination,
                     const RECT& source_rect, bool tile);
void DrawResizableSkinBitmap(HDC target, const skin::SkinBitmap& bitmap,
                             RECT resize_rect, int width, int height, bool tile);
HRGN CreateColorKeyRegion(HBITMAP bitmap, int width, int height,
                          COLORREF transparent);
HRGN CreateSkinWindowRegion(const skin::SkinBitmap& bitmap, RECT resize_rect,
                           int width, int height, bool tile, COLORREF transparent);
// Independent, initial-state main-window preview (0049A6EF), never live state.
HBITMAP RenderSkinPreview(const skin::LegacySkin& source,
                          HMODULE resources = nullptr, HICON fallback_icon = nullptr);
RECT ResolveAlignedRect(RECT bounds, unsigned int alignment, SIZE native,
                        int width, int height, SIZE image_size = {});
void DrawElementFrame(HDC target, const skin::SkinElement& element, RECT bounds,
                      int state, COLORREF transparent);
PlaylistGeometry MakePlaylistGeometry(const skin::PlaylistSkin& layout,
                                      int split_on_lists, int width, int height,
                                      size_t track_count);
void ApplyPlaylistSkinDefaults(const skin::PlaylistSkin& source,
                               settings::PlaylistSettings& target);
void ApplyLyricSkinDefaults(const skin::LyricSkin& source,
                            settings::LyricSettings& target);
void SetControlFont(HWND control, HFONT font);
// Keep these nodes in the parsed skin, but hide their UI by user request.
bool IsSuppressedSkinControl(std::wstring_view name);
bool IsSkinButton(std::wstring_view name);
HFONT CreateSkinFont(const skin::SkinElement& element);
void DrawSkinText(HDC dc, const skin::SkinElement& element,
                  const wchar_t* text);
COLORREF BlendColor(COLORREF from, COLORREF to, int amount, int total);
void DrawScrollingSkinInfo(HDC dc, const skin::SkinElement& element,
                           const wchar_t* current, const wchar_t* next,
                           int horizontal_offset, int vertical_offset);
std::wstring FormatInfoDuration(std::chrono::milliseconds duration);
std::wstring FormatAudioDescription(const audio::AudioFormat& format);
std::wstring FormatLedTime(std::chrono::milliseconds position);
RECT SkinLedBounds(const skin::SkinElement& led, std::wstring_view value);
void DrawSkinLed(HDC dc, const skin::SkinElement& led, std::wstring_view value,
                 COLORREF transparent);
bool ReadEqualizerProfileFile(const std::filesystem::path& path,
                              std::array<int, 11>& values);
bool WriteEqualizerProfileFile(const std::filesystem::path& path,
                               const std::array<int, 11>& values);
// CColorSelectCtrl (0048CD60/0048CF13): the original options colour button
// opens the 48-colour popup first and only enters ChooseColor from its
// bottom "Custom..." item.
bool ShowLegacyPresetColor(
    HWND owner, HWND button, HMODULE resources, COLORREF initial,
    std::function<void(COLORREF)> on_selected);
} // namespace ttplayer::ui::detail
