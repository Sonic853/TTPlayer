#include "ttplayer/settings/settings.h"
#include "ttplayer/skin/skin.h"

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary);
    Require(static_cast<bool>(file.write(data.data(), data.size())), "cannot write fixture");
}
using Colors = std::array<COLORREF, 16>;
template<class Playlist, class Lyric, class Visual>
Colors Collect(const Playlist& p, const Lyric& l, const Visual& v) {
    return {p.text_color, p.highlight_color, p.number_color, p.duration_color,
        p.selected_color, p.background_color, p.alternate_background_color,
        l.text_color, l.highlight_color, l.background_color, v.spectrum_top_color,
        v.spectrum_bottom_color, v.spectrum_middle_color, v.spectrum_peak_color,
        v.blur_scope_color, v.text_color};
}
Colors FixtureColors() {
    Colors result;
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = RGB(17 + i, 67 + i, 117 + i);
    return result;
}
Colors OriginalColors() {
    return {RGB(139,186,198), RGB(255,255,255), RGB(139,186,198), RGB(139,186,198),
        RGB(136,170,203), RGB(75,103,130), RGB(64,91,118), RGB(139,186,198),
        RGB(255,255,255), RGB(49,71,91), RGB(39,67,95), RGB(39,67,95),
        RGB(39,67,95), RGB(39,67,95), RGB(39,67,95), RGB(39,67,95)};
}
std::string Hex(COLORREF color) {
    char text[8]{};
    sprintf_s(text, "#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}
void U16(std::string& data, unsigned value) {
    data.push_back(static_cast<char>(value)); data.push_back(static_cast<char>(value >> 8));
}
void U32(std::string& data, unsigned value) { U16(data, value); U16(data, value >> 16); }
unsigned Crc(const std::string& data) {
    unsigned crc = ~0U;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return ~crc;
}
using Entries = std::vector<std::pair<std::string, std::string>>;
std::string Zip(const Entries& entries) {
    std::string data, directory;
    for (const auto& [name, content] : entries) {
        const auto offset = static_cast<unsigned>(data.size());
        const auto size = static_cast<unsigned>(content.size());
        const auto crc = Crc(content);
        U32(data, 0x04034b50); U16(data, 20); U16(data, 0); U16(data, 0);
        U32(data, 0); U32(data, crc); U32(data, size); U32(data, size);
        U16(data, static_cast<unsigned>(name.size())); U16(data, 0);
        data += name; data += content;
        U32(directory, 0x02014b50); U16(directory, 20); U16(directory, 20);
        U16(directory, 0); U16(directory, 0); U32(directory, 0); U32(directory, crc);
        U32(directory, size); U32(directory, size); U16(directory, static_cast<unsigned>(name.size()));
        U16(directory, 0); U16(directory, 0); U16(directory, 0); U16(directory, 0);
        U32(directory, 0); U32(directory, offset); directory += name;
    }
    const auto offset = static_cast<unsigned>(data.size());
    data += directory;
    U32(data, 0x06054b50); U16(data, 0); U16(data, 0);
    U16(data, static_cast<unsigned>(entries.size())); U16(data, static_cast<unsigned>(entries.size()));
    U32(data, static_cast<unsigned>(directory.size())); U32(data, offset); U16(data, 0);
    return data;
}
Entries Descriptors() {
    const auto colors = FixtureColors();
    const char* names[] = {"Color_Text", "Color_Hilight", "Color_Number", "Color_Duration",
        "Color_Select", "Color_Bkgnd", "Color_Bkgnd2", "TextColor", "HilightColor", "BkgndColor",
        "SpectrumTopColor", "SpectrumBtmColor", "SpectrumMidColor", "SpectrumPeakColor",
        "BlurScopeColor", "TextColor"};
    const auto attributes = [&](size_t begin, size_t end) {
        std::string xml;
        for (size_t i = begin; i < end; ++i)
            xml += " " + std::string(names[i]) + "=\"" + Hex(colors[i]) + "\"";
        return xml;
    };
    return {{"Playlist.xml", "<ttplayer_playlist><PlayList" + attributes(0, 7) + "/></ttplayer_playlist>"},
        {"Lyric.xml", "<ttplayer_lyric><Lyric" + attributes(7, 10) + "/></ttplayer_lyric>"},
        {"Visual.xml", "<ttplayer_visual><Visual" + attributes(10, 16) + "/></ttplayer_visual>"}};
}
void ReplaceZip(const fs::path& dll, const Entries& entries) {
    auto archive = Zip(entries);
    const HANDLE update = BeginUpdateResourceW(dll.c_str(), TRUE);
    Require(update != nullptr, "cannot update fixture DLL");
    if (!UpdateResourceW(update, L"ZIP", L"<DEFAULT_SKIN>", 2052, archive.data(),
                         static_cast<DWORD>(archive.size()))) {
        EndUpdateResourceW(update, TRUE);
        throw std::runtime_error("cannot write fixture ZIP resource");
    }
    Require(EndUpdateResourceW(update, FALSE) != FALSE, "cannot commit fixture resource");
}
void RunChild(const fs::path& exe, const std::wstring& mode, const fs::path& cwd) {
    std::wstring command = L"\"" + exe.wstring() + L"\" --child " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, cwd.c_str(), &startup, &child) != FALSE, "cannot start palette child");
    CloseHandle(child.hThread);
    const auto waited = WaitForSingleObject(child.hProcess, 20000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &code);
    else TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hProcess);
    Require(code == 0, "isolated DLL palette child failed");
}
void CheckChild(const std::wstring& mode) {
    const auto expected = mode == L"original" ? OriginalColors() : mode == L"fixture" ? FixtureColors() :
        Collect(skin::PlaylistColors{}, skin::LyricColors{}, skin::VisualColors{});
    const auto& defaults = skin::DefaultSkinColors();
    Require(Collect(defaults.playlist, defaults.lyric, defaults.visual) == expected,
            "cached palette does not match EXE-local DLL");
    settings::Settings settings;
    Require(Collect(settings.playlist, settings.lyric, settings.visual) == expected,
            "Settings constructors do not use DLL colors");
    skin::PlaylistSkin list; skin::LyricSkin lyric;
    Require(Collect(list, lyric, settings.visual) == expected, "skin constructors differ from settings");
    settings.playlist.text_color = RGB(1, 2, 3);
    settings.lyric.background_color = RGB(4, 5, 6);
    settings.visual.text_color = RGB(7, 8, 9);
    settings = settings::Settings{};
    Require(Collect(settings.playlist, settings.lyric, settings.visual) == expected,
            "Reset All did not restore immutable DLL baseline");
    Require(settings.visual.type == 2 && settings.visual.frames_per_second == 25 &&
            settings.lyric.fullscreen_text_color == RGB(0, 128, 192),
            "DLL palette changed independent mode preferences");
}
void SparseSkin(const fs::path& directory) {
    fs::create_directories(directory);
    BITMAPFILEHEADER file{0x4d42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 64,
        0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = info.biHeight = 4;
    info.biPlanes = 1; info.biBitCount = 32;
    std::string bitmap(reinterpret_cast<const char*>(&file), sizeof(file));
    bitmap.append(reinterpret_cast<const char*>(&info), sizeof(info)); bitmap.append(64, '\0');
    Write(directory / L"base.bmp", bitmap);
    Write(directory / L"Skin.xml", "<skin version=\"2\" name=\"sparse\"><player_window image=\"base.bmp\"/>"
        "<playlist_window image=\"base.bmp\"><playlist position=\"0,0,4,4\"/></playlist_window>"
        "<lyric_window image=\"base.bmp\"><lyric position=\"0,0,4,4\"/></lyric_window></skin>");
    settings::Settings seed;
    seed.playlist.text_color = RGB(1,2,3); seed.playlist.background_color = RGB(4,5,6);
    seed.playlist.selected_color = RGB(7,8,9); seed.lyric.text_color = RGB(10,11,12);
    seed.lyric.background_color = RGB(13,14,15);
    auto absent = skin::LegacySkin::Load(directory, &seed);
    Require(absent.Valid() && absent.Playlist().text_color == seed.playlist.text_color &&
        absent.Playlist().selected_color == seed.playlist.selected_color &&
        absent.Lyric().background_color == seed.lyric.background_color, "missing descriptors did not inherit current colors");
    Write(directory / L"Playlist.xml", "<ttplayer_playlist><PlayList Color_Text=\"#123456\" "
        "Color_Bkgnd=\"invalid\"/></ttplayer_playlist>");
    Write(directory / L"Lyric.xml", "<ttplayer_lyric><Lyric TextColor=\"#654321\"/></ttplayer_lyric>");
    auto partial = skin::LegacySkin::Load(directory, &seed);
    Require(partial.Playlist().text_color == RGB(0x12,0x34,0x56) &&
        partial.Playlist().background_color == seed.playlist.background_color &&
        partial.Playlist().selected_color == GetSysColor(COLOR_HIGHLIGHT) &&
        partial.Lyric().text_color == RGB(0x65,0x43,0x21) &&
        partial.Lyric().background_color == seed.lyric.background_color, "sparse package inheritance differs from original");
    Write(directory / L"Playlist.xml", "<ttplayer_playlist><PlayList Color_Select=\"invalid\"/></ttplayer_playlist>");
    Require(skin::LegacySkin::Load(directory, &seed).Playlist().selected_color == GetSysColor(COLOR_HIGHLIGHT),
            "invalid package selection color did not use system fallback");
    Write(directory / L"profile.xml", "<ttplayer><PlayList Color_Select=\"invalid\"/></ttplayer>");
    Require(settings::LoadSkinVisualProfile(directory / L"profile.xml", seed.player, seed.playlist,
        seed.lyric, seed.visual) && seed.playlist.selected_color == RGB(7,8,9),
        "user profile must preserve its current selection color for an invalid field");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--child") { CheckChild(argv[2]); return 0; }
        Require(argc == 2, "expected original runtime directory");
        const fs::path source = argv[1];
        Require(fs::exists(source / L"ttpres.dll") && fs::exists(source / L"ttpcomm.dll"),
                "original fixture DLLs absent");
        wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
        const auto root = fs::temp_directory_path() / (L"TTPlayer-default-colors-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount()));
        fs::create_directories(root);
        // Put an unrelated color DLL in the CWD. All children must ignore it.
        fs::copy_file(source / L"ttpres.dll", root / L"ttpres.dll");
        ReplaceZip(root / L"ttpres.dll", Descriptors());
        for (const auto mode : {L"original", L"fixture", L"missing", L"broken"}) {
            const auto directory = root / mode; fs::create_directories(directory);
            const auto exe = directory / L"palette-test.exe"; fs::copy_file(self, exe);
            fs::copy_file(source / L"ttpcomm.dll", directory / L"ttpcomm.dll");
            if (std::wstring(mode) != L"missing") {
                fs::copy_file(source / L"ttpres.dll", directory / L"ttpres.dll");
                if (std::wstring(mode) == L"fixture") ReplaceZip(directory / L"ttpres.dll", Descriptors());
                if (std::wstring(mode) == L"broken") Write(directory / L"ttpres.dll", "invalid PE");
            }
            RunChild(exe, mode, root);
        }
        const auto partial = root / L"partial.dll";
        fs::copy_file(source / L"ttpres.dll", partial);
        ReplaceZip(partial, {{"Playlist.xml", "<ttplayer_playlist><PlayList Color_Text=\"#123456\" "
            "Color_Bkgnd=\"bad\"/></ttplayer_playlist>"}, {"Lyric.xml", "<ttplayer_lyric>"},
            {"Visual.xml", "<ttplayer_visual><Visual TextColor=\"#654321\"/></ttplayer_visual>"}});
        HMODULE module = LoadLibraryExW(partial.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
        Require(module != nullptr, "partial resource DLL load failed");
        const auto colors = skin::ReadDefaultSkinColors(module, nullptr); FreeLibrary(module);
        Require(colors.playlist.text_color == RGB(0x12,0x34,0x56) &&
            colors.playlist.background_color == skin::PlaylistColors{}.background_color &&
            colors.playlist.selected_color == GetSysColor(COLOR_HIGHLIGHT) &&
            colors.lyric.background_color == skin::LyricColors{}.background_color &&
            colors.visual.text_color == RGB(0x65,0x43,0x21), "damaged DLL descriptor fallback failed");
        SparseSkin(root / L"sparse");
        std::cout << "DLL palette isolation / replacement / reset / fallback / sparse skin tests passed\n";
        // Keep isolated evidence files, without changing the user's runtime.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
