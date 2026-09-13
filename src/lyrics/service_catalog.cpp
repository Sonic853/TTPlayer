#include "ttplayer/lyrics/service_catalog.h"
#include "service_xml.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <winhttp.h>

namespace ttplayer::lyrics {
namespace {
void BuiltinsFirst(std::vector<LyricService>& entries) {
    // One global DLL prefix, even when several AddIns have sibling INIs.
    // Preserve the original order within each source group.
    std::stable_partition(entries.begin(), entries.end(), [](const auto& entry) { return entry.read_only; });
}
std::string Read(const std::filesystem::path& path, bool& exists) {
    std::error_code error;
    exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("Cannot inspect INI");
    if (!exists) return {};
    if (std::filesystem::file_size(path, error) > 2 * 1024 * 1024 || error)
        throw std::runtime_error("Cannot read INI size");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read INI");
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (file.bad() || bytes.size() > 2 * 1024 * 1024) throw std::runtime_error("Cannot read INI");
    return bytes;
}
std::wstring Resource(HMODULE module, UINT id) {
    LPCWSTR text{};
    int size = LoadStringW(module, id, reinterpret_cast<LPWSTR>(&text), 0);
    return size > 0 ? std::wstring(text, size) : std::wstring{};
}
void Error(ServiceCatalog& catalog, const std::filesystem::path& path, const wchar_t* reason) {
    if (!catalog.error.empty()) catalog.error += L"\r\n";
    catalog.error += path.wstring() + L": " + reason;
}
std::vector<LyricService> IniEntries(const ServiceFile& file, size_t provider) {
    auto doc = xml::Parse(file.original);
    xml::ComPtr<IXMLDOMNode> root;
    doc->selectSingleNode(_bstr_t(L"/ttp_lrcsvr"), &root);
    if (!root) throw std::runtime_error("Not ttp_lrcsvr XML");
    xml::ComPtr<IXMLDOMNodeList> nodes;
    root->selectNodes(_bstr_t(L"server"), &nodes);
    long size{}; nodes->get_length(&size);
    if (size > 128) throw std::runtime_error("Too many servers");
    std::vector<LyricService> entries;
    for (long i = 0; i < size; ++i) {
        xml::ComPtr<IXMLDOMNode> node; nodes->get_item(i, &node);
        LyricService entry{xml::Attribute(node.Get(), L"name"), xml::Attribute(node.Get(), L"url"),
            {}, file.module, file.ini, false, provider};
        if (entry.name.empty() || !ValidServiceUrl(entry.url)) throw std::runtime_error("Invalid server name/URL");
        entry.key = ServiceKey(entry);
        const auto order = core::WideToUtf8(xml::Attribute(node.Get(), L"order"));
        int value{};
        const auto parsed = std::from_chars(order.data(), order.data() + order.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == order.data() + order.size() && value >= 0)
            entry.order = value;
        entries.push_back(std::move(entry));
    }
    return entries;
}
void AtomicWrite(const ServiceFile& file, const std::string& bytes) {
    bool exists{};
    if (Read(file.ini, exists) != file.original || exists != file.exists)
        throw std::runtime_error("INI changed outside editor; reopen and retry");
    wchar_t temp[MAX_PATH]{};
    if (!GetTempFileNameW(file.ini.parent_path().c_str(), L"lrs", 0, temp)) throw std::runtime_error("Cannot create temporary INI");
    HANDLE output = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written{};
    bool ok = output != INVALID_HANDLE_VALUE && WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(output);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (ok) {
        try { ok = Read(file.ini, exists) == file.original && exists == file.exists; }
        catch (...) { ok = false; }
    }
    if (ok) {
        // ReplaceFile retains metadata and leaves a recoverable backup. The
        // only overwrite target is the validated DLL-sibling INI, never DLL.
        const auto backup = file.ini.wstring() + L".bak";
        ok = file.exists ? ReplaceFileW(file.ini.c_str(), temp, backup.c_str(), 0, nullptr, nullptr) != FALSE
            : MoveFileExW(temp, file.ini.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) { DeleteFileW(temp); throw std::runtime_error("Cannot save INI (changed, read-only or inaccessible)"); }
}
}

std::filesystem::path ServiceIniPath(std::filesystem::path module) { return module.replace_extension(L".ini"); }
bool CanMoveService(const std::vector<LyricService>& entries, int index, int direction) noexcept {
    if ((direction != -1 && direction != 1) || index < 0 || static_cast<size_t>(index) >= entries.size()) return false;
    const auto target = static_cast<std::int64_t>(index) + direction;
    return target >= 0 && static_cast<size_t>(target) < entries.size() &&
        !entries[index].read_only && !entries[static_cast<size_t>(target)].read_only;
}
bool MoveService(std::vector<LyricService>& entries, int index, int direction) {
    if (!CanMoveService(entries, index, direction)) return false;
    std::swap(entries[index], entries[index + direction]);
    // Persist a common INI-only order, including moves across plugin files.
    // Unknown XML attributes are ignored by the original lyric DLL.
    int order = 0;
    for (auto& entry : entries) if (!entry.read_only) entry.order = order++;
    return true;
}
std::wstring ServiceKey(const LyricService& service) {
    auto module = service.module.filename().wstring();
    std::transform(module.begin(), module.end(), module.begin(), towlower);
    return (service.read_only ? L"dll|" : L"ini|") + module + L"|" + service.name + L"|" + service.url;
}
bool ValidServiceUrl(std::wstring_view value) {
    if (value.empty() || value.size() > 8192 ||
        std::any_of(value.begin(), value.end(), [](wchar_t c) { return c <= L' ' || c == L'\\'; })) return false;
    URL_COMPONENTS url{sizeof(url)};
    url.dwHostNameLength = url.dwUserNameLength = url.dwPasswordLength = url.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(value.data(), static_cast<DWORD>(value.size()), 0, &url)) return false;
    return (url.nScheme == INTERNET_SCHEME_HTTP || url.nScheme == INTERNET_SCHEME_HTTPS) && url.dwHostNameLength &&
        !url.dwUserNameLength && !url.dwPasswordLength && value.find(L'#') == value.npos;
}
ServiceCatalog ReadServiceCatalog(const std::vector<plugins::LyricSearchProviderInfo>& providers) {
    xml::Apartment apartment;
    ServiceCatalog catalog;
    std::set<std::filesystem::path> visited;
    for (size_t i = 0; i < providers.size(); ++i) {
        const auto& provider = providers[i];
        if (!visited.insert(provider.module_path).second) continue;
        ServiceFile file{provider.module_path, ServiceIniPath(provider.module_path)};
        try {
            file.original = Read(file.ini, file.exists);
            if (file.exists) {
                auto custom = IniEntries(file, i);
                catalog.entries.insert(catalog.entries.end(), custom.begin(), custom.end());
            }
        } catch (...) {
            file.valid = false;
            Error(catalog, file.ini, L"无法读取有效的服务器 XML，保留原文件，不允许覆盖。");
        }
        catalog.files.push_back(file);
        // Load only resource data; never unload/reload a live sound AddIn.
        HMODULE module = LoadLibraryExW(provider.module_path.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        unsigned builtins = 0;
        if (module) {
            for (UINT j = 0; j < 2; ++j) {
                const auto name = Resource(module, 32000 + j), url = Resource(module, 32002 + j);
                if (name.empty() || !ValidServiceUrl(url)) continue;
                LyricService entry{name, url, {}, provider.module_path, provider.module_path, true, i + j};
                entry.key = ServiceKey(entry); catalog.entries.push_back(std::move(entry)); ++builtins;
            }
            FreeLibrary(module);
        }
        if (!builtins) {
            // Unrepresented AddIns still run through their private ABI. Do
            // not invent URL resources for a different protocol provider.
            for (size_t j = i; j < providers.size(); ++j) if (providers[j].module_path == provider.module_path) {
                LyricService entry{providers[j].name, {}, {}, provider.module_path, provider.module_path, true, j};
                entry.key = ServiceKey(entry); catalog.entries.push_back(std::move(entry));
            }
        }
    }
    BuiltinsFirst(catalog.entries);
    const auto custom = std::find_if(catalog.entries.begin(), catalog.entries.end(), [](const auto& e) { return !e.read_only; });
    std::stable_sort(custom, catalog.entries.end(), [](const auto& a, const auto& b) {
        return a.order.value_or(std::numeric_limits<int>::max()) < b.order.value_or(std::numeric_limits<int>::max());
    });
    return catalog;
}

ServiceCatalog SaveServiceCatalog(const ServiceCatalog& baseline, const std::vector<LyricService>& edited) {
    xml::Apartment apartment;
    ServiceCatalog result = baseline;
    result.error.clear();
    try {
        std::vector<LyricService> fixed_before, fixed_after;
        for (const auto& entry : baseline.entries) if (entry.read_only) fixed_before.push_back(entry);
        for (const auto& entry : edited) if (entry.read_only) fixed_after.push_back(entry);
        if (fixed_before != fixed_after || !std::is_partitioned(edited.begin(), edited.end(),
            [](const auto& entry) { return entry.read_only; }))
            throw std::runtime_error("DLL order is fixed; INI entries must follow DLL entries");
        for (const auto& original : baseline.entries) if (original.read_only &&
            std::find(edited.begin(), edited.end(), original) == edited.end())
            throw std::runtime_error("DLL entries are read-only");
        for (const auto& entry : edited) {
            if (entry.read_only) {
                if (std::find(baseline.entries.begin(), baseline.entries.end(), entry) == baseline.entries.end())
                    throw std::runtime_error("Cannot change DLL entries");
            } else if (entry.name.empty() || entry.name.size() > 256 || !ValidServiceUrl(entry.url) ||
                (entry.order && *entry.order < 0) ||
                entry.storage != ServiceIniPath(entry.module) ||
                std::none_of(baseline.files.begin(), baseline.files.end(), [&](const auto& f) { return f.module == entry.module; }))
                throw std::runtime_error("Invalid server or save location");
        }
        // Validate/serialize every changed file before any write. Never drop
        // unknown root attributes, comments or extra nodes from valid XML.
        std::vector<std::pair<size_t, std::string>> writes;
        for (size_t i = 0; i < baseline.files.size(); ++i) {
            const auto& file = baseline.files[i];
            std::vector<LyricService> before, after;
            for (const auto& entry : baseline.entries) if (!entry.read_only && entry.module == file.module) before.push_back(entry);
            for (const auto& entry : edited) if (!entry.read_only && entry.module == file.module) after.push_back(entry);
            if (before == after) continue;
            if (!file.valid || after.size() > 128) throw std::runtime_error("Invalid original INI or too many entries");
            bool exists{};
            if (Read(file.ini, exists) != file.original || exists != file.exists) throw std::runtime_error("INI changed outside editor; reopen and retry");
            auto doc = xml::Parse(file.exists ? file.original : "<ttp_lrcsvr/>");
            xml::ComPtr<IXMLDOMElement> root; doc->get_documentElement(&root);
            xml::ComPtr<IXMLDOMNodeList> nodes; root->selectNodes(_bstr_t(L"server"), &nodes);
            long count{}; nodes->get_length(&count);
            for (long n = count; n-- > 0;) {
                xml::ComPtr<IXMLDOMNode> node, removed; nodes->get_item(n, &node); root->removeChild(node.Get(), &removed);
            }
            for (const auto& entry : after) {
                xml::ComPtr<IXMLDOMElement> node; doc->createElement(_bstr_t(L"server"), &node);
                node->setAttribute(_bstr_t(L"name"), _variant_t(entry.name.c_str()));
                node->setAttribute(_bstr_t(L"url"), _variant_t(entry.url.c_str()));
                if (entry.order) node->setAttribute(_bstr_t(L"order"), _variant_t(*entry.order));
                xml::ComPtr<IXMLDOMNode> appended; root->appendChild(node.Get(), &appended);
            }
            writes.emplace_back(i, xml::Serialize(doc.Get()));
        }
        for (const auto& [i, bytes] : writes) {
            AtomicWrite(baseline.files[i], bytes);
            result.files[i].original = bytes; result.files[i].exists = true;
            const auto module = baseline.files[i].module;
            std::erase_if(result.entries, [&](const auto& e) { return !e.read_only && e.module == module; });
            for (auto entry : edited) if (!entry.read_only && entry.module == module) {
                entry.key = ServiceKey(entry); result.entries.push_back(std::move(entry));
            }
        }
        // Keep each source group's order, including newly added INI rows.
        result.entries = edited;
        for (auto& entry : result.entries) entry.key = ServiceKey(entry);
    } catch (const std::exception& error) {
        result.error = L"保存未完成（已成功保存的文件已保留）：" + core::Utf8ToWide(error.what());
    } catch (...) { result.error = L"保存失败，未完成的文件保持原样。"; }
    BuiltinsFirst(result.entries);
    return result;
}
std::shared_ptr<CatalogJob> ReadServiceCatalogAsync(std::vector<plugins::LyricSearchProviderInfo> providers) {
    auto job = std::make_shared<CatalogJob>();
    std::thread([job, providers = std::move(providers)] {
        ServiceCatalog result;
        try { result = ReadServiceCatalog(providers); } catch (...) { result.error = L"读取歌词服务器列表失败。"; }
        std::lock_guard lock(job->mutex); job->result = std::move(result);
    }).detach();
    return job;
}
std::shared_ptr<CatalogJob> SaveServiceCatalogAsync(ServiceCatalog baseline, std::vector<LyricService> edited) {
    auto job = std::make_shared<CatalogJob>();
    std::thread([job, baseline = std::move(baseline), edited = std::move(edited)] {
        auto result = SaveServiceCatalog(baseline, edited);
        std::lock_guard lock(job->mutex); job->result = std::move(result);
    }).detach();
    return job;
}
} // namespace ttplayer::lyrics
