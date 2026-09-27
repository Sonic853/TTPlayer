#include "plugin_registry.h"
#include "dfx_registry_helper.h"
#include "ttplayer/core/text.h"
#include "ttplayer/update/update.h"
#include "../update/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <intrin.h>
#include <shlobj.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace ttplayer::audio::detail {
namespace {
constexpr wchar_t kOzone[] = L"hkey_current_user\\software\\izotope\\ozone\\winamp2";
constexpr size_t kMaximumFile = 4 * 1024 * 1024;
constexpr size_t kMaximumHandles = 65536;
size_t KeyCost(std::wstring_view path) { return 64 + path.size() * 6; }
size_t ValueCost(std::wstring_view name, size_t bytes) { return 80 + name.size() * 6 + bytes * 2; }
std::wstring Normal(std::wstring value) {
    for (auto& c : value) {
        c = static_cast<wchar_t>(towlower(c));
    }
    while (!value.empty() && value.back() == L'\\') value.pop_back();
    // The player and its audited plug-ins use the 32-bit registry view.
    constexpr std::wstring_view wow = L"hkey_local_machine\\software\\wow6432node";
    if (value == wow || (value.starts_with(wow) && value[wow.size()] == L'\\'))
        value.replace(0, wow.size(), L"hkey_local_machine\\software");
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
const std::pair<HKEY, const wchar_t*> kRoots[] = {
    {HKEY_CURRENT_USER, L"hkey_current_user"}, {HKEY_LOCAL_MACHINE, L"hkey_local_machine"},
    {HKEY_CLASSES_ROOT, L"hkey_classes_root"}, {HKEY_USERS, L"hkey_users"},
    {HKEY_CURRENT_CONFIG, L"hkey_current_config"}
};
std::wstring RootName(HKEY key) {
    for (const auto& [root, name] : kRoots) if (root == key) return name;
    return {};
}
HKEY SplitRoot(std::wstring_view path, std::wstring_view& subkey) {
    for (const auto& [root, name] : kRoots) {
        const std::wstring_view prefix(name);
        if (path == prefix) { subkey = {}; return root; }
        if (path.starts_with(prefix) && path[prefix.size()] == L'\\') {
            subkey = path.substr(prefix.size() + 1); return root;
        }
    }
    return nullptr;
}
struct NativeKey {
    HKEY key{};
    ~NativeKey() { if (key) RegCloseKey(key); }
    NativeKey() = default;
    NativeKey(const NativeKey&) = delete;
    LSTATUS Open(std::wstring_view path, REGSAM access = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, REGSAM view = KEY_WOW64_32KEY) {
        std::wstring_view sub;
        const auto root = SplitRoot(path, sub);
        return root ? RegOpenKeyExW(root, std::wstring(sub).c_str(), 0, access | view, &key)
                    : ERROR_FILE_NOT_FOUND;
    }
};
}

struct FileRegistry::Impl {
    struct Value { DWORD type{}; std::vector<BYTE> bytes; std::wstring name; bool deleted{}; };
    using Tree = std::map<std::wstring, std::map<std::wstring, Value>>;
    struct Key { std::wstring path; REGSAM access{}; bool closed{}; bool deleted{}; };
    std::filesystem::path file;
    HANDLE lease = INVALID_HANDLE_VALUE;
    mutable std::mutex mutex;
    std::mutex saving;
    Tree tree;
    std::set<std::wstring> masked_keys;
    std::map<HKEY, std::unique_ptr<Key>> handles;
    std::deque<HKEY> retired;
    HANDLE timer{};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    uint64_t revision{}, saved{};
    size_t storage_cost = 256;
    bool stopping{};
    bool read_only_source{};
    LSTATUS save_error = ERROR_SUCCESS;
    size_t MaskCost() const {
        size_t size{}; for (const auto& path : masked_keys) size += KeyCost(path); return size;
    }
    bool Masked(std::wstring_view path) const {
        for (const auto& mask : masked_keys)
            if (path == mask || (path.starts_with(mask) && path.size() > mask.size() && path[mask.size()] == L'\\')) return true;
        return false;
    }
    void Materialize(std::wstring path) {
        std::vector<std::wstring> missing; size_t cost{};
        for (;;) {
            if (!tree.contains(path)) { missing.push_back(path); cost += KeyCost(path); }
            const auto slash = path.rfind(L'\\'); if (slash == path.npos) break; path.resize(slash);
        }
        if (storage_cost + cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
        for (const auto& key : missing) tree[key];
        storage_cost += cost;
    }
    static LSTATUS NativeValue(HKEY key, const wchar_t* name, Value& value) {
        for (unsigned retry = 0; retry != 3; ++retry) {
            DWORD size{};
            auto status = RegQueryValueExW(key, name, nullptr, &value.type, nullptr, &size);
            if (status != ERROR_SUCCESS) return status;
            if (size > kMaximumFile / 4) return ERROR_NOT_ENOUGH_MEMORY;
            value.bytes.resize(size);
            status = RegQueryValueExW(key, name, nullptr, &value.type, value.bytes.data(), &size);
            if (status == ERROR_MORE_DATA) continue;
            if (status == ERROR_SUCCESS) { value.bytes.resize(size); value.name = name ? name : L""; }
            return status;
        }
        return ERROR_MORE_DATA;
    }
    LSTATUS ReadValue(std::wstring_view path, const wchar_t* name, Value& value, REGSAM view = KEY_WOW64_32KEY) const {
        const auto key = tree.find(std::wstring(path));
        if (key != tree.end()) {
            const auto item = key->second.find(ValueName(name));
            if (item != key->second.end()) {
                if (item->second.deleted) return ERROR_FILE_NOT_FOUND;
                value = item->second; return ERROR_SUCCESS;
            }
        }
        if (Masked(path)) return ERROR_FILE_NOT_FOUND;
        NativeKey native; const auto status = native.Open(path, KEY_QUERY_VALUE, view);
        return status == ERROR_SUCCESS ? NativeValue(native.key, name, value) : status;
    }
    std::map<std::wstring, Value> Values(const std::wstring& path, REGSAM view) const {
        std::map<std::wstring, Value> values;
        NativeKey native;
        if (!Masked(path) && native.Open(path, KEY_QUERY_VALUE, view) == ERROR_SUCCESS) {
            std::vector<wchar_t> name(16384);
            size_t cost{};
            for (DWORD i = 0;; ++i) {
                DWORD size = static_cast<DWORD>(name.size());
                const auto status = RegEnumValueW(native.key, i, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) break;
                if (status != ERROR_SUCCESS) throw std::runtime_error("Cannot enumerate native registry values");
                Value value;
                if (NativeValue(native.key, name.data(), value) != ERROR_SUCCESS) continue;
                cost += ValueCost(value.name, value.bytes.size());
                if (cost > kMaximumFile) throw std::runtime_error("Native registry enumeration exceeds limit");
                values[ValueName(name.data())] = std::move(value);
            }
        }
        const auto key = tree.find(path);
        if (key != tree.end()) for (const auto& [name, value] : key->second) {
            if (value.deleted) values.erase(name); else values[name] = value;
        }
        return values;
    }
    Key* Handle(HKEY key, REGSAM access, LSTATUS& status) {
        const auto found = handles.find(key);
        if (found == handles.end() || found->second->closed) { status = ERROR_INVALID_HANDLE; return nullptr; }
        if (found->second->deleted) { status = ERROR_KEY_DELETED; return nullptr; }
        if ((found->second->access & access) != access) { status = ERROR_ACCESS_DENIED; return nullptr; }
        status = ERROR_SUCCESS;
        return found->second.get();
    }
    static REGSAM View(const Key& key) { return key.access & KEY_WOW64_64KEY ? KEY_WOW64_64KEY : KEY_WOW64_32KEY; }
    std::set<std::wstring> Children(const Key& key) {
        std::set<std::wstring> names;
        const auto prefix = key.path.empty() ? L"" : key.path + L"\\";
        NativeKey native;
        if (!Masked(key.path) && native.Open(key.path, KEY_ENUMERATE_SUB_KEYS, View(key)) == ERROR_SUCCESS) {
            size_t cost{};
            for (DWORD i = 0;; ++i) {
                wchar_t name[256]{}; DWORD size = 256;
                const auto status = RegEnumKeyExW(native.key, i, name, &size, nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) break;
                if (status != ERROR_SUCCESS) throw std::runtime_error("Cannot enumerate native registry keys");
                auto normalized = Normal(name);
                if (!Masked(prefix + normalized)) names.insert(std::move(normalized));
                cost += KeyCost(name);
                if (cost > kMaximumFile) throw std::runtime_error("Native registry enumeration exceeds limit");
            }
        }
        for (const auto& [path, values] : tree) {
            if (path.size() <= prefix.size() || !path.starts_with(prefix)) continue;
            const auto rest = path.substr(prefix.size());
            names.insert(rest.substr(0, rest.find(L'\\')));
        }
        return names;
    }

    explicit Impl(std::filesystem::path path, bool source_only = false) : file(std::move(path)), read_only_source(source_only) {
        if (file.empty()) throw std::runtime_error("Missing plugin registry file");
        std::filesystem::create_directories(file.parent_path());
        lease = CreateFileW((file.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lease == INVALID_HANDLE_VALUE) throw std::runtime_error("Plugin configuration is busy or read-only");
        try {
            if (std::filesystem::exists(file)) {
                const auto json = update::detail::JsonReader(Read(file),kMaximumFile).Read();
                const auto version = json["formatVersion"].Integer();
                if (version == 3 && json["registryView"].Integer() == 32) {
                    LoadTree(json["keys"], tree);
                    if (json["maskedKeys"].kind != update::detail::Json::array) throw std::runtime_error("Invalid registry masks");
                    for (const auto& item : json["maskedKeys"].items) {
                        if (item.kind != update::detail::Json::string) throw std::runtime_error("Invalid registry mask");
                        masked_keys.insert(Normal(core::Utf8ToWide(item.String())));
                    }
                } else if (version == 1) {
                    LoadTree(json["keys"], tree, L"hkey_current_user\\");
                    ++revision;
                } else if (version == 2 && json["plugins"].kind == update::detail::Json::object) {
                    for (const auto& [name, profile] : json["plugins"].fields) {
                        if (name == "Ozone") LoadTree(profile["keys"], tree, L"hkey_current_user\\", true);
                        else if (name == "Dsp_Dfx" && profile["registryView"].Integer() == 32)
                            LoadTree(profile["keys"], tree, {}, true);
                        else throw std::runtime_error("Unsupported legacy registry profile");
                    }
                    ++revision;
                } else throw std::runtime_error("Unsupported plugin registry format");
                // Materialize ancestors so imported/migrated keys are reachable.
                std::vector<std::wstring> paths;
                for (const auto& [key, values] : tree) paths.push_back(key);
                for (const auto& key : paths) Materialize(key);
                storage_cost = 256 + Cost(tree) + MaskCost();
                if (storage_cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
            }
            // Use the OS pool rather than creating a thread whose DLL detach
            // notifications would have to finish inside plug-in teardown.
            if (!source_only && !CreateTimerQueueTimer(&timer, nullptr, Persist, this, 200, 200, WT_EXECUTEDEFAULT))
                throw std::runtime_error("Cannot schedule plugin configuration saves");
        } catch (...) { CloseHandle(lease); throw; }
    }
    void LoadTree(const update::detail::Json& keys, Tree& target, std::wstring_view prefix = {}, bool merge = false) {
        if (keys.kind != update::detail::Json::array)
            throw std::runtime_error("Invalid plugin registry keys");
        for (const auto& node : keys.items) {
            const auto path_value = Normal(std::wstring(prefix) + core::Utf8ToWide(node["path"].String()));
            if (node["path"].kind != update::detail::Json::string ||
                node["values"].kind != update::detail::Json::array || (!merge && target.contains(path_value)))
                throw std::runtime_error("Invalid plugin registry key");
            auto& values = target[path_value];
            storage_cost += KeyCost(path_value);
            for (const auto& item : node["values"].items) {
                const auto name = ValueName(core::Utf8ToWide(item["name"].String()).c_str());
                const auto type = item["type"].Integer();
                if (item["deleted"].True()) {
                    if (item["name"].kind != update::detail::Json::string || values.contains(name)) throw std::runtime_error("Invalid registry tombstone");
                    values.emplace(name, Value{0, {}, core::Utf8ToWide(item["name"].String()), true});
                    continue;
                }
                if (item["name"].kind != update::detail::Json::string ||
                    item["type"].kind != update::detail::Json::number || type > MAXDWORD ||
                    item["dataHex"].kind != update::detail::Json::string || values.contains(name))
                    throw std::runtime_error("Invalid plugin registry value");
                auto bytes = Unhex(item["dataHex"].String());
                storage_cost += ValueCost(name, bytes.size());
                if (storage_cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
                values.emplace(name, Value{static_cast<DWORD>(type), std::move(bytes), core::Utf8ToWide(item["name"].String())});
            }
            if (storage_cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
        }
    }
    ~Impl() {
        if (read_only_source) { CloseHandle(lease); return; }
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
    static std::string SerializeTree(const Tree& snapshot) {
        std::string text = "[";
        bool first_key = true;
        for (const auto& [path, values] : snapshot) {
            if (!first_key) text += ','; first_key = false;
            text += "\n{\"path\":" + Quote(path) + ",\"values\":[";
            bool first_value = true;
            for (const auto& [name, value] : values) {
                if (value.deleted) {
                    if (!first_value) text += ','; first_value = false;
                    text += "{\"name\":" + Quote(value.name) + ",\"deleted\":true}";
                    continue;
                }
                if (!first_value) text += ','; first_value = false;
                text += "{\"name\":" + Quote(value.name) + ",\"type\":" + std::to_string(value.type) +
                        ",\"dataHex\":\"" + Hex(value.bytes.data(), value.bytes.size()) + "\"}";
            }
            text += "]}";
        }
        text += "]";
        return text;
    }
    static std::string Serialize(const Tree& snapshot, const std::set<std::wstring>& masked) {
        std::string text = "{\"formatVersion\":3,\"registryView\":32,\"keys\":" + SerializeTree(snapshot) + ",\"maskedKeys\":[";
        bool first = true;
        for (const auto& path : masked) { if (!first) text += ','; first = false; text += Quote(path); }
        return text + "]}\n";
    }
    static size_t Cost(const Tree& tree) {
        size_t result{};
        for (const auto& [path, values] : tree) {
            result += KeyCost(path);
            for (const auto& [name, value] : values) result += ValueCost(name, value.bytes.size());
        }
        return result;
    }
    // Caller holds saving, then mutex. Never publish a partially saved import.
    void Commit(Tree next) {
        const auto cost = 256 + Cost(next) + MaskCost();
        if (cost > kMaximumFile) throw std::runtime_error("Plugin registry capacity exceeded");
        const auto text = Serialize(next, masked_keys);
        if (text.size() > kMaximumFile || !Save(file, text))
            throw std::runtime_error("Cannot save imported plugin configuration");
        tree.swap(next);
        storage_cost = cost;
        saved = ++revision;
        save_error = ERROR_SUCCESS;
    }
    LSTATUS Flush() {
        std::lock_guard serial(saving);
        Tree snapshot; std::set<std::wstring> masked; uint64_t version{};
        { std::lock_guard lock(mutex); if (saved == revision) return save_error;
          snapshot = tree; masked = masked_keys; version = revision; }
        const auto text = Serialize(snapshot, masked);
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
LSTATUS FileRegistry::Status(HKEY key) const {
    std::lock_guard lock(impl_->mutex);LSTATUS status{};impl_->Handle(key,0,status);return status;
}
LSTATUS FileRegistry::Open(std::wstring path, REGSAM access, bool create, HKEY* result, DWORD* disposition) {
    if (!result || ((access & KEY_WOW64_64KEY) && (access & KEY_WOW64_32KEY))) return ERROR_INVALID_PARAMETER;
    path = Normal(std::move(path));
    std::lock_guard lock(impl_->mutex);
    if (impl_->handles.size() >= kMaximumHandles) return ERROR_TOO_MANY_OPEN_FILES;
    auto& tree = impl_->tree;
    const auto view = Impl::View(Impl::Key{path, access});
    NativeKey native;
    const auto native_status = impl_->Masked(path) ? ERROR_FILE_NOT_FOUND : native.Open(path, KEY_QUERY_VALUE, view);
    const bool existed = tree.contains(path) || native_status == ERROR_SUCCESS;
    if (!existed && !create) return native_status;
    if (create && !tree.contains(path)) {
        std::vector<std::wstring> missing;
        auto ancestor = path;
        for (;;) {
            if (!tree.contains(ancestor)) missing.push_back(ancestor);
            const auto slash = ancestor.rfind(L'\\');
            if (slash == ancestor.npos) break;
            ancestor.resize(slash);
        }
        size_t cost{};
        for (const auto& name : missing) cost += KeyCost(name);
        if (impl_->storage_cost + cost > kMaximumFile) return ERROR_DISK_FULL;
        for (const auto& name : missing) tree[name];
        impl_->storage_cost += cost;
        ++impl_->revision;
    }
    auto key = std::make_unique<Impl::Key>(Impl::Key{path, access});
    const auto handle = reinterpret_cast<HKEY>(key.get());
    impl_->handles.emplace(handle, std::move(key));
    *result = handle;
    if (disposition) *disposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
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
    LSTATUS status{};
    const auto found = impl_->Handle(key, KEY_QUERY_VALUE, status);
    if (!found) return status;
    Impl::Value value;
    status = impl_->ReadValue(found->path, name, value, Impl::View(*found));
    if (status == ERROR_SUCCESS) { type = value.type; data = std::move(value.bytes); }
    return status;
}
LSTATUS FileRegistry::Set(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size) {
    if ((!data && size) || size > kMaximumFile / 4) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{};
    const auto found = impl_->Handle(key, KEY_SET_VALUE, status);
    if (!found) return status;
    const auto value_name = ValueName(name);
    impl_->Materialize(found->path);
    auto& values = impl_->tree[found->path];
    const auto old = values.find(value_name);
    if(old!=values.end() && !old->second.deleted && old->second.type==type && old->second.bytes.size()==size &&
        (!size || std::memcmp(old->second.bytes.data(),data,size)==0))return ERROR_SUCCESS;
    const size_t old_cost = old == values.end() ? 0 : ValueCost(value_name, old->second.bytes.size());
    const size_t new_cost = ValueCost(value_name, size);
    if (impl_->storage_cost - old_cost + new_cost > kMaximumFile) return ERROR_DISK_FULL;
    Impl::Value value{type, {}, name ? name : L""};
    if (size) value.bytes.assign(data, data + size);
    values.insert_or_assign(value_name, std::move(value));
    impl_->storage_cost = impl_->storage_cost - old_cost + new_cost;
    ++impl_->revision; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Close(HKEY key) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->handles.find(key);
    if (found == impl_->handles.end() || found->second->closed) return ERROR_INVALID_HANDLE;
    found->second->closed = true;
    impl_->retired.push_back(key);
    // DFX polls hundreds of keys per second. Bound retired handles instead of
    // exhausting a lifetime allocation limit after a few minutes of playback.
    // As with native HKEYs, callers must never use a handle after closing it.
    if(impl_->retired.size()>4096) {impl_->handles.erase(impl_->retired.front());impl_->retired.pop_front();}
    return ERROR_SUCCESS;
}
LSTATUS FileRegistry::Flush() { return impl_->Flush(); }
LSTATUS FileRegistry::EnumKey(HKEY key, DWORD index, std::wstring& name) {
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{}; const auto found = impl_->Handle(key, KEY_ENUMERATE_SUB_KEYS, status);
    if (!found) return status;
    const auto children = impl_->Children(*found);
    if (index >= children.size()) return ERROR_NO_MORE_ITEMS;
    name = *std::next(children.begin(), index); return ERROR_SUCCESS;
}
LSTATUS FileRegistry::EnumValue(HKEY key, DWORD index, std::wstring& name, DWORD& type, std::vector<BYTE>& data) {
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{}; const auto found = impl_->Handle(key, KEY_QUERY_VALUE, status);
    if (!found) return status;
    const auto values = impl_->Values(found->path, Impl::View(*found));
    if (index >= values.size()) return ERROR_NO_MORE_ITEMS;
    const auto& [label, value] = *std::next(values.begin(), index);
    name = value.name; type = value.type; data = value.bytes; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::DeleteValue(HKEY key, const wchar_t* name) {
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{}; const auto found = impl_->Handle(key, KEY_SET_VALUE, status);
    if (!found) return status;
    Impl::Value previous;
    status = impl_->ReadValue(found->path, name, previous, Impl::View(*found));
    if (status != ERROR_SUCCESS) return status;
    const auto next_cost = ValueCost(ValueName(name), 0);
    if (impl_->storage_cost + next_cost > kMaximumFile) return ERROR_DISK_FULL;
    impl_->Materialize(found->path);
    auto& values = impl_->tree[found->path];
    const auto item = values.find(ValueName(name));
    if (item != values.end()) impl_->storage_cost -= ValueCost(item->first, item->second.bytes.size());
    values[ValueName(name)] = Impl::Value{0, {}, name ? name : L"", true};
    impl_->storage_cost += next_cost;
    ++impl_->revision; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::DeleteKey(HKEY key, const wchar_t* subkey) {
    if (!subkey || !*subkey) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{}; const auto parent = impl_->Handle(key, 0, status);
    if (!parent) return status;
    const auto path = Normal(parent->path + L"\\" + subkey);
    auto& tree = impl_->tree;
    const auto found = tree.find(path);
    NativeKey native;
    if (found == tree.end() && (impl_->Masked(path) || native.Open(path) != ERROR_SUCCESS)) return ERROR_FILE_NOT_FOUND;
    if (!impl_->Children(Impl::Key{path}).empty()) return ERROR_ACCESS_DENIED;
    if (impl_->storage_cost + KeyCost(path) > kMaximumFile) return ERROR_DISK_FULL;
    if (found != tree.end()) {
        impl_->storage_cost -= KeyCost(path);
        for (const auto& [name, value] : found->second) impl_->storage_cost -= ValueCost(name, value.bytes.size());
        tree.erase(found);
    }
    if (impl_->masked_keys.insert(path).second) impl_->storage_cost += KeyCost(path);
    for (auto& [handle, open] : impl_->handles)
        if (open->path == path) open->deleted = true;
    ++impl_->revision; return ERROR_SUCCESS;
}
LSTATUS FileRegistry::QueryInfo(HKEY key, DWORD& subkeys, DWORD& max_subkey, DWORD& values,
                               DWORD& max_value_name, DWORD& max_value_data, bool ansi) {
    std::lock_guard lock(impl_->mutex);
    LSTATUS status{}; const auto found = impl_->Handle(key, KEY_QUERY_VALUE, status);
    if (!found) return status;
    const auto children = impl_->Children(*found);
    const auto items = impl_->Values(found->path, Impl::View(*found));
    subkeys = static_cast<DWORD>(children.size()); max_subkey = 0;
    const auto length=[ansi](const std::wstring& name)->DWORD {
        return ansi?static_cast<DWORD>(WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,nullptr,0,nullptr,nullptr)-1):static_cast<DWORD>(name.size());
    };
    for (const auto& name : children) max_subkey = std::max(max_subkey, length(name));
    values = static_cast<DWORD>(items.size()); max_value_name = max_value_data = 0;
    for (const auto& [name, value] : items) {
        max_value_name = std::max(max_value_name, length(value.name));
        max_value_data = std::max(max_value_data, static_cast<DWORD>(value.bytes.size()));
    }
    return ERROR_SUCCESS;
}
void FileRegistry::ConfigureDfxPaths(const std::filesystem::path& plugin, const std::filesystem::path& host) {
    std::lock_guard serial(impl_->saving);
    std::lock_guard lock(impl_->mutex);
    auto next = impl_->tree;
    const auto set = [&](const wchar_t* key, const std::filesystem::path& value) {
        const auto text = value.native();
        const auto* begin = reinterpret_cast<const BYTE*>(text.c_str());
        auto path=Normal(std::wstring(L"hkey_local_machine\\software\\dfx\\") + key);
        next[path][L""] =
            Impl::Value{REG_SZ, {begin, begin + (text.size() + 1) * sizeof(wchar_t)}};
        for(auto at=path.rfind(L'\\');at!=path.npos;at=path.rfind(L'\\')) {path.resize(at);next[path];}
    };
    set(L"11\\host_plugin_folder", plugin.parent_path());
    set(L"11\\top_folder", plugin.parent_path() / L"DFX");
    set(L"11\\top_host_folder", host);
    set(L"shared\\top_shared_folder", plugin.parent_path() / L"DFX");
    impl_->Commit(std::move(next));
}
void FileRegistry::Import(const std::filesystem::path& file) {
    // Parse and validate everything before atomically publishing a merged tree.
    // A REG file is data only: never passed to regedit or written to native hives.
    const auto bytes = Read(file);
    std::wstring text;
    if (bytes.size() >= 2 && BYTE(bytes[0]) == 0xff && BYTE(bytes[1]) == 0xfe) {
        if (bytes.size() % 2) throw std::runtime_error("Invalid UTF-16 registry file");
        text.resize((bytes.size() - 2) / 2);
        std::memcpy(text.data(), bytes.data() + 2, bytes.size() - 2);
    } else {
        const bool utf8 = bytes.starts_with("\xef\xbb\xbf");
        const auto raw = std::string_view(bytes).substr(utf8 ? 3 : 0);
        const int count = MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, MB_ERR_INVALID_CHARS,
            raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        if (!count) throw std::runtime_error("Invalid registry encoding");
        text.resize(count);
        MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, MB_ERR_INVALID_CHARS,
            raw.data(), static_cast<int>(raw.size()), text.data(), count);
    }
    if (text.find(L'\0') != text.npos) throw std::runtime_error("Invalid registry text");
    const auto trim = [](std::wstring value) {
        const auto first = value.find_first_not_of(L" \t\r");
        return first == value.npos ? std::wstring{} : value.substr(first, value.find_last_not_of(L" \t\r") - first + 1);
    };
    const auto quoted = [](std::wstring_view raw, size_t& at) {
        if (at == raw.size() || raw[at++] != L'"') throw std::runtime_error("Expected registry string");
        std::wstring value;
        while (at < raw.size()) {
            wchar_t c = raw[at++];
            if (c == L'"') return value;
            if (c == L'\\') {
                if (at == raw.size() || (raw[at] != L'\\' && raw[at] != L'"')) throw std::runtime_error("Invalid registry escape");
                c = raw[at++];
            }
            value += c;
        }
        throw std::runtime_error("Unterminated registry string");
    };
    const auto hex = [](std::wstring_view raw) {
        if (raw.empty() || raw.size() > 8) throw std::runtime_error("Invalid registry hex number");
        DWORD result{};
        for (const auto character : raw) {
            const auto c = towlower(character);
            const unsigned digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : 16;
            if (digit == 16) throw std::runtime_error("Invalid registry hex digit");
            result = (result << 4) | digit;
        }
        return result;
    };
    Impl::Tree imported; std::wstring selected; size_t start{}; bool header{};
    const auto line = [&]() {
        const auto end = text.find(L'\n', start);
        auto result = trim(text.substr(start, end == text.npos ? text.npos : end - start));
        start = end == text.npos ? text.size() : end + 1; return result;
    };
    while (start < text.size()) {
        auto current = line();
        if (current.empty() || current[0] == L';') continue;
        if (!header) {
            if (current != L"Windows Registry Editor Version 5.00" && current != L"REGEDIT4") throw std::runtime_error("Invalid registry header");
            header = true; continue;
        }
        if (current[0] == L'[') {
            if (current.back() != L']') throw std::runtime_error("Invalid registry section");
            selected = Normal(current.substr(1, current.size() - 2));
            std::wstring_view subkey;
            if (!SplitRoot(selected, subkey)) throw std::runtime_error("Invalid or deleting registry branch");
            imported[selected]; continue;
        }
        if (selected.empty()) throw std::runtime_error("Missing registry branch");
        size_t at{}; std::wstring name;
        if (current[at] == L'@') ++at; else name = quoted(current, at);
        while (at < current.size() && (current[at] == L' ' || current[at] == L'\t')) ++at;
        if (at == current.size() || current[at++] != L'=') throw std::runtime_error("Invalid registry assignment");
        auto raw = trim(current.substr(at)); Impl::Value value; value.name = name;
        if (!raw.empty() && raw[0] == L'"') {
            at = 0; const auto string = quoted(raw, at);
            if (at != raw.size()) throw std::runtime_error("Unexpected registry suffix");
            value.type = REG_SZ;
            const auto* begin = reinterpret_cast<const BYTE*>(string.c_str());
            value.bytes.assign(begin, begin + (string.size() + 1) * sizeof(wchar_t));
        } else if (raw.size() == 14 && raw.starts_with(L"dword:")) {
            const DWORD number = hex(std::wstring_view(raw).substr(6)); value.type = REG_DWORD;
            const auto* begin = reinterpret_cast<const BYTE*>(&number); value.bytes.assign(begin, begin + sizeof(number));
        } else if (raw.starts_with(L"hex:") || raw.starts_with(L"hex(")) {
            size_t offset = 4; value.type = REG_BINARY;
            if (raw[3] == L'(') {
                const auto close = raw.find(L"):", 4);
                if (close == raw.npos) throw std::runtime_error("Invalid registry hex type");
                value.type = hex(std::wstring_view(raw).substr(4, close - 4)); offset = close + 2;
            }
            while (raw.ends_with(L"\\")) {
                if (start >= text.size()) throw std::runtime_error("Incomplete registry continuation");
                raw.pop_back(); raw += line();
            }
            for (at = offset; at < raw.size();) {
                const auto comma = raw.find(L',', at);
                const auto byte = trim(raw.substr(at, comma == raw.npos ? raw.npos : comma - at));
                if (byte.size() != 2) throw std::runtime_error("Invalid registry byte");
                value.bytes.push_back(static_cast<BYTE>(hex(byte)));
                if (comma == raw.npos) break;
                at = comma + 1;
                if (at == raw.size()) throw std::runtime_error("Missing registry byte");
            }
            if ((value.type == REG_SZ || value.type == REG_EXPAND_SZ || value.type == REG_MULTI_SZ) && value.bytes.size() % 2)
                throw std::runtime_error("Invalid UTF-16 registry value");
        } else throw std::runtime_error("Unsupported or deleting registry value");
        ValueName(name.c_str());
        imported[selected].insert_or_assign(ValueName(name.c_str()), std::move(value));
    }
    if (!header || imported.empty()) throw std::runtime_error("Empty registry import");
    std::lock_guard serial(impl_->saving); std::lock_guard lock(impl_->mutex);
    auto next = impl_->tree;
    for (auto& [path, values] : imported) {
        auto& target = next[path];
        for (auto& [name, value] : values) target.insert_or_assign(name, std::move(value));
        auto ancestor = path;
        for (auto at = ancestor.rfind(L'\\'); at != ancestor.npos; at = ancestor.rfind(L'\\')) { ancestor.resize(at); next[ancestor]; }
    }
    impl_->Commit(std::move(next));
}
void FileRegistry::MigrateLegacyOzone(const std::filesystem::path& file) {
    std::lock_guard serial(impl_->saving); std::lock_guard lock(impl_->mutex);
    if (impl_->tree.contains(kOzone) || !std::filesystem::exists(file)) return;
    Impl legacy(file, true);
    auto next = impl_->tree;
    for (const auto& [path, values] : legacy.tree) next.try_emplace(path, values);
    // The old store is a migration source, not a second writable registry.
    impl_->Commit(std::move(next));
}
void FileRegistry::MigrateEnhancer() {
    constexpr wchar_t logical[] = L"hkey_local_machine\\software\\ioscasoft\\enhancer\\version 017";
    constexpr wchar_t machine[] = L"Software\\Ioscasoft\\Enhancer\\Version 017";
    std::lock_guard serial(impl_->saving); std::lock_guard lock(impl_->mutex);
    if (impl_->tree.contains(logical) || impl_->Masked(logical)) return;
    auto next = impl_->tree; bool found{};
    const auto copy = [&](HKEY root, const std::wstring& path, REGSAM view) {
        for (const auto sub : {L"", L"\\Skin File"}) {
            NativeKey native;
            if (RegOpenKeyExW(root, (path + sub).c_str(), 0, KEY_QUERY_VALUE | view, &native.key) != ERROR_SUCCESS) continue;
            std::vector<wchar_t> name(16384);
            for (DWORD i = 0;; ++i) {
                DWORD size = static_cast<DWORD>(name.size());
                if (RegEnumValueW(native.key, i, name.data(), &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                Impl::Value value;
                if (Impl::NativeValue(native.key, name.data(), value) != ERROR_SUCCESS) continue;
                next[Normal(std::wstring(logical) + sub)][ValueName(name.data())] = std::move(value); found = true;
            }
        }
    };
    copy(HKEY_LOCAL_MACHINE, machine, KEY_WOW64_32KEY);
    // A legacy host sees per-user VirtualStore values over machine defaults.
    BOOL wow{}; IsWow64Process(GetCurrentProcess(), &wow);
    copy(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\") +
        (wow ? L"WOW6432Node\\" : L"") + L"Ioscasoft\\Enhancer\\Version 017", wow ? KEY_WOW64_64KEY : 0);
    if (found) {
        std::wstring ancestor(logical);
        for (auto at = ancestor.rfind(L'\\'); at != ancestor.npos; at = ancestor.rfind(L'\\')) { ancestor.resize(at); next[ancestor]; }
        impl_->Commit(std::move(next));
    }
}

struct PluginRegistry::Impl {
    HMODULE module{};
    size_t image_size{};
    std::shared_ptr<FileRegistry> registry;
    std::filesystem::path plugin;
    std::filesystem::path data_directory;
    bool dfx{};
    bool enhancer{};
    std::mutex handles_mutex;
    std::set<HKEY> enhancer_handles;
    std::unique_ptr<DfxRegistryHelper> helper;
    std::vector<std::wstring> helper_diagnostics;
    std::vector<std::wstring> helper_trace;
    std::function<void(std::wstring_view)> trace;
    std::vector<std::pair<ULONG_PTR*, ULONG_PTR>> imports;
};
namespace {
std::mutex contexts_mutex;
std::vector<std::shared_ptr<PluginRegistry::Impl>> contexts;
using Context = std::shared_ptr<PluginRegistry::Impl>;
thread_local Context broker_context;
std::map<std::wstring,std::weak_ptr<FileRegistry>> stores;
std::shared_ptr<FileRegistry> SharedStore(const std::filesystem::path& path) {
    const auto key=Normal(std::filesystem::absolute(path).lexically_normal().native());
    std::lock_guard lock(contexts_mutex);
    if(auto existing=stores[key].lock()) return existing;
    auto store=std::make_shared<FileRegistry>(path);stores[key]=store;return store;
}
Context FindContext(void* caller, HKEY key = nullptr) {
    if(broker_context) return broker_context;
    std::lock_guard lock(contexts_mutex);
    const auto address = reinterpret_cast<ULONG_PTR>(caller);
    for (const auto& context : contexts) {
        const auto base = reinterpret_cast<ULONG_PTR>(context->module);
        if (address >= base && address - base < context->image_size) return context;
    }
    for(const auto& context:contexts)
        if(key && context->registry->Owns(key)) return context;
    return {};
}
void Trace(const Context& c,const wchar_t* api,HKEY key,const wchar_t* name,LSTATUS status) {
    if(!c || !c->trace)return;
    const auto path=c->registry->Owns(key)?c->registry->Path(key):std::to_wstring(reinterpret_cast<ULONG_PTR>(key));
    c->trace(std::wstring(api)+L" key="+path+L" name="+(name?name:L"<default>")+L" status="+std::to_wstring(status));
}
std::wstring Wide(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (!count) throw std::runtime_error("Invalid ANSI registry name");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), count);
    result.pop_back(); return result;
}
bool Within(std::wstring_view path,std::wstring_view root) {
    return path==root || (path.starts_with(root) && path.size()>root.size() && path[root.size()]==L'\\');
}
LSTATUS CloseVirtual(const Context& c, HKEY key) {
    if (c->enhancer) { std::lock_guard lock(c->handles_mutex); c->enhancer_handles.erase(key); }
    return c->registry->Close(key);
}
LSTATUS Open(const Context& c, HKEY parent, const wchar_t* subkey, REGSAM access,
              bool create, HKEY* result, DWORD* disposition) {
    if (!c) return create ? RegCreateKeyExW(parent, subkey, 0, nullptr, 0, access, nullptr, result, disposition)
                          : RegOpenKeyExW(parent, subkey, 0, access, result);
    const bool virtual_parent = c->registry->Owns(parent);
    if (virtual_parent) { const auto status = c->registry->Status(parent); if (status != ERROR_SUCCESS) return status; }
    auto path = virtual_parent ? c->registry->Path(parent) : RootName(parent);
    if (path.empty()) {
        // Handles from outside the adapted plug-in may be read, never written.
        if (create || (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER))) return ERROR_ACCESS_DENIED;
        return RegOpenKeyExW(parent, subkey, 0, access, result);
    }
    if (subkey && *subkey) { path += L'\\'; path += subkey; }
    path = Normal(std::move(path));
    constexpr std::wstring_view old_helper = L"hkey_current_user\\software\\dfx\\8\\11";
    if (c->dfx && broker_context == c && Within(path, old_helper))
        path.replace(0, old_helper.size(), L"hkey_current_user\\software\\dfx\\9\\11");
    const auto status = c->registry->Open(path, access, create, result, disposition);
    if (status == ERROR_SUCCESS && c->enhancer) {
        std::lock_guard lock(c->handles_mutex); c->enhancer_handles.insert(*result);
    }
    return status;
}
struct RootKeyScope {
    Context context;
    HKEY key{}, temporary{};
    LSTATUS status = ERROR_SUCCESS;
    RootKeyScope(Context c, HKEY original, REGSAM access, bool create = false) : context(std::move(c)), key(original) {
        if (context && !context->registry->Owns(key) && !RootName(key).empty()) {
            status = Open(context, key, nullptr, access, create, &temporary, nullptr);
            if (status == ERROR_SUCCESS) key = temporary;
        }
    }
    ~RootKeyScope() { if (temporary) CloseVirtual(context, temporary); }
};
LSTATUS WINAPI OpenW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY result) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegOpenKeyExW(key, sub, options, access, result);
        if (options) return ERROR_INVALID_PARAMETER;
        const auto status=Open(context, key, sub, access, false, result, nullptr);
        Trace(context,L"RegOpenKeyExW",key,sub,status);return status;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI OpenA(HKEY key, LPCSTR sub, DWORD options, REGSAM access, PHKEY result) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegOpenKeyExA(key, sub, options, access, result);
        if (options) return ERROR_INVALID_PARAMETER;
        const auto path=Wide(sub);const auto status=Open(context, key, path.c_str(), access, false, result, nullptr);
        Trace(context,L"RegOpenKeyExA",key,path.c_str(),status);return status;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI CreateW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD options,
    REGSAM access, LPSECURITY_ATTRIBUTES security, PHKEY result, DWORD* disposition) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegCreateKeyExW(key, sub, reserved, cls, options, access, security, result, disposition);
        if (reserved || options || security) {Trace(context,L"RegCreateKeyExW arguments",key,sub,ERROR_INVALID_PARAMETER);return ERROR_INVALID_PARAMETER;}
        const auto status=Open(context, key, sub, access, true, result, disposition);
        Trace(context,L"RegCreateKeyExW",key,sub,status);return status;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI CreateA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD options,
    REGSAM access, LPSECURITY_ATTRIBUTES security, PHKEY result, DWORD* disposition) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        if (!context) return RegCreateKeyExA(key, sub, reserved, cls, options, access, security, result, disposition);
        if (reserved || options || security) return ERROR_INVALID_PARAMETER;
        return Open(context, key, Wide(sub).c_str(), access, true, result, disposition);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI QueryW(HKEY key, LPCWSTR name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        RootKeyScope root(context, key, KEY_QUERY_VALUE); if (root.status) return root.status; key = root.key;
        if (!context || !context->registry->Owns(key)) {
            const auto status=RegQueryValueExW(key,name,reserved,type,data,size);
            Trace(context,L"RegQueryValueExW native",key,name,status);return status;
        }
        if (reserved) return ERROR_INVALID_PARAMETER;
        const auto status=context->registry->Query(key, name, type, data, size);
        Trace(context,L"RegQueryValueExW",key,name,status);return status;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI QueryA(HKEY key, LPCSTR name, DWORD* reserved, DWORD* type, BYTE* data, DWORD* size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        RootKeyScope root(context, key, KEY_QUERY_VALUE); if (root.status) return root.status; key = root.key;
        if (!context || !context->registry->Owns(key)) {
            const auto status=RegQueryValueExA(key,name,reserved,type,data,size);
            Trace(context,L"RegQueryValueExA native",key,Wide(name).c_str(),status);return status;
        }
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
        RootKeyScope root(context, key, KEY_SET_VALUE, true); if (root.status) return root.status; key = root.key;
        if (!context) return RegSetValueExW(key, name, reserved, type, data, size);
        if (!context->registry->Owns(key)) return ERROR_ACCESS_DENIED;
        if (reserved) return ERROR_INVALID_PARAMETER;
        const auto status=context->registry->Set(key, name, type, data, size);
        Trace(context,L"RegSetValueExW",key,name,status);return status;
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI SetA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) noexcept {
    try {
        const auto context = FindContext(_ReturnAddress(), key);
        RootKeyScope root(context, key, KEY_SET_VALUE, true); if (root.status) return root.status; key = root.key;
        if (!context) return RegSetValueExA(key, name, reserved, type, data, size);
        if (!context->registry->Owns(key)) return ERROR_ACCESS_DENIED;
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
        if (context && context->registry->Owns(key)) return CloseVirtual(context, key);
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
LSTATUS CopyName(const std::wstring& name, bool ansi, void* buffer, DWORD* size) {
    if(!size || !buffer) return ERROR_INVALID_PARAMETER;
    const DWORD capacity=*size;
    if(ansi) {
        const int required=WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,nullptr,0,nullptr,nullptr);
        if(!required) return ERROR_NO_UNICODE_TRANSLATION;
        *size=static_cast<DWORD>(required-1);
        if(capacity<static_cast<DWORD>(required)) return ERROR_MORE_DATA;
        WideCharToMultiByte(CP_ACP,0,name.c_str(),-1,static_cast<char*>(buffer),required,nullptr,nullptr);
    } else {
        *size=static_cast<DWORD>(name.size());
        if(capacity<=name.size()) return ERROR_MORE_DATA;
        std::memcpy(buffer,name.c_str(),(name.size()+1)*sizeof(wchar_t));
    }
    return ERROR_SUCCESS;
}
LSTATUS EnumKeyImpl(const Context& context,HKEY key,DWORD index,void* name,DWORD* size,
    DWORD* reserved,void* cls,DWORD* cls_size,FILETIME* time,bool ansi) {
    if(reserved || (cls && !cls_size)) return ERROR_INVALID_PARAMETER;
    std::wstring label;
    const auto status=context->registry->EnumKey(key,index,label);
    if(status!=ERROR_SUCCESS) return status;
    if(time) *time={};
    if(cls_size) {const auto capacity=*cls_size;*cls_size=0;if(cls && !capacity)return ERROR_MORE_DATA;}
    if(cls) {if(ansi)*static_cast<char*>(cls)=0;else *static_cast<wchar_t*>(cls)=0;}
    return CopyName(label,ansi,name,size);
}
LSTATUS WINAPI EnumKeyW(HKEY key,DWORD index,LPWSTR name,DWORD* size,DWORD* reserved,LPWSTR cls,DWORD* cls_size,FILETIME* time) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;
        return c && c->registry->Owns(key)?EnumKeyImpl(c,key,index,name,size,reserved,cls,cls_size,time,false):RegEnumKeyExW(key,index,name,size,reserved,cls,cls_size,time);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI EnumKeyA(HKEY key,DWORD index,LPSTR name,DWORD* size,DWORD* reserved,LPSTR cls,DWORD* cls_size,FILETIME* time) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;
        return c && c->registry->Owns(key)?EnumKeyImpl(c,key,index,name,size,reserved,cls,cls_size,time,true):RegEnumKeyExA(key,index,name,size,reserved,cls,cls_size,time);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS EnumValueImpl(const Context& c,HKEY key,DWORD index,void* name,DWORD* name_size,
    DWORD* reserved,DWORD* type,BYTE* data,DWORD* size,bool ansi) {
    if(reserved || (data && !size)) return ERROR_INVALID_PARAMETER;
    std::wstring label;std::vector<BYTE> bytes;DWORD kind{};
    const auto status=c->registry->EnumValue(key,index,label,kind,bytes);
    if(status!=ERROR_SUCCESS)return status;
    if(ansi && (kind==REG_SZ || kind==REG_EXPAND_SZ || kind==REG_MULTI_SZ)) {
        if(bytes.size()%2)return ERROR_INVALID_DATA;
        const auto count=static_cast<int>(bytes.size()/2);
        const auto* wide=reinterpret_cast<const wchar_t*>(bytes.data());
        const int length=count?WideCharToMultiByte(CP_ACP,0,wide,count,nullptr,0,nullptr,nullptr):0;
        std::vector<BYTE> converted(length);
        if(length)WideCharToMultiByte(CP_ACP,0,wide,count,reinterpret_cast<char*>(converted.data()),length,nullptr,nullptr);
        bytes=std::move(converted);
    }
    if(type)*type=kind;
    const DWORD capacity=size?*size:0;if(size)*size=static_cast<DWORD>(bytes.size());
    const auto copied=CopyName(label,ansi,name,name_size);
    if(copied!=ERROR_SUCCESS)return copied;
    if(data && capacity<bytes.size())return ERROR_MORE_DATA;
    if(data && !bytes.empty())std::memcpy(data,bytes.data(),bytes.size());
    return ERROR_SUCCESS;
}
LSTATUS WINAPI EnumValueW(HKEY key,DWORD index,LPWSTR name,DWORD* name_size,DWORD* reserved,DWORD* type,BYTE* data,DWORD* size) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;
        return c && c->registry->Owns(key)?EnumValueImpl(c,key,index,name,name_size,reserved,type,data,size,false):RegEnumValueW(key,index,name,name_size,reserved,type,data,size);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI EnumValueA(HKEY key,DWORD index,LPSTR name,DWORD* name_size,DWORD* reserved,DWORD* type,BYTE* data,DWORD* size) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;
        return c && c->registry->Owns(key)?EnumValueImpl(c,key,index,name,name_size,reserved,type,data,size,true):RegEnumValueA(key,index,name,name_size,reserved,type,data,size);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS DeleteValueImpl(const Context& c,HKEY key,const wchar_t* name) {
    RootKeyScope root(c, key, KEY_SET_VALUE); if (root.status) return root.status; key = root.key;
    if(!c->registry->Owns(key))return ERROR_ACCESS_DENIED;
    return c->registry->DeleteValue(key,name);
}
LSTATUS WINAPI DeleteValueW(HKEY key,LPCWSTR name) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);return c?DeleteValueImpl(c,key,name):RegDeleteValueW(key,name);}catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI DeleteValueA(HKEY key,LPCSTR name) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);return c?DeleteValueImpl(c,key,Wide(name).c_str()):RegDeleteValueA(key,name);}catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS DeleteKeyImpl(const Context& c,HKEY key,const wchar_t* subkey) {
    HKEY parent{};const auto status=Open(c,key,L"",KEY_READ,false,&parent,nullptr);
    if(status!=ERROR_SUCCESS)return status;
    const bool owned=c->registry->Owns(parent);
    LSTATUS result=ERROR_ACCESS_DENIED;
    if(owned && subkey)
        result=c->registry->DeleteKey(parent,subkey);
    if(owned)CloseVirtual(c,parent);else RegCloseKey(parent);
    return result;
}
LSTATUS WINAPI DeleteKeyW(HKEY key,LPCWSTR name) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);return c?DeleteKeyImpl(c,key,name):RegDeleteKeyW(key,name);}catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI DeleteKeyA(HKEY key,LPCSTR name) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);return c?DeleteKeyImpl(c,key,Wide(name).c_str()):RegDeleteKeyA(key,name);}catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS InfoImpl(const Context& c,HKEY key,void* cls,DWORD* cls_size,DWORD* reserved,DWORD* subkeys,
    DWORD* max_subkey,DWORD* max_class,DWORD* values,DWORD* max_name,DWORD* max_data,DWORD* security,FILETIME* time,bool ansi) {
    if(reserved || (cls && !cls_size))return ERROR_INVALID_PARAMETER;
    DWORD count{},length{},items{},names{},bytes{};
    const auto status=c->registry->QueryInfo(key,count,length,items,names,bytes,ansi);
    if(status!=ERROR_SUCCESS)return status;
    if(subkeys)*subkeys=count;if(max_subkey)*max_subkey=length;if(max_class)*max_class=0;
    if(values)*values=items;if(max_name)*max_name=names;if(max_data)*max_data=bytes;
    if(security)*security=0;if(time)*time={};
    if(cls_size){const auto capacity=*cls_size;*cls_size=0;if(cls && !capacity)return ERROR_MORE_DATA;}
    if(cls){if(ansi)*static_cast<char*>(cls)=0;else *static_cast<wchar_t*>(cls)=0;}
    return ERROR_SUCCESS;
}
LSTATUS WINAPI InfoW(HKEY key,LPWSTR cls,DWORD* cls_size,DWORD* reserved,DWORD* subkeys,DWORD* max_subkey,DWORD* max_class,
    DWORD* values,DWORD* max_name,DWORD* max_data,DWORD* security,FILETIME* time) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;return c && c->registry->Owns(key)
        ?InfoImpl(c,key,cls,cls_size,reserved,subkeys,max_subkey,max_class,values,max_name,max_data,security,time,false)
        :RegQueryInfoKeyW(key,cls,cls_size,reserved,subkeys,max_subkey,max_class,values,max_name,max_data,security,time);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI InfoA(HKEY key,LPSTR cls,DWORD* cls_size,DWORD* reserved,DWORD* subkeys,DWORD* max_subkey,DWORD* max_class,
    DWORD* values,DWORD* max_name,DWORD* max_data,DWORD* security,FILETIME* time) noexcept {
    try {const auto c=FindContext(_ReturnAddress(),key);
        RootKeyScope root(c,key,KEY_READ);if(root.status)return root.status;key=root.key;return c && c->registry->Owns(key)
        ?InfoImpl(c,key,cls,cls_size,reserved,subkeys,max_subkey,max_class,values,max_name,max_data,security,time,true)
        :RegQueryInfoKeyA(key,cls,cls_size,reserved,subkeys,max_subkey,max_class,values,max_name,max_data,security,time);
    }catch(...){return ERROR_INVALID_HANDLE;}
}
LSTATUS WINAPI BasicOpenW(HKEY key,LPCWSTR sub,HKEY* result) noexcept {try {return Open(FindContext(_ReturnAddress(),key),key,sub,KEY_ALL_ACCESS,false,result,nullptr);}catch(...){return ERROR_INVALID_HANDLE;}}
LSTATUS WINAPI BasicOpenA(HKEY key,LPCSTR sub,HKEY* result) noexcept {try {return Open(FindContext(_ReturnAddress(),key),key,Wide(sub).c_str(),KEY_ALL_ACCESS,false,result,nullptr);}catch(...){return ERROR_INVALID_HANDLE;}}
LSTATUS WINAPI BasicCreateW(HKEY key,LPCWSTR sub,HKEY* result) noexcept {try {return Open(FindContext(_ReturnAddress(),key),key,sub,KEY_ALL_ACCESS,true,result,nullptr);}catch(...){return ERROR_INVALID_HANDLE;}}
LSTATUS WINAPI BasicCreateA(HKEY key,LPCSTR sub,HKEY* result) noexcept {try {return Open(FindContext(_ReturnAddress(),key),key,Wide(sub).c_str(),KEY_ALL_ACCESS,true,result,nullptr);}catch(...){return ERROR_INVALID_HANDLE;}}
LSTATUS BasicQuery(const Context& c, HKEY parent, const wchar_t* sub, void* data, LONG* size, bool ansi) {
    if (!size || *size < 0) return ERROR_INVALID_PARAMETER;
    HKEY key{}; auto status = Open(c, parent, sub, KEY_QUERY_VALUE, false, &key, nullptr);
    if (status != ERROR_SUCCESS) return status;
    DWORD bytes = static_cast<DWORD>(*size), type{};
    status = ansi ? QueryA(key, nullptr, nullptr, &type, static_cast<BYTE*>(data), &bytes)
                  : QueryW(key, nullptr, nullptr, &type, static_cast<BYTE*>(data), &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        bytes = ansi ? 1 : sizeof(wchar_t);
        status = data && static_cast<DWORD>(*size) < bytes ? ERROR_MORE_DATA : ERROR_SUCCESS;
        if (data && status == ERROR_SUCCESS) std::memset(data, 0, bytes);
    }
    if (c && c->registry->Owns(key)) CloseVirtual(c, key); else RegCloseKey(key);
    *size = static_cast<LONG>(bytes); return status;
}
LSTATUS WINAPI BasicQueryA(HKEY key, LPCSTR sub, LPSTR data, LONG* size) noexcept {
    try {
        const auto c = FindContext(_ReturnAddress(), key);
        if (!c) return RegQueryValueA(key, sub, data, size);
        return BasicQuery(c, key, Wide(sub).c_str(), data, size, true);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI BasicQueryW(HKEY key, LPCWSTR sub, LPWSTR data, LONG* size) noexcept {
    try {
        const auto c = FindContext(_ReturnAddress(), key);
        if (!c) return RegQueryValueW(key, sub, data, size);
        return BasicQuery(c, key, sub, data, size, false);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS BasicSet(const Context& c, HKEY parent, const wchar_t* sub, DWORD type, const void* data, bool ansi) {
    if (type != REG_SZ || !data) return ERROR_INVALID_PARAMETER;
    HKEY key{}; auto status = Open(c, parent, sub, KEY_SET_VALUE, true, &key, nullptr);
    if (status != ERROR_SUCCESS) return status;
    if (ansi) {
        const auto string = static_cast<const char*>(data);
        status = SetA(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(string), static_cast<DWORD>(strlen(string) + 1));
    } else {
        const auto string = static_cast<const wchar_t*>(data);
        status = SetW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(string), static_cast<DWORD>((wcslen(string) + 1) * sizeof(wchar_t)));
    }
    CloseVirtual(c, key); return status;
}
LSTATUS WINAPI BasicSetA(HKEY key, LPCSTR sub, DWORD type, LPCSTR data, DWORD size) noexcept {
    try {
        const auto c = FindContext(_ReturnAddress(), key);
        if (!c) return RegSetValueA(key, sub, type, data, size);
        return BasicSet(c, key, Wide(sub).c_str(), type, data, true);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
LSTATUS WINAPI BasicSetW(HKEY key, LPCWSTR sub, DWORD type, LPCWSTR data, DWORD size) noexcept {
    try {
        const auto c = FindContext(_ReturnAddress(), key);
        if (!c) return RegSetValueW(key, sub, type, data, size);
        return BasicSet(c, key, sub, type, data, false);
    } catch (...) { return ERROR_INVALID_HANDLE; }
}
FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) noexcept;
BOOL WINAPI StopOzoneThread(HANDLE thread, DWORD exit_code) noexcept {
    try {
        const auto caller = _ReturnAddress();
        const auto context = FindContext(caller);
        // In this exact binary 59445706 has already cleared object+100, and
        // its worker (59443D30 / 594441C2) polls that flag. TerminateThread at
        // 5944570D can abandon the loader lock during DLL_THREAD_ATTACH.
        if (!context || context->dfx || context->enhancer || reinterpret_cast<BYTE*>(caller) !=
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
BOOL WINAPI DfxFolderW(HWND owner,LPWSTR output,int folder,BOOL create) noexcept {
    try {
        const auto context=FindContext(_ReturnAddress());
        if(!context || !context->dfx)return SHGetSpecialFolderPathW(owner,output,folder,create);
        const wchar_t* suffix=folder==CSIDL_COMMON_APPDATA?L"CommonData":folder==CSIDL_LOCAL_APPDATA?L"UserData":folder==CSIDL_PERSONAL?L"Documents":nullptr;
        if(!suffix || !output)return FALSE;
        const auto path=context->data_directory/suffix;
        if(path.native().size()>=MAX_PATH)return FALSE;
        if(create)std::filesystem::create_directories(path);
        wcscpy_s(output,MAX_PATH,path.c_str());return TRUE;
    } catch(...) {return FALSE;}
}
BOOL WINAPI DfxFolderA(HWND owner,LPSTR output,int folder,BOOL create) noexcept {
    wchar_t path[MAX_PATH]{};
    if(!output || !DfxFolderW(owner,path,folder,create))return FALSE;
    BOOL replaced{};
    if(WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,path,-1,output,MAX_PATH,nullptr,&replaced)>0 && !replaced)return TRUE;
    wchar_t short_path[MAX_PATH]{};
    const auto size=GetShortPathNameW(path,short_path,MAX_PATH);
    replaced=FALSE;
    return size && size<MAX_PATH && WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,short_path,-1,output,MAX_PATH,nullptr,&replaced)>0 && !replaced;
}
HWND WINAPI DfxFindWindowA(LPCSTR cls,LPCSTR title) noexcept {
    if(!cls || !title || strcmp(cls,"DFX_WINDOW_2006") || strcmp(title,"DFX 8 Winamp"))return nullptr;
    HWND found{};
    EnumWindows([](HWND window,LPARAM parameter)->BOOL {
        DWORD pid{};GetWindowThreadProcessId(window,&pid);
        wchar_t name[64]{};GetClassNameW(window,name,64);
        if(pid==GetCurrentProcessId() && wcscmp(name,L"DFX_WINDOW_11")==0) {
            *reinterpret_cast<HWND*>(parameter)=window;return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
FARPROC Replacement(std::string_view name) {
#define ENTRY(api, replacement) if (name == #api) return reinterpret_cast<FARPROC>(replacement)
    ENTRY(RegOpenKeyExA, OpenA); ENTRY(RegOpenKeyExW, OpenW);
    ENTRY(RegCreateKeyExA, CreateA); ENTRY(RegCreateKeyExW, CreateW);
    ENTRY(RegQueryValueExA, QueryA); ENTRY(RegQueryValueExW, QueryW);
    ENTRY(RegSetValueExA, SetA); ENTRY(RegSetValueExW, SetW);
    ENTRY(RegCloseKey, Close); ENTRY(RegFlushKey, Flush);
    ENTRY(RegEnumKeyExA, EnumKeyA); ENTRY(RegEnumKeyExW, EnumKeyW);
    ENTRY(RegEnumValueA, EnumValueA); ENTRY(RegEnumValueW, EnumValueW);
    ENTRY(RegDeleteValueA, DeleteValueA); ENTRY(RegDeleteValueW, DeleteValueW);
    ENTRY(RegDeleteKeyA, DeleteKeyA); ENTRY(RegDeleteKeyW, DeleteKeyW);
    ENTRY(RegQueryInfoKeyA, InfoA); ENTRY(RegQueryInfoKeyW, InfoW);
    ENTRY(RegOpenKeyA, BasicOpenA); ENTRY(RegOpenKeyW, BasicOpenW);
    ENTRY(RegCreateKeyA, BasicCreateA); ENTRY(RegCreateKeyW, BasicCreateW);
    ENTRY(RegQueryValueA, BasicQueryA); ENTRY(RegQueryValueW, BasicQueryW);
    ENTRY(RegSetValueA, BasicSetA); ENTRY(RegSetValueW, BasicSetW);
    ENTRY(GetProcAddress, Resolve);
    ENTRY(TerminateThread, StopOzoneThread);
    ENTRY(SHGetSpecialFolderPathW, DfxFolderW); ENTRY(SHGetSpecialFolderPathA, DfxFolderA);
    ENTRY(FindWindowA,DfxFindWindowA);
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
    if (_wcsicmp(plugin.filename().c_str(), L"dsp_enh.dll") == 0)
        return update::Sha256(plugin) == "55eb2f2dece655a491141376916f4488a6714e221298fab26032d647fff2287f";
    if (_wcsicmp(plugin.filename().c_str(), L"dsp_dfx.dll") == 0)
        return update::Sha256(plugin) == "987e7d531b92df9a582c1ab803f92856e948cc87f0a6b1ed900d3a7898a0eb03";
    if (_wcsicmp(plugin.filename().c_str(), L"dsp_izOzone.dll") != 0) return false;
    return update::Sha256(plugin) == "e3bb0eef979ea8016fb1278b373c7c70ae4507719524bfefe802b5bc3c800e59";
}
std::unique_ptr<PluginRegistry> PluginRegistry::Attach(HMODULE module,
    const std::filesystem::path& plugin, const std::filesystem::path& directory) {
    if (!Supports(plugin)) return {};
    auto context = std::make_shared<Impl>(); context->module = module;
    context->plugin=plugin;
    context->dfx=_wcsicmp(plugin.filename().c_str(),L"dsp_dfx.dll")==0;
    context->enhancer=_wcsicmp(plugin.filename().c_str(),L"dsp_enh.dll")==0;
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    context->image_size = nt->OptionalHeader.SizeOfImage;
    const auto folder = directory.empty() ? plugin.parent_path() / L"PluginState" : directory;
    context->data_directory=folder/L"Dsp_Dfx";
    const auto legacy_folder = directory.empty() ? plugin.parent_path() / L"Ozone" : directory / L"Ozone";
    const auto file = folder / L"registry.json";
    context->registry = SharedStore(file);
    context->registry->MigrateLegacyOzone(legacy_folder / L"registry.json");
    if (context->registry->Empty()) {
        auto import = folder / L"registry-import.reg";
        if (!std::filesystem::exists(import)) import = legacy_folder / L"registry-import.reg";
        if (std::filesystem::exists(import)) context->registry->Import(import);
    }
    if (context->dfx) context->registry->ConfigureDfxPaths(plugin, update::ExecutablePath().parent_path());
    if (context->enhancer) context->registry->MigrateEnhancer();
    { std::lock_guard lock(contexts_mutex); contexts.push_back(context); }
    auto attachment = std::unique_ptr<PluginRegistry>(new PluginRegistry(context));
    if (context->enhancer) {
        // PECompact overwrites its import-name table. Patch only the audited
        // decompressed business IAT, after checking every original pointer.
        constexpr std::pair<DWORD, const char*> slots[] = {
            {0xf000, "RegCreateKeyExA"}, {0xf004, "RegSetValueA"}, {0xf008, "RegSetValueExA"},
            {0xf00c, "RegQueryValueA"}, {0xf010, "RegEnumValueA"}
        };
        for (const auto& [rva, name] : slots) {
            if (rva + sizeof(ULONG_PTR) > context->image_size ||
                *reinterpret_cast<FARPROC*>(base + rva) != GetProcAddress(GetModuleHandleW(L"advapi32.dll"), name))
                throw std::runtime_error("Enhancer business imports do not match the audited binary");
        }
        for (const auto& [rva, name] : slots) {
            auto* slot = reinterpret_cast<ULONG_PTR*>(base + rva); DWORD previous{};
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) throw std::runtime_error("Cannot adapt Enhancer imports");
            context->imports.emplace_back(slot, *slot);
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<void*>(Replacement(name)));
            DWORD ignored{}; VirtualProtect(slot, sizeof(*slot), previous, &ignored);
        }
        return attachment;
    }
    const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva);
    for (; descriptor->Name; ++descriptor) {
        const auto* library = reinterpret_cast<const char*>(base + descriptor->Name);
        const bool registry = _stricmp(library, "ADVAPI32.dll") == 0;
        if (!registry && _stricmp(library, "KERNEL32.dll") != 0 &&
            !(context->dfx && _stricmp(library,"SHELL32.dll")==0)) continue;
        if (!descriptor->OriginalFirstThunk) throw std::runtime_error("Plugin imports have no name table");
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            const auto replacement = Replacement(reinterpret_cast<const char*>(name));
            if (!replacement) {
                if (registry && std::strncmp(reinterpret_cast<const char*>(name), "Reg", 3) == 0)
                    throw std::runtime_error("Uncovered plugin registry import");
                continue;
            }
            auto* slot = &slots->u1.Function;
            DWORD previous{};
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) throw std::runtime_error("Cannot adapt plugin imports");
            context->imports.emplace_back(slot, *slot);
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<void*>(replacement));
            DWORD ignored{}; VirtualProtect(slot, sizeof(*slot), previous, &ignored);
        }
    }
    return attachment;
}
PluginRegistry::PluginRegistry(std::shared_ptr<Impl> state) : impl_(std::move(state)) {}
PluginRegistry::~PluginRegistry() {
    impl_->helper.reset();
    Restore(*impl_);
    { std::lock_guard lock(impl_->handles_mutex);
      for (const auto key : impl_->enhancer_handles) impl_->registry->Close(key);
      impl_->enhancer_handles.clear(); }
    std::lock_guard lock(contexts_mutex);
    std::erase(contexts, impl_);
}
bool PluginRegistry::Flush() noexcept {
    try { return impl_->registry->Flush() == ERROR_SUCCESS; }
    catch (...) { return false; }
}
bool PluginRegistry::ConfigureDfx(HWND) {
    if(!impl_->dfx)return false;
    if(impl_->helper && impl_->helper->Running())return true;
    if(impl_->helper) {
        auto messages=impl_->helper->Diagnostics();
        impl_->helper_diagnostics.insert(impl_->helper_diagnostics.end(),messages.begin(),messages.end());
        if(impl_->helper_diagnostics.size()>64)impl_->helper_diagnostics.erase(impl_->helper_diagnostics.begin(),impl_->helper_diagnostics.end()-64);
        const auto trace=impl_->helper->Trace();
        for(const auto& line:trace)if(impl_->helper_trace.size()<2048)impl_->helper_trace.push_back(line);
        impl_->helper.reset();
    }
    const std::weak_ptr<Impl> weak=impl_;
    impl_->helper=std::make_unique<DfxRegistryHelper>(impl_->plugin.parent_path()/L"DFX"/L"Apps"/L"dfxwsettings.exe",
        [weak](std::string_view name,std::array<ULONG_PTR,12>& a)->LSTATUS {
            const auto context=weak.lock();if(!context)return ERROR_INVALID_HANDLE;
            struct Scope {Context old;Scope(Context now):old(std::exchange(broker_context,std::move(now))){}~Scope(){broker_context=std::move(old);}} scope(context);
            const auto function=Replacement(name);
            if(!function)return ERROR_CALL_NOT_IMPLEMENTED;
            if(name=="FindWindowA")
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1]);
            if(name=="SHGetSpecialFolderPathA")
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3]);
            // All supported Win32 registry APIs use stdcall and 32-bit slots.
            if(name=="RegCloseKey" || name=="RegFlushKey")
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR)>(function)(a[0]);
            if(name.starts_with("RegDelete"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1]);
            if(name=="RegOpenKeyA" || name=="RegOpenKeyW" || name=="RegCreateKeyA" || name=="RegCreateKeyW")
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2]);
            if(name.starts_with("RegOpenKeyEx"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3],a[4]);
            if(name.starts_with("RegQueryValueEx") || name.starts_with("RegSetValueEx"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3],a[4],a[5]);
            if(name.starts_with("RegEnum"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
            if(name.starts_with("RegCreateKeyEx"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);
            if(name.starts_with("RegQueryInfoKey"))
                return reinterpret_cast<LSTATUS(WINAPI*)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR)>(function)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11]);
            return ERROR_CALL_NOT_IMPLEMENTED;
        });
    return true;
}
std::vector<std::wstring> PluginRegistry::Diagnostics() {
    auto messages=std::exchange(impl_->helper_diagnostics,{});
    if(impl_->helper) {auto current=impl_->helper->Diagnostics();messages.insert(messages.end(),current.begin(),current.end());}
    return messages;
}
std::vector<std::wstring> PluginRegistry::HelperTrace() {
    auto result=impl_->helper_trace;
    if(impl_->helper)for(const auto& line:impl_->helper->Trace())if(result.size()<4096)result.push_back(line);
    return result;
}
void PluginRegistry::SetTraceSink(std::function<void(std::wstring_view)> sink){impl_->trace=std::move(sink);}
} // namespace ttplayer::audio::detail
