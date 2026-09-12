#include "ttplayer/ui/player_window.h"
#include "project_links.h"
#include "player_window_internal.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace ttplayer::ui::detail;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ShellCall {
    int count{};
    HWND owner{};
    std::wstring verb, url;
    bool null_parameters{}, null_directory{};
    int show{};
} shell_call;

HINSTANCE WINAPI RecordOpen(HWND owner, LPCWSTR verb, LPCWSTR file,
                            LPCWSTR parameters, LPCWSTR directory, int show) {
    ++shell_call.count;
    shell_call.owner = owner;
    shell_call.verb = verb ? verb : L"";
    shell_call.url = file ? file : L"";
    shell_call.null_parameters = parameters == nullptr;
    shell_call.null_directory = directory == nullptr;
    shell_call.show = show;
    return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(33));
}

void CheckDispatch() {
    Require(kProjectLinks.size() == 2, "expected two project links");
    Require(std::wstring_view(kProjectLinks[0].label) == L"Github仓库" &&
            std::wstring_view(kProjectLinks[0].url) ==
                L"https://github.com/Sonic853/TTPlayer", "repository mapping");
    Require(std::wstring_view(kProjectLinks[1].label) == L"提交反馈" &&
            std::wstring_view(kProjectLinks[1].url) ==
                L"https://github.com/Sonic853/TTPlayer/issues", "feedback mapping");
    Require(kProjectLinks[0].command != kProjectLinks[1].command,
            "duplicate commands");
    const HWND owner = GetDesktopWindow();
    for (const auto& link : kProjectLinks) {
        Require(FindProjectLink(link.command) == &link, "command lookup");
        Require(link.command <= 0xffff, "command must fit WM_COMMAND");
        const int before = shell_call.count;
        Require(OpenProjectLink(owner, link.command, RecordOpen), "unhandled link");
        Require(shell_call.count == before + 1 && shell_call.owner == owner &&
                shell_call.verb == L"open" && shell_call.url == link.url &&
                shell_call.null_parameters && shell_call.null_directory &&
                shell_call.show == SW_SHOWNORMAL, "browser dispatch arguments");
    }
    const int before = shell_call.count;
    Require(!OpenProjectLink(owner, 0, RecordOpen) &&
            !OpenProjectLink(owner, kMenuRelatedLinks, RecordOpen) &&
            shell_call.count == before, "unrelated command opened a browser");
}

struct ResourceModule {
    HMODULE module;
    explicit ResourceModule(const std::filesystem::path& path)
        : module(LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        Require(module != nullptr, "could not load resource DLL");
    }
    ~ResourceModule() { FreeLibrary(module); }
};

struct Menu {
    HMENU handle;
    ~Menu() { if (handle) DestroyMenu(handle); }
};

struct Canvas {
    static constexpr int width = 180, height = 24;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap{};
    HGDIOBJ old{};
    DWORD* pixels{};
    Canvas() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0);
        Require(dc && bitmap, "menu canvas allocation");
        old = SelectObject(dc, bitmap);
    }
    ~Canvas() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
};

// Read an independent source mask before ImageList_AddMasked or HICON export.
std::array<unsigned char, 256> SourceMask(HMODULE resources, UINT command) {
    for (const UINT strip : {0x82U, 0x83U, 0x94U}) {
        const auto resource = FindResourceW(resources, MAKEINTRESOURCEW(strip),
                                             MAKEINTRESOURCEW(241));
        const auto* words = static_cast<const WORD*>(
            LockResource(LoadResource(resources, resource)));
        if (!words) continue;
        int frame = 0;
        for (WORD row = 0; row < words[3]; ++row) {
            if (words[4 + row] == command) {
                std::array<unsigned char, 256> mask{};
                const HBITMAP bitmap = LoadBitmapW(resources, MAKEINTRESOURCEW(strip));
                const HDC dc = CreateCompatibleDC(nullptr);
                Require(bitmap && dc, "reference toolbar bitmap");
                const auto old = SelectObject(dc, bitmap);
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x)
                        mask[y * 16 + x] = GetPixel(dc, frame * 16 + x, y) !=
                                          RGB(192, 192, 192);
                SelectObject(dc, old); DeleteDC(dc); DeleteObject(bitmap);
                return mask;
            }
            if (words[4 + row]) ++frame;
        }
    }
    throw std::runtime_error("reference command image missing");
}
} // namespace

