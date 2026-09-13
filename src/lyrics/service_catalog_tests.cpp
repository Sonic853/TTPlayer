#include "ttplayer/lyrics/service_catalog.h"
#include "ttplayer/lyrics/lyric_http.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary); stream << text;
    Require(stream.good(), "fixture write failed");
}
lyrics::ServiceCatalog Await(std::shared_ptr<lyrics::CatalogJob> job) {
    const auto end = GetTickCount64() + 10000;
    for (;;) {
        { std::lock_guard lock(job->mutex); if (job->result) return *job->result; }
        Require(GetTickCount64() < end, "catalog worker timeout"); Sleep(10);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && (std::wstring_view(argv[1]) == L"--fetch" || std::wstring_view(argv[1]) == L"--reject-tls")) {
            settings::NetworkSettings network; network.proxy_type = 0;
            const bool reject = std::wstring_view(argv[1]) == L"--reject-tls";
            try {
                auto body = lyrics::FetchLyricHttp(argv[2], network, {});
                Require(!reject, "untrusted TLS certificate was accepted");
                Require(!body.empty(), "empty HTTPS response");
                std::cout << "PASS: certificate-verified HTTPS, " << body.size() << " bytes\n";
            } catch (const std::exception& error) {
                if (!reject || std::string(error.what()).find("12175") == std::string::npos) throw;
                std::cout << "PASS: WinHTTP rejected untrusted TLS certificate (12175)\n";
            }
            return 0;
        }
        Require(argc == 2, "expected repository root");
        const auto directory = fs::temp_directory_path() /
            (L"TTPlayer-lyric-catalog-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory);
        const auto first = directory / L"ttp_lrcsh.dll", second = directory / L"other.dll";
        fs::copy_file(fs::path(argv[1]) / L"AddIn/ttp_lrcsh.dll", first);
        fs::copy_file(first, second);
        const auto dll_bytes = Read(first);
        const auto ini = lyrics::ServiceIniPath(first), other_ini = lyrics::ServiceIniPath(second);
        const std::vector<plugins::LyricSearchProviderInfo> providers{{L"One",first},{L"Two",first},{L"Other",second}};
        auto original = Await(lyrics::ReadServiceCatalogAsync(providers));
        Require(original.error.empty() && original.entries.size() == 4 && original.files.size() == 2,
            "resource enumeration or DLL deduplication failed");
        Require(std::all_of(original.entries.begin(), original.entries.end(), [](const auto& s) {
            return s.read_only && s.module == s.storage && s.storage.extension() == L".dll";
        }), "DLL source not immutable");
        auto edit = original.entries;
        edit[0].name += L"changed";
        Require(!lyrics::SaveServiceCatalog(original, edit).error.empty(), "backend accepted DLL rename");
        edit = original.entries; edit.erase(edit.begin());
        Require(!lyrics::SaveServiceCatalog(original, edit).error.empty(), "backend accepted DLL deletion");
        edit = original.entries;
        for (int i = 0; i < 3; ++i)
            edit.push_back({L"服务 <&\" " + std::to_wstring(i), L"https://example.test/lyrics?a=1&b=2", {}, first, ini, false, 0});
        auto saved = Await(lyrics::SaveServiceCatalogAsync(original, edit));
        Require(saved.error.empty() && saved.entries.size() == 7 && fs::exists(ini) && !fs::exists(other_ini),
            "INI creation wrote wrong DLL or lost third service");
        auto readback = Await(lyrics::ReadServiceCatalogAsync(providers));
        Require(readback.error.empty() && readback.entries.size() == 7 && readback.entries[6].name == edit.back().name,
            "UTF-8/XML escaping or more-than-two service roundtrip");
        Require(std::equal(original.entries.begin(), original.entries.end(), readback.entries.begin()),
            "all DLLs must precede INI entries, retaining DLL order");
        Require(std::equal(saved.entries.begin(), saved.entries.end(), readback.entries.begin()),
            "save/reload changed stable DLL-first ordering");
        Require(Read(first) == dll_bytes && Read(second) == dll_bytes, "DLL bytes modified");
        const auto initial_bytes = Read(ini);
        auto shuffled = readback.entries;
        std::rotate(shuffled.begin(), shuffled.begin() + 4, shuffled.end());
        const auto reordered = lyrics::SaveServiceCatalog(readback, shuffled);
        Require(!reordered.error.empty() && reordered.entries == readback.entries && Read(ini) == initial_bytes,
            "saving INI-first input bypassed the fixed DLL boundary");
        shuffled = readback.entries; std::swap(shuffled[0], shuffled[1]);
        Require(!lyrics::SaveServiceCatalog(readback, shuffled).error.empty() && Read(ini) == initial_bytes,
            "backend accepted reordering DLL entries");
        auto invalid = readback.entries;
        invalid[4].storage = directory / L"unrelated.ini";
        Require(!lyrics::SaveServiceCatalog(readback, invalid).error.empty() && Read(ini) == initial_bytes,
            "backend accepted writable storage path");
        invalid = readback.entries; invalid[4].url = L"file:///C:/Windows/win.ini";
        Require(!lyrics::SaveServiceCatalog(readback, invalid).error.empty(), "accepted non-web protocol");
        for (const auto& url : {L"http://user:pass@example.test/", L"https://example.test/#fragment",
            L"https://example.test/ injected", L"https://", L"ftp://example.test/"})
            Require(!lyrics::ValidServiceUrl(url), "invalid URL accepted");
        Require(!lyrics::ValidServiceUrl(std::wstring(L"https://example.test/") + L'\0' + L"tail"), "embedded NUL URL");
        Require(lyrics::ValidServiceUrl(L"https://[::1]:8443/lyrics"), "HTTPS IPv6 rejected");
        Write(ini, initial_bytes + "<!--external edit-->");
        invalid = readback.entries; invalid[4].name += L" changed";
        Require(!lyrics::SaveServiceCatalog(readback, invalid).error.empty() && Read(ini) == initial_bytes + "<!--external edit-->",
            "external edit conflict overwritten");
        Write(ini, "\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"UTF-8\"?><ttp_lrcsvr custom=\"keep\">"
            "<!--keep--><server name=\"Local\" url=\"http://127.0.0.1/lyrics\"/>"
            "<extra title=\"help\" url=\"https://example.test/\"/></ttp_lrcsvr>");
        readback = lyrics::ReadServiceCatalog(providers); invalid = readback.entries;
        invalid[4].name = L"Updated";
        saved = lyrics::SaveServiceCatalog(readback, invalid);
        Require(saved.error.empty() && Read(ini).find("<extra") != std::string::npos &&
            Read(ini).find("custom=\"keep\"") != std::string::npos && Read(ini).find("<!--keep-->") != std::string::npos,
            "unrelated INI XML metadata lost");
        Require(fs::exists(ini.wstring()+L".bak"), "recoverable previous INI not kept");
        const auto before_readonly = Read(ini);
        SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_READONLY);
        invalid = saved.entries; invalid[4].name = L"Denied";
        const auto denied = lyrics::SaveServiceCatalog(saved, invalid);
        SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
        Require(!denied.error.empty() && Read(ini) == before_readonly, "read-only file overwritten");
        for (const std::string bad : {"<ttp_lrcsvr><server", "<html/>",
            "<!DOCTYPE ttp_lrcsvr [<!ENTITY x SYSTEM 'file:///C:/Windows/win.ini'>]><ttp_lrcsvr>&x;</ttp_lrcsvr>"}) {
            Write(ini, bad); readback = lyrics::ReadServiceCatalog(providers);
            Require(!readback.error.empty() && readback.entries.size() == 4 && !readback.files[0].valid,
                "invalid INI/DTD did not preserve DLL entries");
            invalid = readback.entries; invalid.push_back(edit.back());
            Require(!lyrics::SaveServiceCatalog(readback, invalid).error.empty() && Read(ini) == bad,
                "malformed INI silently overwritten");
        }
        Write(ini, initial_bytes); readback = lyrics::ReadServiceCatalog(providers);
        invalid = readback.entries; std::erase_if(invalid, [](const auto& e) { return !e.read_only; });
        saved = lyrics::SaveServiceCatalog(readback, invalid);
        Require(saved.error.empty() && lyrics::ReadServiceCatalog(providers).entries.size() == 4,
            "delete all INI entries should retain DLL defaults");
        Require(lyrics::ReadServiceCatalog({}).entries.empty(), "enumerated INI without loaded DLL");
        settings::Settings settings; settings.lyric.server_key = L"ini|ttp_lrcsh.dll|测试|https://example.test/";
        settings.lyric.add_in_index = 3;
        settings::SaveWindowState(directory / L"settings.xml", settings);
        const auto restored = settings::LoadLegacyXml(directory / L"settings.xml");
        Require(restored.lyric.server_key == settings.lyric.server_key && restored.lyric.add_in_index == 3,
            "selected service identity not persisted");
        {
            const auto order_dir = directory / L"ordering";
            fs::create_directories(order_dir);
            const auto module_a = order_dir / L"a.dll", module_b = order_dir / L"b.dll";
            fs::copy_file(first, module_a); fs::copy_file(first, module_b);
            const std::vector<plugins::LyricSearchProviderInfo> order_providers{{L"A",module_a},{L"B",module_b}};
            auto catalog = lyrics::ReadServiceCatalog(order_providers);
            auto draft = catalog.entries;
            draft.push_back({L"A1", L"https://example.test/a1", {}, module_a, lyrics::ServiceIniPath(module_a)});
            draft.push_back({L"A2", L"https://example.test/a2", {}, module_a, lyrics::ServiceIniPath(module_a)});
            draft.push_back({L"B1", L"https://example.test/b1", {}, module_b, lyrics::ServiceIniPath(module_b), false, 1});
            catalog = lyrics::SaveServiceCatalog(catalog, draft);
            Require(catalog.error.empty(), "cross-file order setup failed");
            draft = catalog.entries;
            for (int i = 0; i < 4; ++i) for (int direction : {-1,1})
                Require(!lyrics::CanMoveService(draft, i, direction) && !lyrics::MoveService(draft, i, direction),
                    "DLL row can move");
            Require(!lyrics::MoveService(draft, -1, 1) && !lyrics::MoveService(draft, 7, -1) &&
                !lyrics::MoveService(draft, 4, -1) && !lyrics::MoveService(draft, 6, 1) &&
                !lyrics::MoveService(draft, 5, 0) && draft == catalog.entries, "invalid/boundary move mutated entries");
            Require(lyrics::MoveService(draft, 6, -1) && draft[5].name == L"B1", "cross-INI upward move failed");
            const auto moved = lyrics::SaveServiceCatalog(catalog, draft);
            catalog = lyrics::ReadServiceCatalog(order_providers);
            Require(moved.error.empty() && catalog.error.empty() && catalog.entries == moved.entries &&
                catalog.entries[4].name == L"A1" && catalog.entries[5].name == L"B1" && catalog.entries[6].name == L"A2",
                "cross-INI ordering did not survive reload");
            Require(catalog.entries[5].storage == lyrics::ServiceIniPath(module_b) &&
                Read(lyrics::ServiceIniPath(module_a)).find("B1") == std::string::npos,
                "moving changed the server's save location");
            draft = catalog.entries;
            Require(lyrics::MoveService(draft, 5, -1) && !lyrics::MoveService(draft, 4, -1),
                "cross-INI first row crossed into DLL group");
            const auto first_custom = lyrics::SaveServiceCatalog(catalog, draft);
            Require(first_custom.error.empty() && lyrics::ReadServiceCatalog(order_providers).entries[4].name == L"B1",
                "first INI position not persisted");
            draft = first_custom.entries;
            Require(lyrics::MoveService(draft, 4, 1), "INI downward move failed");
            const auto down = lyrics::SaveServiceCatalog(first_custom, draft);
            Require(down.error.empty() && lyrics::ReadServiceCatalog(order_providers).entries == down.entries,
                "downward order save/reload failed");
            Require(Read(module_a) == dll_bytes && Read(module_b) == dll_bytes, "ordering changed DLL bytes");
        }
        std::wcout << L"PASS: INI/DLL catalog, async read/save, immutable rows, HTTPS URLs, multi-DLL storage,\n"
            L"XML preservation, backups, conflict/read-only/DTD rejection, settings identity.\nFixtures: " << directory << L'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lyric catalog test: " << error.what() << '\n'; return 1;
    }
}
