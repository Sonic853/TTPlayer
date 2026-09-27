#include "plugin_registry.h"
#include "ttplayer/core/text.h"
#include "ttplayer/update/update.h"
#include "../update/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <map>
#include <mutex>
#include <stdexcept>

namespace ttplayer::audio::detail {
namespace {
constexpr wchar_t kOzone[] = L"software\\izotope\\ozone\\winamp2";
constexpr size_t kMaximumFile = 4 * 1024 * 1024;
constexpr size_t kMaximumHandles = 65536;
size_t KeyCost(std::wstring_view path) { return 64 + path.size() * 6; }
size_t ValueCost(std::wstring_view name, size_t bytes) { return 80 + name.size() * 6 + bytes * 2; }
std::wstring Normal(std::wstring value) {
    for (auto& c : value) {
        c = static_cast<wchar_t>(towlower(c));
    }
    while (!value.empty() && value.back() == L'\\') value.pop_back();
    if (value.size() > 32767) throw std::runtime_error("Registry path too long");
    return value;
}
std::wstring ValueName(const wchar_t* name) {
    std::wstring value = name ? name : L"";
    if (value.size() > 16383) throw std::runtime_error("Registry value name too long");
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}
std::string Hex(const BYTE* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out; out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}
std::vector<BYTE> Unhex(std::string_view text) {
    if (text.size() % 2 || text.size() > kMaximumFile) throw std::runtime_error("Invalid registry data");
    const auto digit = [](char c) -> BYTE {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid registry hex");
    };
    std::vector<BYTE> result;
    for (size_t i = 0; i < text.size(); i += 2) result.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
    return result;
}
std::string Quote(std::wstring_view value) {
    std::string out = "\"";
    for (const unsigned char c : core::WideToUtf8(value)) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c < 32) {
            out += "\\u00";
            out += Hex(&c, 1);
        } else out += c;
    }
    return out + '"';
}
std::string Read(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > kMaximumFile) throw std::runtime_error("Registry file too large");
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!stream.read(bytes.data(), bytes.size())) throw std::runtime_error("Cannot read plugin registry file");
    return bytes;
}
bool Save(const std::filesystem::path& path, const std::string& bytes) {
    const auto temporary = path.wstring() + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    bool good = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (good && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        good = CopyFileW(path.c_str(), (path.wstring() + L".bak").c_str(), FALSE) != FALSE;
    if (good) good = MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!good) DeleteFileW(temporary.c_str());
    return good;
}
}

struct FileRegistry::Impl {
    struct Value { DWORD type{}; std::vector<BYTE> bytes; };
    using Tree = std::map<std::wstring, std::map<std::wstring, Value>>;
    struct Key { std::wstring path; REGSAM access{}; bool closed{}; };
    std::filesystem::path file;
    HANDLE lease = INVALID_HANDLE_VALUE;
    mutable std::mutex mutex;
    std::mutex saving;
    Tree tree;
    std::map<HKEY, std::unique_ptr<Key>> handles;
    HANDLE timer{};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    uint64_t revision{}, saved{};
    size_t storage_cost = 256;
    bool stopping{};
    LSTATUS save_error = ERROR_SUCCESS;