namespace ttplayer::testing {
struct SkinRebindAccess {
    static void CheckImage(ui::PlayerWindow& player, const ui::PlayerWindow::MenuVisualItem& visual,
                           HMODULE resources, UINT source_command) {
        Require(visual.image >= 0, "command has no icon binding");
        const bool toolbar_image = source_command != kCmdOptions;
        const auto mask = toolbar_image ? SourceMask(resources, source_command) :
            std::array<unsigned char, 256>{};
        if (toolbar_image &&
            (static_cast<size_t>(visual.image) >= player.popup_menu_image_masks_.size() ||
             player.popup_menu_image_masks_[visual.image] != mask)) {
            std::cerr << "command=" << std::hex << visual.command << " source="
                      << source_command << std::dec << " image=" << visual.image << '\n';
            throw std::runtime_error("source transparency mask lost");
        }
        for (const UINT state : std::array<UINT, 5>{0U, ODS_SELECTED, ODS_DISABLED,
                                 ODS_SELECTED | ODS_DISABLED, ODS_SELECTED | ODS_CHECKED}) {
            Canvas actual, reference;
            DRAWITEMSTRUCT draw{};
            draw.CtlType = ODT_MENU;
            draw.itemID = visual.command;
            draw.itemState = state;
            draw.itemData = reinterpret_cast<ULONG_PTR>(&visual);
            draw.hDC = actual.dc;
            draw.rcItem = {0, 0, Canvas::width, Canvas::height};
            Require(player.DrawPopupMenuItem(draw), "menu icon not drawn");

            FillPopupMenuBackground(reference.dc, draw.rcItem);
            const bool disabled = (state & ODS_DISABLED) != 0;
            const bool selected = (state & ODS_SELECTED) != 0 && !disabled;
            const bool checked = (state & ODS_CHECKED) != 0;
            const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            if (selected) {
                SetDCBrushColor(reference.dc, RGB(193, 210, 238));
                FillRect(reference.dc, &draw.rcItem, brush);
                SetDCBrushColor(reference.dc, RGB(49, 106, 197));
                FrameRect(reference.dc, &draw.rcItem, brush);
            }
            int x = 2, y = 4;
            if (disabled) {
                const HICON icon = ImageList_GetIcon(player.popup_menu_images_, visual.image, 0);
                DrawStateW(reference.dc, nullptr, nullptr, reinterpret_cast<LPARAM>(icon),
                           0, x, y, 16, 16, DST_ICON | DSS_DISABLED);
                DestroyIcon(icon);
            } else {
                if (checked) {
                    RECT frame{1, 3, 19, 21};
                    SetDCBrushColor(reference.dc, RGB(193, 210, 238));
                    FillRect(reference.dc, &frame, brush);
                    SetDCBrushColor(reference.dc, RGB(49, 106, 197));
                    FrameRect(reference.dc, &frame, brush);
                } else if (selected) {
                    if (toolbar_image) {
                        for (int row = 0; row < 16; ++row)
                            for (int col = 0; col < 16; ++col)
                                if (mask[row * 16 + col])
                                    SetPixelV(reference.dc, x + 1 + col, y + 1 + row,
                                              RGB(128, 128, 128));
                    } else {
                        const HICON icon = ImageList_GetIcon(player.popup_menu_images_, visual.image, 0);
                        DrawStateW(reference.dc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)),
                            nullptr, reinterpret_cast<LPARAM>(icon), 0,
                            x + 1, y + 1, 16, 16, DST_ICON | DSS_MONO);
                        DestroyIcon(icon);
                    }
                    --x; --y;
                }
                ImageList_Draw(player.popup_menu_images_, visual.image, reference.dc,
                               x, y, ILD_TRANSPARENT);
            }
            GdiFlush();
            for (int row = 0; row < Canvas::height; ++row)
                for (int col = 0; col < 20; ++col)
                    Require((actual.pixels[row * Canvas::width + col] & 0xffffff) ==
                            (reference.pixels[row * Canvas::width + col] & 0xffffff),
                            "menu icon/shadow pixels differ from source-mask reference");
        }
    }

    static void CheckMenu(HMODULE resources) {
        settings::Settings settings;
        settings.general.send_title_to_msn = false;
        settings.general.tray_icon = false;
        settings.general.menu_tips = true;
        ui::PlayerWindow player(settings);
        player.instance_ = GetModuleHandleW(nullptr);
        player.SetSkinResourceModule(resources);
        // Normal startup loads at least one playlist before menus are built.
        // Use only an in-memory fixture here, not the user's saved lists.
        player.playlists_.NewList(L"Menu test");
        // Build the real resource-backed menu without launching playback,
        // showing a popup or touching the user's configuration.
        for (int repeat = 0; repeat < 2; ++repeat) {
            Menu root{player.BuildContextMenu()};
            Require(root.handle != nullptr, "main menu missing");
            MENUITEMINFOW parent{sizeof(parent)};
            parent.fMask = MIIM_SUBMENU | MIIM_STATE;
            Require(GetMenuItemInfoW(root.handle, kMenuRelatedLinks, FALSE, &parent),
                    "related links placeholder missing");
            Require(parent.hSubMenu && GetMenuItemCount(parent.hSubMenu) == 2,
                    "related links submenu must contain exactly two entries");
            Require((parent.fState & MFS_DISABLED) == 0, "parent disabled");
            for (size_t index = 0; index < kProjectLinks.size(); ++index) {
                const auto& link = kProjectLinks[index];
                wchar_t text[128]{};
                GetMenuStringW(parent.hSubMenu, static_cast<UINT>(index), text,
                               static_cast<int>(std::size(text)), MF_BYPOSITION);
                Require(std::wstring_view(text) == link.label, "menu label mismatch");
                Require(GetMenuItemID(parent.hSubMenu, static_cast<int>(index)) ==
                        link.command, "menu command mismatch");
                Require((GetMenuState(parent.hSubMenu, link.command, MF_BYCOMMAND) &
                        (MF_GRAYED | MF_DISABLED)) == 0, "link disabled");
                Require(player.MenuToolTipText(link.command) == link.url,
                        "link tooltip mismatch");
            }
            player.BeginPopupMenuStyle(root.handle, true);
            for (const auto& link : kProjectLinks) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_FTYPE | MIIM_DATA;
                Require(GetMenuItemInfoW(parent.hSubMenu, link.command, FALSE, &item) &&
                        (item.fType & MFT_OWNERDRAW), "popup style missing");
                const auto* visual = player.FindPopupMenuItem(item.dwItemData);
                Require(visual && visual->text == link.label &&
                        visual->command == link.command, "styled link lost its text/command");
                CheckImage(player, *visual, resources, link.image_command);
            }
            // Check every main toolbar-backed item, including submenu headers.
            for (const auto& mapping : {
                    std::pair{kCmdOptions, kCmdOptions},
                    std::pair{0x009dU, 0x009dU}, std::pair{0x008dU, 0x7d00U},
                    std::pair{0x008eU, 0x008eU}, std::pair{0x008fU, 0x008fU},
                    std::pair{0x0090U, 0x0090U}, std::pair{0x0091U, 0x0091U},
                    std::pair{0x0092U, 0x0092U}, std::pair{0x0093U, 0x0093U},
                    std::pair{0x0096U, 0x0096U}, std::pair{0x0097U, 0x0097U},
                    std::pair{0x009bU, 0x009bU}, std::pair{0x0186U, 0x0186U},
                    std::pair{0x8039U, 0x8039U}, std::pair{0x7dd5U, 0x7dd5U},
                    std::pair{0xe141U, 0xe141U}}) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_DATA;
                Require(GetMenuItemInfoW(root.handle, mapping.first, FALSE, &item),
                        "main command missing");
                const auto* visual = player.FindPopupMenuItem(item.dwItemData);
                Require(visual != nullptr, "main command style missing");
                CheckImage(player, *visual, resources, mapping.second);
            }
            const auto count = ImageList_GetImageCount(player.popup_menu_images_);
            player.EnsurePopupMenuImages();
            Require(count == ImageList_GetImageCount(player.popup_menu_images_),
                    "reopening popup duplicated icons");
            player.EndPopupMenuStyle();
        }
        player.settings_.general.menu_tips = false;
        Require(player.MenuToolTipText(kProjectLinks[0].command).empty(),
                "menu tooltip preference ignored");
    }
};
} // namespace ttplayer::testing

int wmain(int argc, wchar_t** argv) {
    try {
        CheckDispatch();
        const auto root = argc > 1 ? std::filesystem::path(argv[1]) :
                                    std::filesystem::current_path();
        int checked = 0;
        // Non-skin behavior is recovered only from 5.7.9, not TTPlayer6120.
        for (const auto& relative : {L"ttpres.dll",
                                    L"rebuild/build/Release/ttpres.dll"}) {
            const auto path = root / relative;
            if (!std::filesystem::exists(path)) {
                std::wcout << L"SKIP optional resource menu: " << path << L'\n';
                continue;
            }
            ResourceModule resources(path);
            ttplayer::testing::SkinRebindAccess::CheckMenu(resources.module);
            ++checked;
        }
        std::cout << "PASS project links, root menu icons and five-state pixel/shadow checks: "
                  << checked << " resource variants (no browser launched)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