    explicit Impl(std::filesystem::path path) : file(std::move(path)) {
        if (file.empty()) throw std::runtime_error("Missing plugin registry file");
        std::filesystem::create_directories(file.parent_path());
        lease = CreateFileW((file.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lease == INVALID_HANDLE_VALUE) throw std::runtime_error("Plugin configuration is busy or read-only");
        try {
            if (std::filesystem::exists(file)) {
                const auto json = update::detail::JsonReader(Read(file)).Read();
                if (json["formatVersion"].Integer() != 1 || json["keys"].kind != update::detail::Json::array)
                    throw std::runtime_error("Unsupported plugin registry format");
                for (const auto& node : json["keys"].items) {
                    const auto path_value = Normal(core::Utf8ToWide(node["path"].String()));
                    if (node["path"].kind != update::detail::Json::string ||
                        node["values"].kind != update::detail::Json::array || tree.contains(path_value))
                        throw std::runtime_error("Invalid plugin registry key");
                    auto& values = tree[path_value];
                    storage_cost += KeyCost(path_value);
                    for (const auto& item : node["values"].items) {
                        const auto name = ValueName(core::Utf8ToWide(item["name"].String()).c_str());
                        const auto type = item["type"].Integer();
                        if (item["name"].kind != update::detail::Json::string ||
                            item["type"].kind != update::detail::Json::number || type > MAXDWORD ||
                            item["dataHex"].kind != update::detail::Json::string || values.contains(name))
                            throw std::runtime_error("Invalid plugin registry value");
                        auto bytes = Unhex(item["dataHex"].String());
                        storage_cost += ValueCost(name, bytes.size());
                        if (storage_cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
                        values.emplace(name, Value{static_cast<DWORD>(type), std::move(bytes)});
                    }
                    if (storage_cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
                }
            }
            // Use the OS pool rather than creating a thread whose DLL detach
            // notifications would have to finish inside plug-in teardown.
            if (!CreateTimerQueueTimer(&timer, nullptr, Persist, this, 200, 200, WT_EXECUTEDEFAULT))
                throw std::runtime_error("Cannot schedule plugin configuration saves");
        } catch (...) { CloseHandle(lease); throw; }
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (done) {
            const BOOL removed = DeleteTimerQueueTimer(nullptr, timer, done);
            if (removed || GetLastError() == ERROR_IO_PENDING) {
                while (MsgWaitForMultipleObjects(1, &done, FALSE, INFINITE,
                                                 QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
                    MSG message{};
                    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                }
            } else DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
            CloseHandle(done);
        } else {
            DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
        }
        try { Flush(); } catch (...) {}
        CloseHandle(lease);
    }
    static void CALLBACK Persist(void* state, BOOLEAN) noexcept {
        auto& self = *static_cast<Impl*>(state);
        if (self.writing.test_and_set()) return;
        try {
            bool dirty{};
            { std::lock_guard lock(self.mutex); dirty = !self.stopping && self.revision != self.saved; }
            if (dirty) self.Flush();
        } catch (...) { std::lock_guard lock(self.mutex); self.save_error = ERROR_WRITE_FAULT; }
        self.writing.clear();
    }
    LSTATUS Flush() {
        std::lock_guard serial(saving);
        Tree snapshot; uint64_t version{};
        { std::lock_guard lock(mutex); if (saved == revision) return save_error;
          snapshot = tree; version = revision; }
        std::string text = "{\"formatVersion\":1,\"keys\":[";
        bool first_key = true;
        for (const auto& [path, values] : snapshot) {
            if (!first_key) text += ','; first_key = false;
            text += "\n{\"path\":" + Quote(path) + ",\"values\":[";
            bool first_value = true;
            for (const auto& [name, value] : values) {
                if (!first_value) text += ','; first_value = false;
                text += "{\"name\":" + Quote(name) + ",\"type\":" + std::to_string(value.type) +
                        ",\"dataHex\":\"" + Hex(value.bytes.data(), value.bytes.size()) + "\"}";
            }
            text += "]}";
        }
        text += "]}\n";
        const bool good = text.size() <= kMaximumFile && Save(file, text);
        std::lock_guard lock(mutex);
        save_error = good ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
        if (good) saved = version;
        return save_error;
    }
};
FileRegistry::FileRegistry(const std::filesystem::path& file) : impl_(std::make_unique<Impl>(file)) {}
FileRegistry::~FileRegistry() = default;
bool FileRegistry::Empty() const { std::lock_guard lock(impl_->mutex); return impl_->tree.empty(); }
bool FileRegistry::Owns(HKEY key) const { std::lock_guard lock(impl_->mutex); return impl_->handles.contains(key); }
std::wstring FileRegistry::Path(HKEY key) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(key);
    if (found == impl_->handles.end() || found->second->closed) throw std::runtime_error("Invalid virtual registry handle");
    return found->second->path;
}
LSTATUS FileRegistry::Open(std::wstring path, REGSAM access, bool create, HKEY* result, DWORD* disposition) {
    if (!result || (access & KEY_WOW64_64KEY)) return ERROR_INVALID_PARAMETER;
    path = Normal(std::move(path));
    std::lock_guard lock(impl_->mutex);
    if (impl_->handles.size() >= kMaximumHandles) return ERROR_TOO_MANY_OPEN_FILES;
    bool added{};
    if (!impl_->tree.contains(path)) {
        if (!create) return ERROR_FILE_NOT_FOUND;
        const auto cost = KeyCost(path);
        if (impl_->storage_cost + cost > kMaximumFile) return ERROR_DISK_FULL;
        impl_->tree[path]; impl_->storage_cost += cost;
        ++impl_->revision; added = true;
    }
    auto key = std::make_unique<Impl::Key>(Impl::Key{path, access, false});
    const auto handle = reinterpret_cast<HKEY>(key.get());
    impl_->handles.emplace(handle, std::move(key));
    *result = handle;
    if (disposition) *disposition = added ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
    return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Query(HKEY key, const wchar_t* name, DWORD* type, BYTE* data, DWORD* size) {
    if (data && !size) return ERROR_INVALID_PARAMETER;
    DWORD value_type{}; std::vector<BYTE> bytes;
    const auto status = ReadValue(key, name, value_type, bytes);
    if (status != ERROR_SUCCESS) return status;
    if (type) *type = value_type;
    const DWORD available = size ? *size : 0;
    if (size) *size = static_cast<DWORD>(bytes.size());
    if (data && available < bytes.size()) return ERROR_MORE_DATA;
    if (data && !bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
    return ERROR_SUCCESS;
}
LSTATUS FileRegistry::ReadValue(HKEY key, const wchar_t* name, DWORD& type, std::vector<BYTE>& data) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(key);
    if (found == impl_->handles.end() || found->second->closed) return ERROR_INVALID_HANDLE;
    if (!(found->second->access & KEY_QUERY_VALUE)) return ERROR_ACCESS_DENIED;
    const auto& values = impl_->tree.at(found->second->path);
    const auto value = values.find(ValueName(name));
    if (value == values.end()) return ERROR_FILE_NOT_FOUND;
    type = value->second.type; data = value->second.bytes;
    return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Set(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size) {
    if ((!data && size) || size > kMaximumFile / 4) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(key);
    if (found == impl_->handles.end() || found->second->closed) return ERROR_INVALID_HANDLE;
    if (!(found->second->access & KEY_SET_VALUE)) return ERROR_ACCESS_DENIED;
    const auto value_name = ValueName(name);
    auto& values = impl_->tree.at(found->second->path);
    const auto old = values.find(value_name);
    const size_t old_cost = old == values.end() ? 0 : ValueCost(value_name, old->second.bytes.size());
    const size_t new_cost = ValueCost(value_name, size);
    if (impl_->storage_cost - old_cost + new_cost > kMaximumFile) return ERROR_DISK_FULL;
    Impl::Value value{type, {}};
    if (size) value.bytes.assign(data, data + size);
    values.insert_or_assign(value_name, std::move(value));
    impl_->storage_cost = impl_->storage_cost - old_cost + new_cost;
    ++impl_->revision; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Close(HKEY key) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(key);
    if (found == impl_->handles.end() || found->second->closed) return ERROR_INVALID_HANDLE;
    found->second->closed = true; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Flush() { return impl_->Flush(); }
void FileRegistry::Import(const std::filesystem::path& file) {
    // Deliberately narrow import format: only Ozone's three REG_SZ fields and
    // Minimized DWORD in its exact branch, no deletions or executable content.
    const auto bytes = Read(file);
    if (bytes.size() < 2 || bytes.size() % 2 || BYTE(bytes[0]) != 0xff || BYTE(bytes[1]) != 0xfe)
        throw std::runtime_error("Ozone registry import must be UTF-16LE");
    std::wstring text((bytes.size() - 2) / 2, L'\0');
    std::memcpy(text.data(), bytes.data() + 2, bytes.size() - 2);
    std::vector<std::pair<std::wstring, std::wstring>> fields;
    size_t start{}; bool selected{};
    while (start < text.size()) {
        const auto end = text.find(L'\n', start);
        auto line = text.substr(start, end == text.npos ? text.npos : end - start);
        start = end == text.npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L';' || line == L"Windows Registry Editor Version 5.00") continue;
        if (line[0] == L'[') {
            const auto section = Normal(line);
            if (section == L"[hkey_current_user\\software\\izotope]" ||
                section == L"[hkey_current_user\\software\\izotope\\ozone]") {
                selected = false;
                continue;
            }
            if (section != L"[hkey_current_user\\software\\izotope\\ozone\\winamp2]")
                throw std::runtime_error("Unexpected Ozone import branch");
            selected = true; continue;
        }
        if (!selected) throw std::runtime_error("Missing Ozone import branch");
        const auto split = line.find(L"\"=\"");
        if (split == line.npos || line.front() != L'"' || line.back() != L'"') {
            // Do not copy the historical UI flag into a portable profile.
            if (line.rfind(L"\"Minimized\"=dword:", 0) == 0) continue;
            throw std::runtime_error("Unsupported Ozone registry import value");
        }
        auto name = line.substr(1, split - 1);
        const auto lowered = Normal(name);
        if (lowered != L"emailaddress" && lowered != L"regname" && lowered != L"regcode")
            throw std::runtime_error("Unexpected Ozone import value");
        auto raw = line.substr(split + 3, line.size() - split - 4);
        if (raw.size() > 32767) throw std::runtime_error("Ozone import field is too large");
        std::wstring value;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == L'\\') {
                if (++i == raw.size() || (raw[i] != L'\\' && raw[i] != L'"'))
                    throw std::runtime_error("Invalid Ozone import escape");
            }
            value += raw[i];
        }
        fields.emplace_back(std::move(name), std::move(value));
    }
    std::map<std::wstring, bool> names;
    for (const auto& field : fields) names.emplace(ValueName(field.first.c_str()), true);
    if (fields.size() != 3 || names.size() != 3) throw std::runtime_error("Ozone import requires three registration strings");
    HKEY key{};
    if (Open(kOzone, KEY_ALL_ACCESS, true, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot create Ozone file configuration");
    for (const auto& [name, value] : fields) {
        if (Set(key, name.c_str(), REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            Close(key);
            throw std::runtime_error("Cannot import Ozone configuration field");
        }
    }
    Close(key);
    if (Flush() != ERROR_SUCCESS) throw std::runtime_error("Cannot save Ozone imported configuration");
}

struct PluginRegistry::Impl {
    HMODULE module{};
    size_t image_size{};
    std::unique_ptr<FileRegistry> registry;
    std::vector<std::pair<ULONG_PTR*, ULONG_PTR>> imports;
};
namespace {
std::mutex contexts_mutex;
std::vector<std::shared_ptr<PluginRegistry::Impl>> contexts;
using Context = std::shared_ptr<PluginRegistry::Impl>;
Context FindContext(void* caller, HKEY key = nullptr) {
    std::lock_guard lock(contexts_mutex);
    const auto address = reinterpret_cast<ULONG_PTR>(caller);
    for (const auto& context : contexts) {
        const auto base = reinterpret_cast<ULONG_PTR>(context->module);
        if ((address >= base && address - base < context->image_size) ||
            (key && context->registry->Owns(key))) return context;
    }
    return {};
}
std::wstring Wide(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (!count) throw std::runtime_error("Invalid ANSI registry name");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), count);
    result.pop_back(); return result;
}
bool OwnedPath(std::wstring_view path) {
    return path == kOzone || (path.starts_with(kOzone) && path.size() > std::size(kOzone) - 1 &&
                             path[std::size(kOzone) - 1] == L'\\');
}
bool Ancestor(std::wstring_view path) {
    const std::wstring_view owned(kOzone);
    return path.empty() || (owned.starts_with(path) && owned.size() > path.size() && owned[path.size()] == L'\\');
}
LSTATUS Open(const Context& c, HKEY parent, const wchar_t* subkey, REGSAM access,
              bool create, HKEY* result, DWORD* disposition) {
    if (!c) return create ? RegCreateKeyExW(parent, subkey, 0, nullptr, 0, access, nullptr, result, disposition)
                          : RegOpenKeyExW(parent, subkey, 0, access, result);
    const bool virtual_parent = c->registry->Owns(parent);
    if (parent == HKEY_CURRENT_USER || virtual_parent) {
        auto path = virtual_parent ? c->registry->Path(parent) : std::wstring{};
        if (subkey && *subkey) { if (!path.empty()) path += L'\\'; path += subkey; }
        path = Normal(std::move(path));
        if (OwnedPath(path) || Ancestor(path))
            return c->registry->Open(path, access, create || Ancestor(path), result, disposition);
        if (create || (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER)))
            return ERROR_ACCESS_DENIED;
        return RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, result);
    }
    if (create || (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER)))
        return ERROR_ACCESS_DENIED;
    return RegOpenKeyExW(parent, subkey, 0, access, result);
}
LSTATUS WINAPI OpenW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY result) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegOpenKeyExW(key, sub, options, access, result);
        if (options) return ERROR_INVALID_PARAMETER;
        return Open(context, key, sub, access, false, result, nullptr);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI OpenA(HKEY key, LPCSTR sub, DWORD options, REGSAM access, PHKEY result) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegOpenKeyExA(key, sub, options, access, result);
        if (options) return ERROR_INVALID_PARAMETER;
        return Open(context, key, Wide(sub).c_str(), access, false, result, nullptr);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI CreateW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD options,
    REGSAM access, LPSECURITY_ATTRIBUTES security, PHKEY result, DWORD* disposition) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegCreateKeyExW(key, sub, reserved, cls, options, access, security, result, disposition);
        if (reserved || options || cls || security) return ERROR_INVALID_PARAMETER;
        return Open(context, key, sub, access, true, result, disposition);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI CreateA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD options,
    REGSAM access, LPSECURITY_ATTRIBUTES security, PHKEY result, DWORD* disposition) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegCreateKeyExA(key, sub, reserved, cls, options, access, security, result, disposition);
        if (reserved || options || cls || security) return ERROR_INVALID_PARAMETER;
        return Open(context, key, Wide(sub).c_str(), access, true, result, disposition);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI QueryW(HKEY key, LPCWSTR name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context || !context->registry->Owns(key)) return RegQueryValueExW(key, name, reserved, type, data, size);
        if (reserved) return ERROR_INVALID_PARAMETER;
        return context->registry->Query(key, name, type, data, size);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI QueryA(HKEY key, LPCSTR name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context || !context->registry->Owns(key)) return RegQueryValueExA(key, name, reserved, type, data, size);
        if (reserved || (data && !size)) return ERROR_INVALID_PARAMETER;
        const auto wide_name = Wide(name);
        DWORD value_type{};
        std::vector<BYTE> bytes;
        const LSTATUS status = context->registry->ReadValue(key, wide_name.c_str(), value_type, bytes);
        if (status != ERROR_SUCCESS) return status;
        if (value_type == REG_SZ || value_type == REG_EXPAND_SZ || value_type == REG_MULTI_SZ) {
            if (bytes.size() % sizeof(wchar_t)) return ERROR_INVALID_DATA;
            const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data());
            const int characters = static_cast<int>(bytes.size() / sizeof(wchar_t));
            const int required = characters ? WideCharToMultiByte(CP_ACP, 0, wide, characters, nullptr, 0, nullptr, nullptr) : 0;
            std::vector<BYTE> ansi(required);
            if (required) WideCharToMultiByte(CP_ACP, 0, wide, characters, reinterpret_cast<char*>(ansi.data()), required, nullptr, nullptr);
            bytes = std::move(ansi);
        }
        if (type) *type = value_type;
        const DWORD available = size ? *size : 0;
        if (size) *size = static_cast<DWORD>(bytes.size());
        if (data && available < bytes.size()) return ERROR_MORE_DATA;
        if (data && !bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI SetW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegSetValueExW(key, name, reserved, type, data, size);
        if (!context->registry->Owns(key)) return ERROR_ACCESS_DENIED;
        if (reserved) return ERROR_INVALID_PARAMETER;
        if (!OwnedPath(context->registry->Path(key))) return ERROR_ACCESS_DENIED;
        return context->registry->Set(key, name, type, data, size);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI SetA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegSetValueExA(key, name, reserved, type, data, size);
        if (!context->registry->Owns(key) || !OwnedPath(context->registry->Path(key))) return ERROR_ACCESS_DENIED;
        if (reserved || (!data && size) || size > kMaximumFile / 4) return ERROR_INVALID_PARAMETER;
        const auto wide_name = Wide(name);
        if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
            const int count = size ? MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(data), size, nullptr, 0) : 0;
            std::wstring wide(count, L'\0');
            if (count) MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(data), size, wide.data(), count);
            return context->registry->Set(key, wide_name.c_str(), type,
                reinterpret_cast<const BYTE*>(wide.data()), static_cast<DWORD>(wide.size() * sizeof(wchar_t)));
        }
        return context->registry->Set(key, wide_name.c_str(), type, data, size);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI Close(HKEY key) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (context && context->registry->Owns(key)) return context->registry->Close(key);
        return RegCloseKey(key);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI Flush(HKEY key) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (context && context->registry->Owns(key)) {
            context->registry->Path(key); // Reject closed handles before flushing.
            return context->registry->Flush();
        }
        return context ? ERROR_ACCESS_DENIED : RegFlushKey(key);
    } catch (...) { return ERROR_WRITE_FAULT; }
}
FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) noexcept;
BOOL WINAPI StopOzoneThread(HANDLE thread, DWORD exit_code) noexcept {
    try {
        const auto caller = _ReturnAddress();
        const auto context = FindContext(caller);
        // In this exact binary 59445706 has already cleared object+100, and
        // its worker (59443D30 / 594441C2) polls that flag. TerminateThread at
        // 5944570D can abandon the loader lock during DLL_THREAD_ATTACH.
        if (!context || reinterpret_cast<BYTE*>(caller) !=
                reinterpret_cast<BYTE*>(context->module) + 0x35713 || exit_code != 0)
            return TerminateThread(thread, exit_code);
        MemoryBarrier();
        DWORD waited{};
        while ((waited = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE,
                                                   QS_SENDMESSAGE)) == WAIT_OBJECT_0 + 1) {
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        return waited == WAIT_OBJECT_0;
    } catch (...) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
}
FARPROC Replacement(std::string_view name) {
#define ENTRY(api, replacement) if (name == #api) return reinterpret_cast<FARPROC>(replacement)
    ENTRY(RegOpenKeyExA, OpenA); ENTRY(RegOpenKeyExW, OpenW);
    ENTRY(RegCreateKeyExA, CreateA); ENTRY(RegCreateKeyExW, CreateW);
    ENTRY(RegQueryValueExA, QueryA); ENTRY(RegQueryValueExW, QueryW);
    ENTRY(RegSetValueExA, SetA); ENTRY(RegSetValueExW, SetW);
    ENTRY(RegCloseKey, Close); ENTRY(RegFlushKey, Flush);
    ENTRY(GetProcAddress, Resolve);
    ENTRY(TerminateThread, StopOzoneThread);
#undef ENTRY
    return nullptr;
}
FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) noexcept {
    if (reinterpret_cast<ULONG_PTR>(name) > 0xffff && module == GetModuleHandleW(L"advapi32.dll") &&
        std::strncmp(name, "Reg", 3) == 0) {
        if (auto replacement = Replacement(name)) return replacement;
        SetLastError(ERROR_PROC_NOT_FOUND); return nullptr;
    }
    return GetProcAddress(module, name);
}
void Restore(PluginRegistry::Impl& context) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(context.module, &info, sizeof(info)) || info.AllocationBase != context.module || info.Type != MEM_IMAGE) return;
    for (const auto& [slot, original] : context.imports) {
        DWORD previous{};
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) continue;
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<void*>(original));
        DWORD ignored{}; VirtualProtect(slot, sizeof(*slot), previous, &ignored);
    }
}
}
bool PluginRegistry::Supports(const std::filesystem::path& plugin) {
    if (_wcsicmp(plugin.filename().c_str(), L"dsp_izOzone.dll") != 0) return false;
    return update::Sha256(plugin) == "e3bb0eef979ea8016fb1278b373c7c70ae4507719524bfefe802b5bc3c800e59";
}
std::unique_ptr<PluginRegistry> PluginRegistry::Attach(HMODULE module,
    const std::filesystem::path& plugin, const std::filesystem::path& directory) {
    if (!Supports(plugin)) return {};
    auto context = std::make_shared<Impl>(); context->module = module;
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    context->image_size = nt->OptionalHeader.SizeOfImage;
    const auto folder = directory.empty() ? plugin.parent_path() / L"Ozone" : directory / L"Ozone";
    const auto file = folder / L"registry.json";
    const bool existing = std::filesystem::exists(file);
    context->registry = std::make_unique<FileRegistry>(file);
    if (!existing) {
        const auto import = folder / L"registry-import.reg";
        if (std::filesystem::exists(import)) context->registry->Import(import);
        else {
            // Read-only migration, restricted to this product's values.
            HKEY native{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kOzone, 0, KEY_QUERY_VALUE, &native) == ERROR_SUCCESS) {
                HKEY virtual_key{};
                context->registry->Open(kOzone, KEY_ALL_ACCESS, true, &virtual_key, nullptr);
                for (const auto name : {L"EmailAddress", L"RegName", L"RegCode", L"Minimized"}) {
                    DWORD type{}, size{};
                    if (RegQueryValueExW(native, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size > 65536) continue;
                    std::vector<BYTE> data(size);
                    if (RegQueryValueExW(native, name, nullptr, &type, data.data(), &size) == ERROR_SUCCESS)
                        context->registry->Set(virtual_key, name, type, data.data(), size);
                }
                context->registry->Close(virtual_key); RegCloseKey(native);
                if (context->registry->Flush() != ERROR_SUCCESS) throw std::runtime_error("Ozone configuration migration failed");
            }
        }
    }
    { std::lock_guard lock(contexts_mutex); contexts.push_back(context); }
    auto attachment = std::unique_ptr<PluginRegistry>(new PluginRegistry(context));
    const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva);
    for (; descriptor->Name; ++descriptor) {
        const auto* library = reinterpret_cast<const char*>(base + descriptor->Name);
        const bool registry = _stricmp(library, "ADVAPI32.dll") == 0;
        if (!registry && _stricmp(library, "KERNEL32.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk) throw std::runtime_error("Ozone imports have no name table");
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            const auto replacement = Replacement(reinterpret_cast<const char*>(name));
            if (!replacement) {
                if (registry && std::strncmp(reinterpret_cast<const char*>(name), "Reg", 3) == 0)
                    throw std::runtime_error("Uncovered Ozone registry import");
                continue;
            }
            auto* slot = &slots->u1.Function;
            DWORD previous{};
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) throw std::runtime_error("Cannot adapt Ozone imports");
            context->imports.emplace_back(slot, *slot);
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<void*>(replacement));
            DWORD ignored{}; VirtualProtect(slot, sizeof(*slot), previous, &ignored);
        }
    }
    return attachment;
}
PluginRegistry::PluginRegistry(std::shared_ptr<Impl> state) : impl_(std::move(state)) {}
PluginRegistry::~PluginRegistry() {
    Restore(*impl_);
    std::lock_guard lock(contexts_mutex);
    std::erase(contexts, impl_);
}
bool PluginRegistry::Flush() noexcept {
    try { return impl_->registry->Flush() == ERROR_SUCCESS; }
    catch (...) { return false; }
}
} // namespace ttplayer::audio::detail
