#include "ttplayer/settings/file_association.h"
#include "ttplayer/i18n/i18n.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <functional>
#include <map>
#include <variant>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propsys.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace ttplayer::settings {
namespace {

// Keep the recovery metadata separate from the legacy program's unqualified
// "Backup" value.  That value may still be needed if an original TTPlayer
// installation owns the class before the rebuild is selected.
constexpr wchar_t kOwnerValue[] = L"TTPlayer.Rebuild.Owner";
constexpr wchar_t kInstalledValue[] = L"TTPlayer.Rebuild.Installed";
constexpr wchar_t kBackupValue[] = L"TTPlayer.Rebuild.Backup";
constexpr wchar_t kBackupPresentValue[] = L"TTPlayer.Rebuild.BackupPresent";
constexpr wchar_t kKeyExistedValue[] = L"TTPlayer.Rebuild.KeyExisted";

struct RegKeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key) RegCloseKey(key);
    }
};
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

template<class Interface>
struct ComReleaser {
    void operator()(Interface* value) const noexcept {
        if (value) value->Release();
    }
};
template<class Interface>
using UniqueComPtr = std::unique_ptr<Interface, ComReleaser<Interface>>;

struct ManagedChange {
    FileAssociationResult result;
    bool changed{};
};

[[nodiscard]] std::wstring Win32Message(DWORD error) {
    wchar_t* allocated = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&allocated), 0, nullptr);
    if (!length || !allocated) return i18n::Text(L"Windows error ") + std::to_wstring(error);
    std::wstring message(allocated, length);
    LocalFree(allocated);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

[[nodiscard]] FileAssociationResult Success(bool changed = false) {
    FileAssociationResult result;
    result.changed = changed;
    return result;
}

[[nodiscard]] FileAssociationResult Invalid(std::wstring operation,
                                            std::wstring message) {
    FileAssociationResult result;
    result.error = FileAssociationError::invalid_argument;
    result.native_error = ERROR_INVALID_PARAMETER;
    result.hresult = E_INVALIDARG;
    result.operation = std::move(operation);
    result.message = std::move(message);
    return result;
}

[[nodiscard]] FileAssociationResult Win32Failure(FileAssociationError kind,
                                                 std::wstring operation,
                                                 DWORD error) {
    FileAssociationResult result;
    result.error = kind;
    result.native_error = error;
    result.hresult = HRESULT_FROM_WIN32(error);
    result.operation = std::move(operation);
    result.message = Win32Message(error);
    return result;
}

[[nodiscard]] FileAssociationResult HResultFailure(FileAssociationError kind,
                                                   std::wstring operation,
                                                   HRESULT error) {
    FileAssociationResult result;
    result.error = kind;
    result.native_error = HRESULT_FACILITY(error) == FACILITY_WIN32
                              ? HRESULT_CODE(error)
                              : ERROR_SUCCESS;
    result.hresult = error;
    result.operation = std::move(operation);
    result.message = Win32Message(
        result.native_error ? result.native_error : static_cast<DWORD>(error));
    return result;
}

[[nodiscard]] FileAssociationResult OwnershipConflict(
    std::wstring_view path, std::wstring_view existing, std::wstring_view requested) {
    FileAssociationResult result;
    result.error = FileAssociationError::ownership_conflict;
    result.hresult = E_FAIL;
    result.registry_path = path;
    result.requested_installation = requested;
    if (!existing.empty()) result.conflicting_installations.emplace_back(existing);
    result.operation = i18n::Literal(L"文件关联由另一个安装目录管理");
    result.message = i18n::Text(L"已登记的安装：") + std::wstring(existing) +
        i18n::Text(L"\n当前安装：") + std::wstring(requested);
    return result;
}

[[nodiscard]] bool EqualInsensitive(std::wstring_view left,
                                    std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    return _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] std::wstring JoinRegistryPath(std::wstring_view left,
                                            std::wstring_view right) {
    std::wstring value(left);
    if (!value.empty() && value.back() != L'\\') value.push_back(L'\\');
    while (!right.empty() && right.front() == L'\\') right.remove_prefix(1);
    value.append(right);
    return value;
}

// Windows XP has no transactional registry API. Journal only the values and
// keys changed by this operation, and undo in reverse order on failure. A later
// external edit is never overwritten by rollback: its current value must still
// equal our last successful write. This is failure rollback, not crash recovery.
struct RegistryValue {
    DWORD type{};
    std::vector<BYTE> bytes;
    bool operator==(const RegistryValue&) const = default;
};
LONG ReadRawValue(HKEY key, const wchar_t* name, std::optional<RegistryValue>& value) {
    value.reset();
    DWORD type{}, size{};
    auto status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    if (size > 1024U * 1024U) return ERROR_FILE_TOO_LARGE;
    RegistryValue result{type, std::vector<BYTE>(size)};
    status = RegQueryValueExW(key, name, nullptr, &type, result.bytes.data(), &size);
    if (status != ERROR_SUCCESS) return status;
    result.type = type; result.bytes.resize(size); value = std::move(result);
    return ERROR_SUCCESS;
}
struct RegistryKeyValues {
    std::map<std::wstring, RegistryValue> values;
    std::vector<BYTE> security;
};
LONG ReadKeyValues(HKEY key, RegistryKeyValues& output) {
    for (DWORD index = 0;; ++index) {
        wchar_t name[16384]{}; DWORD size = static_cast<DWORD>(std::size(name));
        auto status = RegEnumValueW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return status;
        std::optional<RegistryValue> value;
        status = ReadRawValue(key, name, value);
        if (status != ERROR_SUCCESS) return status;
        if (value) output.values.emplace(name, std::move(*value));
    }
    DWORD size{};
    auto status = RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &size);
    if (status != ERROR_INSUFFICIENT_BUFFER) return status;
    output.security.resize(size);
    return RegGetKeySecurity(key, DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(output.security.data()), &size);
}
class RegistryJournal;
thread_local RegistryJournal* active_journal{};
class RegistryJournal {
    struct ValueUndo {
        std::wstring path, name;
        std::optional<RegistryValue> previous, installed;
    };
    struct CreatedKey { std::wstring path; };
    struct DeletedKey { std::wstring path; RegistryKeyValues previous; };
    std::vector<std::variant<ValueUndo, CreatedKey, DeletedKey>> undo_;
    std::map<HKEY, std::wstring> handles_;
    HANDLE mutex_{};
    bool locked_{}, finished_{};
public:
    FileAssociationResult initial_result;
    RegistryJournal() {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\TTPlayerRebuild.Association.Transaction");
        if (!mutex_) { initial_result = Win32Failure(FileAssociationError::registry, L"CreateMutexW", GetLastError()); return; }
        const auto wait = WaitForSingleObject(mutex_, 5000);
        locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!locked_) { initial_result = Win32Failure(FileAssociationError::registry,
            i18n::Literal(L"等待另一项文件关联操作"), wait == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError()); return; }
        active_journal = this;
    }
    ~RegistryJournal() {
        if (!finished_ && locked_) static_cast<void>(Rollback());
        if (active_journal == this) active_journal = nullptr;
        if (locked_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    void Track(HKEY key, std::wstring_view path) { if (key) handles_[key] = path; }
    static std::vector<std::wstring> MissingParents(std::wstring_view path) {
        std::vector<std::wstring> result;
        for (size_t end = path.find(L'\\');; end = path.find(L'\\', end + 1)) {
            const auto current = std::wstring(path.substr(0, end));
            HKEY key{};
            const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, current.c_str(), 0, KEY_READ, &key);
            if (key) RegCloseKey(key);
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) result.push_back(current);
            if (end == std::wstring_view::npos) break;
        }
        return result;
    }
    void RecordCreated(const std::vector<std::wstring>& paths) {
        for (const auto& path : paths) {
            HKEY key{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
                RegCloseKey(key); undo_.push_back(CreatedKey{path});
            }
        }
    }
    LONG Write(HKEY key, const wchar_t* name, const std::optional<RegistryValue>& installed) {
        const auto found = handles_.find(key);
        if (found == handles_.end()) return ERROR_INVALID_HANDLE;
        std::optional<RegistryValue> previous;
        auto status = ReadRawValue(key, name, previous);
        if (status != ERROR_SUCCESS || previous == installed) return status;
        // Allocate the undo before mutating, so an allocation failure cannot
        // strand a successful write without its recovery data.
        undo_.push_back(ValueUndo{found->second, name ? name : L"", previous, installed});
        status = installed ? RegSetValueExW(key, name, 0, installed->type,
            installed->bytes.data(), static_cast<DWORD>(installed->bytes.size())) : RegDeleteValueW(key, name);
        if (status != ERROR_SUCCESS) undo_.pop_back();
        return status;
    }
    LONG DeleteKey(std::wstring_view path) {
        HKEY raw{};
        auto status = RegOpenKeyExW(HKEY_CURRENT_USER, std::wstring(path).c_str(), 0, KEY_READ, &raw);
        if (status != ERROR_SUCCESS) return status;
        UniqueRegKey key(raw);
        DeletedKey entry{std::wstring(path), {}};
        status = ReadKeyValues(key.get(), entry.previous);
        if (status != ERROR_SUCCESS) return status;
        undo_.push_back(std::move(entry));
        key.reset();
        status = RegDeleteKeyW(HKEY_CURRENT_USER, std::wstring(path).c_str());
        if (status != ERROR_SUCCESS) undo_.pop_back();
        return status;
    }
    LONG DeleteTree(std::wstring_view path) {
        for (;;) {
            HKEY raw{};
            auto status = RegOpenKeyExW(HKEY_CURRENT_USER, std::wstring(path).c_str(), 0, KEY_READ, &raw);
            if (status != ERROR_SUCCESS) return status;
            UniqueRegKey key(raw);
            wchar_t name[256]{}; DWORD size = static_cast<DWORD>(std::size(name));
            status = RegEnumKeyExW(key.get(), 0, name, &size, nullptr, nullptr, nullptr, nullptr);
            key.reset();
            if (status == ERROR_NO_MORE_ITEMS) return DeleteKey(path);
            if (status != ERROR_SUCCESS) return status;
            status = DeleteTree(JoinRegistryPath(path, name));
            if (status != ERROR_SUCCESS) return status;
        }
    }
    LONG Rollback() {
        LONG first_error = ERROR_SUCCESS;
        for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
            const auto status = std::visit([&](const auto& entry) -> LONG {
                using T = std::decay_t<decltype(entry)>;
                HKEY raw{};
                if constexpr (std::is_same_v<T, DeletedKey>) {
                    DWORD disposition{};
                    auto error = RegCreateKeyExW(HKEY_CURRENT_USER, entry.path.c_str(), 0, nullptr, 0,
                        KEY_READ | KEY_WRITE | WRITE_DAC, nullptr, &raw, &disposition);
                    if (error != ERROR_SUCCESS) return error;
                    UniqueRegKey key(raw);
                    if (disposition != REG_CREATED_NEW_KEY) return ERROR_REVISION_MISMATCH;
                    for (const auto& [name, value] : entry.previous.values) {
                        error = RegSetValueExW(key.get(), name.c_str(), 0, value.type,
                            value.bytes.data(), static_cast<DWORD>(value.bytes.size()));
                        if (error != ERROR_SUCCESS) return error;
                    }
                    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
                    const auto security = reinterpret_cast<PSECURITY_DESCRIPTOR>(
                        const_cast<BYTE*>(entry.previous.security.data()));
                    if (!GetSecurityDescriptorControl(security, &control, &revision)) return GetLastError();
                    return RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION |
                        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), security);
                } else {
                    auto error = RegOpenKeyExW(HKEY_CURRENT_USER, entry.path.c_str(), 0,
                        KEY_READ | KEY_SET_VALUE, &raw);
                    if (error != ERROR_SUCCESS) return error;
                    UniqueRegKey key(raw);
                    if constexpr (std::is_same_v<T, CreatedKey>) {
                        DWORD children{}, values{};
                        error = RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &children,
                            nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
                        key.reset();
                        if (error != ERROR_SUCCESS) return error;
                        if (children || values) return ERROR_REVISION_MISMATCH;
                        return RegDeleteKeyW(HKEY_CURRENT_USER, entry.path.c_str());
                    } else {
                        std::optional<RegistryValue> current;
                        error = ReadRawValue(key.get(), entry.name.c_str(), current);
                        if (error != ERROR_SUCCESS) return error;
                        if (current != entry.installed) return ERROR_REVISION_MISMATCH;
                        return entry.previous ? RegSetValueExW(key.get(), entry.name.c_str(), 0,
                            entry.previous->type, entry.previous->bytes.data(), static_cast<DWORD>(entry.previous->bytes.size())) :
                            RegDeleteValueW(key.get(), entry.name.c_str());
                    }
                }
            }, *it);
            if (status != ERROR_SUCCESS && first_error == ERROR_SUCCESS) first_error = status;
        }
        undo_.clear(); finished_ = true;
        return first_error;
    }
    FileAssociationResult Finish(FileAssociationResult result, bool notify) {
        if (result) { result.changed = !undo_.empty(); undo_.clear(); finished_ = true; }
        else {
            const auto error = Rollback();
            result.changed = error != ERROR_SUCCESS;
            result.rollback_failed = result.changed;
            if (error != ERROR_SUCCESS) result.message += i18n::Text(L"\n部分关联恢复失败，请重试。恢复错误：") + Win32Message(error);
        }
        if (active_journal == this) active_journal = nullptr;
        if (result.changed && notify) FileAssociationBackend::NotifyShellAssociationsChanged();
        return result;
    }
};
LONG SetRegistryValue(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size) {
    if (!active_journal) return RegSetValueExW(key, name, 0, type, data, size);
    RegistryValue value{type, {}};
    if (size) value.bytes.assign(data, data + size);
    return active_journal->Write(key, name, value);
}
LONG DeleteRegistryValue(HKEY key, const wchar_t* name) {
    return active_journal ? active_journal->Write(key, name, std::nullopt) : RegDeleteValueW(key, name);
}
LONG DeleteRegistryKey(std::wstring_view path) {
    return active_journal ? active_journal->DeleteKey(path) : RegDeleteKeyW(HKEY_CURRENT_USER, std::wstring(path).c_str());
}
LONG DeleteRegistryTree(std::wstring_view path) {
    return active_journal ? active_journal->DeleteTree(path) : RegDeleteTreeW(HKEY_CURRENT_USER, std::wstring(path).c_str());
}

[[nodiscard]] LONG OpenKey(std::wstring_view path, REGSAM access,
                           UniqueRegKey& key) {
    HKEY raw = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER,
                                      std::wstring(path).c_str(), 0, access,
                                      &raw);
    key.reset(raw);
    if (active_journal && status == ERROR_SUCCESS) active_journal->Track(raw, path);
    return status;
}

[[nodiscard]] LONG CreateKey(std::wstring_view path, UniqueRegKey& key,
                             bool& existed) {
    const auto missing = active_journal ? RegistryJournal::MissingParents(path) : std::vector<std::wstring>{};
    HKEY raw = nullptr;
    DWORD disposition = 0;
    const LONG status = RegCreateKeyExW(
        HKEY_CURRENT_USER, std::wstring(path).c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE |
                                    KEY_ENUMERATE_SUB_KEYS,
        nullptr, &raw, &disposition);
    key.reset(raw);
    existed = disposition == REG_OPENED_EXISTING_KEY;
    if (active_journal) {
        active_journal->RecordCreated(missing);
        if (status == ERROR_SUCCESS) active_journal->Track(raw, path);
    }
    return status;
}

[[nodiscard]] LONG ReadStringValue(HKEY key, const wchar_t* name,
                                   std::optional<std::wstring>& value) {
    value.reset();
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return status;
    if (status != ERROR_SUCCESS) return status;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return ERROR_DATATYPE_MISMATCH;
    if (bytes > 1024U * 1024U) return ERROR_FILE_TOO_LARGE;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1U, L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS) return status;
    buffer.back() = L'\0';
    value.emplace(buffer.data());
    return ERROR_SUCCESS;
}

[[nodiscard]] LONG ReadDwordValue(HKEY key, const wchar_t* name,
                                  std::optional<DWORD>& value) {
    value.reset();
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD data = 0;
    const LONG status = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return status;
    if (status != ERROR_SUCCESS) return status;
    if (type != REG_DWORD || bytes != sizeof(DWORD))
        return ERROR_DATATYPE_MISMATCH;
    value = data;
    return ERROR_SUCCESS;
}

[[nodiscard]] LONG WriteStringValue(HKEY key, const wchar_t* name,
                                    std::wstring_view value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    return SetRegistryValue(key, name, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.data()), bytes);
}

[[nodiscard]] LONG WriteDwordValue(HKEY key, const wchar_t* name, DWORD value) {
    return SetRegistryValue(key, name, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

[[nodiscard]] LONG DeleteValueIfPresent(HKEY key,
                                        const wchar_t* name) noexcept {
    const LONG status = DeleteRegistryValue(key, name);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

[[nodiscard]] std::wstring NormalizeExtension(std::wstring_view extension) {
    while (!extension.empty() &&
           (extension.front() == L' ' || extension.front() == L'\t')) {
        extension.remove_prefix(1);
    }
    while (!extension.empty() &&
           (extension.back() == L' ' || extension.back() == L'\t')) {
        extension.remove_suffix(1);
    }
    if (extension.size() >= 2 && extension[0] == L'*' && extension[1] == L'.')
        extension.remove_prefix(2);
    else if (!extension.empty() && extension.front() == L'.')
        extension.remove_prefix(1);

    if (extension.empty() || extension.size() > 64) return {};
    std::wstring normalized;
    normalized.reserve(extension.size());
    for (const wchar_t character : extension) {
        if (std::iswspace(character) || character == L'.' ||
            character == L'*' || character == L'?' || character == L'\\' ||
            character == L'/' || character == L':' || character == L';' ||
            character == L'"' || character == L'<' || character == L'>' ||
            character == L'|') {
            return {};
        }
        normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return normalized;
}

[[nodiscard]] std::wstring Quote(std::wstring_view value) {
    std::wstring quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back(L'"');
    quoted.append(value);
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::wstring OpenCommand(const std::filesystem::path& executable,
                                       bool append) {
    std::wstring command = Quote(executable.wstring());
    if (append) command += L" /a";
    command += L" \"%1\"";
    return command;
}

[[nodiscard]] std::wstring DefaultIcon(
    const std::filesystem::path& executable) {
    return Quote(executable.wstring()) + L",0";
}

struct ManagedValueNames {
    std::wstring owner{kOwnerValue}, installed{kInstalledValue}, backup{kBackupValue};
    std::wstring backup_present{kBackupPresentValue}, key_existed{kKeyExistedValue};
    std::wstring installed_present{L"TTPlayer.Rebuild.InstalledPresent"};
    explicit ManagedValueNames(const wchar_t* value_name) {
        if (!value_name) return;
        for (auto* name : {&owner, &installed, &backup, &backup_present, &key_existed, &installed_present})
            *name += L"." + std::wstring(value_name);
    }
};

[[nodiscard]] ManagedChange InstallManagedValue(
    std::wstring_view path, const wchar_t* value_name, const std::optional<std::wstring>& value,
    std::wstring_view owner_identity) {
    const ManagedValueNames names(value_name);
    UniqueRegKey key;
    bool key_existed = false;
    LONG status = CreateKey(path, key, key_existed);
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             L"RegCreateKeyExW(" + std::wstring(path) + L")",
                             status), false};
    }

    std::optional<std::wstring> owner;
    status = ReadStringValue(key.get(), names.owner.c_str(), owner);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             i18n::Literal(L"read association owner"), status), false};
    }
    if (owner && !EqualInsensitive(*owner, owner_identity)) {
        return {OwnershipConflict(path, *owner, owner_identity), false};
    }

    std::optional<std::wstring> previous;
    status = ReadStringValue(key.get(), value_name, previous);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             i18n::Literal(L"read registry default value"), status), false};
    }

    if (!owner) {
        if (previous) {
            status = WriteStringValue(key.get(), names.backup.c_str(), *previous);
            if (status != ERROR_SUCCESS)
                return {Win32Failure(FileAssociationError::registry,
                                     i18n::Literal(L"save registry default backup"), status),
                        false};
        } else {
            status = DeleteValueIfPresent(key.get(), names.backup.c_str());
            if (status != ERROR_SUCCESS)
                return {Win32Failure(FileAssociationError::registry,
                                     i18n::Literal(L"clear stale registry backup"), status),
                        false};
        }
        status = WriteDwordValue(key.get(), names.backup_present.c_str(),
                                 previous ? 1U : 0U);
        if (status == ERROR_SUCCESS)
            status = WriteDwordValue(key.get(), names.key_existed.c_str(),
                                     key_existed ? 1U : 0U);
        if (status == ERROR_SUCCESS)
            status = WriteStringValue(key.get(), names.owner.c_str(), owner_identity);
        if (status != ERROR_SUCCESS)
            return {Win32Failure(FileAssociationError::registry,
                                 i18n::Literal(L"save association ownership metadata"), status),
                    false};
    }

    status = value ? WriteStringValue(key.get(), names.installed.c_str(), *value)
                   : DeleteValueIfPresent(key.get(), names.installed.c_str());
    if (status == ERROR_SUCCESS)
        status = WriteDwordValue(key.get(), names.installed_present.c_str(), value ? 1U : 0U);
    if (status == ERROR_SUCCESS)
        status = value ? WriteStringValue(key.get(), value_name, *value)
                       : DeleteValueIfPresent(key.get(), value_name);
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             i18n::Literal(L"write registry default value"), status), false};
    }
    return {Success(previous != value), previous != value};
}

[[nodiscard]] ManagedChange RestoreManagedValue(
    std::wstring_view path, const wchar_t* value_name, std::wstring_view owner_identity,
    bool restore_previous = true) {
    const ManagedValueNames names(value_name);
    UniqueRegKey key;
    LONG status = OpenKey(path, KEY_QUERY_VALUE | KEY_SET_VALUE |
                                    KEY_ENUMERATE_SUB_KEYS,
                          key);
    if (status == ERROR_FILE_NOT_FOUND) return {Success(), false};
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             L"RegOpenKeyExW(" + std::wstring(path) + L")",
                             status), false};
    }

    std::optional<std::wstring> owner;
    status = ReadStringValue(key.get(), names.owner.c_str(), owner);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             i18n::Literal(L"read association owner"), status), false};
    }
    if (status == ERROR_FILE_NOT_FOUND || !owner ||
        !EqualInsensitive(*owner, owner_identity)) {
        return {Success(), false};
    }

    std::optional<std::wstring> current;
    std::optional<std::wstring> installed;
    std::optional<std::wstring> backup;
    std::optional<DWORD> backup_present;
    std::optional<DWORD> key_existed;
    std::optional<DWORD> installed_present;
    const auto read_optional_string = [&](const wchar_t* name,
                                          std::optional<std::wstring>& target) {
        const LONG result = ReadStringValue(key.get(), name, target);
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    };
    status = read_optional_string(value_name, current);
    if (status == ERROR_SUCCESS)
        status = read_optional_string(names.installed.c_str(), installed);
    if (status == ERROR_SUCCESS)
        status = read_optional_string(names.backup.c_str(), backup);
    LONG value_status = ReadDwordValue(key.get(), names.backup_present.c_str(),
                                       backup_present);
    if (value_status != ERROR_SUCCESS && value_status != ERROR_FILE_NOT_FOUND)
        status = value_status;
    value_status = ReadDwordValue(key.get(), names.key_existed.c_str(), key_existed);
    if (value_status != ERROR_SUCCESS && value_status != ERROR_FILE_NOT_FOUND)
        status = value_status;
    value_status = ReadDwordValue(key.get(), names.installed_present.c_str(), installed_present);
    if (value_status != ERROR_SUCCESS && value_status != ERROR_FILE_NOT_FOUND)
        status = value_status;
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             i18n::Literal(L"read association backup metadata"), status),
                false};
    }

    bool changed = false;
    // Do not overwrite a default which the user or Windows changed after this
    // backend installed it (notably a protected UserChoice transition).
    // Older builds did not write InstalledPresent. They always installed a
    // string. New named-value transactions can also own a deliberate deletion.
    if (restore_previous &&
        installed_present.value_or(installed ? 1U : MAXDWORD) == (installed ? 1U : 0U) &&
        current == installed) {
        if (backup_present.value_or(0U) != 0U && backup) {
            status = WriteStringValue(key.get(), value_name, *backup);
        } else {
            status = DeleteRegistryValue(key.get(), value_name);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
        if (status != ERROR_SUCCESS) {
            return {Win32Failure(FileAssociationError::registry,
                                 i18n::Literal(L"restore registry default value"), status),
                    false};
        }
        changed = current != (backup_present.value_or(0U) != 0U ? backup : std::nullopt);
    }

    for (const wchar_t* name : {names.owner.c_str(), names.installed.c_str(), names.backup.c_str(),
                                names.backup_present.c_str(), names.key_existed.c_str(), names.installed_present.c_str()}) {
        status = DeleteValueIfPresent(key.get(), name);
        if (status != ERROR_SUCCESS) {
            auto failure = Win32Failure(FileAssociationError::registry,
                                        i18n::Literal(L"remove association metadata"),
                                        status);
            failure.changed = changed;
            return {std::move(failure), changed};
        }
    }

    DWORD subkeys = 0;
    DWORD values = 0;
    status = RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys,
                              nullptr, nullptr, &values, nullptr, nullptr,
                              nullptr, nullptr);
    const bool delete_empty = status == ERROR_SUCCESS && subkeys == 0 &&
                              values == 0 && key_existed.value_or(1U) == 0U;
    key.reset();
    if (delete_empty) {
        status = DeleteRegistryKey(path);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            return {Win32Failure(FileAssociationError::registry,
                                 i18n::Literal(L"remove empty association key"), status),
                    changed};
        }
        changed = true;
    }
    return {Success(changed), changed};
}

[[nodiscard]] ManagedChange InstallManagedDefault(
    std::wstring_view path, std::wstring_view value, std::wstring_view owner) {
    return InstallManagedValue(path, nullptr, std::wstring(value), owner);
}

[[nodiscard]] ManagedChange RestoreManagedDefault(
    std::wstring_view path, std::wstring_view owner) {
    return RestoreManagedValue(path, nullptr, owner);
}

// FUN_0049bb64 deletes the Vista/Win7 UserChoice KEY. Deleting its Progid
// value instead requires KEY_SET_VALUE, which the shell can deny. Keep our
// backup outside UserChoice so neither registration nor removal needs that
// permission on an existing choice. Never call this on Windows 8 or newer.
[[nodiscard]] ManagedChange ManageLegacyUserChoice(
    const std::wstring& path, const std::wstring& backup_path,
    std::wstring_view owner_identity, bool enabled, bool restore_previous) {
    const auto failure = [](std::wstring operation, LONG error, bool changed = false) {
        auto result = Win32Failure(FileAssociationError::registry, std::move(operation), error);
        result.changed = changed;
        return ManagedChange{std::move(result), changed};
    };
    UniqueRegKey choice;
    LONG status = OpenKey(path, KEY_QUERY_VALUE, choice);
    const bool choice_exists = status == ERROR_SUCCESS;
    if (!choice_exists && status != ERROR_FILE_NOT_FOUND)
        return failure(i18n::Literal(L"read legacy UserChoice"), status);
    std::optional<std::wstring> current;
    if (choice_exists) {
        status = ReadStringValue(choice.get(), L"Progid", current);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return failure(i18n::Literal(L"read legacy UserChoice Progid"), status);
    }
    choice.reset();

    UniqueRegKey backup;
    status = OpenKey(backup_path, KEY_QUERY_VALUE | KEY_SET_VALUE, backup);
    if (status == ERROR_FILE_NOT_FOUND) {
        if (!enabled || !choice_exists) return {Success(), false};
        bool existed{};
        status = CreateKey(backup_path, backup, existed);
    }
    if (status != ERROR_SUCCESS) return failure(i18n::Literal(L"open legacy UserChoice backup"), status);
    std::optional<std::wstring> owner;
    status = ReadStringValue(backup.get(), kOwnerValue, owner);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return failure(i18n::Literal(L"read legacy UserChoice backup owner"), status);
    if (owner && !EqualInsensitive(*owner, owner_identity)) {
        if (!enabled) return {Success(), false};
        return {OwnershipConflict(backup_path, *owner, owner_identity), false};
    }
    if (!owner) {
        if (!enabled) return {Success(), false};
        status = current ? WriteStringValue(backup.get(), kBackupValue, *current)
                         : DeleteValueIfPresent(backup.get(), kBackupValue);
        if (status == ERROR_SUCCESS)
            status = WriteStringValue(backup.get(), kOwnerValue, owner_identity);
        if (status != ERROR_SUCCESS) return failure(i18n::Literal(L"save legacy UserChoice backup"), status);
    }
    if (enabled) {
        if (!choice_exists) return {Success(), false};
        status = DeleteRegistryKey(path);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return failure(i18n::Literal(L"remove legacy UserChoice key"), status);
        return {Success(true), true};
    }

    bool changed = false;
    // An existing key is a later selection made in Windows: leave it alone.
    if (!choice_exists && restore_previous) {
        std::optional<std::wstring> previous;
        status = ReadStringValue(backup.get(), kBackupValue, previous);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return failure(i18n::Literal(L"read legacy UserChoice backup"), status);
        if (previous) {
            bool existed{};
            status = CreateKey(path, choice, existed);
            if (status == ERROR_SUCCESS && !existed) {
                status = WriteStringValue(choice.get(), L"Progid", *previous);
                changed = status == ERROR_SUCCESS;
                if (status != ERROR_SUCCESS) {
                    choice.reset();
                    // Keep the backup retryable if creating the value failed.
                    DeleteRegistryKey(path);
                }
            }
            if (status != ERROR_SUCCESS) return failure(i18n::Literal(L"restore legacy UserChoice"), status);
        }
    }
    for (const auto* name : {kOwnerValue, kBackupValue}) {
        status = DeleteValueIfPresent(backup.get(), name);
        if (status != ERROR_SUCCESS) return failure(i18n::Literal(L"clear legacy UserChoice backup"), status, changed);
    }
    DWORD subkeys{}, values{};
    status = RegQueryInfoKeyW(backup.get(), nullptr, nullptr, nullptr, &subkeys,
        nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    const bool empty = status == ERROR_SUCCESS && subkeys == 0 && values == 0;
    backup.reset();
    if (empty) {
        status = DeleteRegistryKey(backup_path);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return failure(i18n::Literal(L"remove legacy UserChoice backup"), status, changed);
    }
    return {Success(changed), changed};
}

[[nodiscard]] FileAssociationResult ReadDefaultAt(std::wstring_view path,
                                                  std::wstring& value,
                                                  bool& present) {
    value.clear();
    present = false;
    UniqueRegKey key;
    LONG status = OpenKey(path, KEY_QUERY_VALUE, key);
    if (status == ERROR_FILE_NOT_FOUND) return Success();
    if (status != ERROR_SUCCESS)
        return Win32Failure(FileAssociationError::registry,
                            L"RegOpenKeyExW(" + std::wstring(path) + L")",
                            status);
    std::optional<std::wstring> read;
    status = ReadStringValue(key.get(), nullptr, read);
    if (status == ERROR_FILE_NOT_FOUND) return Success();
    if (status != ERROR_SUCCESS)
        return Win32Failure(FileAssociationError::registry,
                            i18n::Literal(L"read registry default value"), status);
    value = read.value_or(std::wstring{});
    present = true;
    return Success();
}

[[nodiscard]] std::wstring ManagedProgId(
    const FileAssociationBackendOptions& options,
    std::wstring_view extension) {
    std::wstring id = options.prog_id_prefix.empty() ? L"Audio"
                                                     : options.prog_id_prefix;
    id.push_back(L'.');
    id.append(extension);
    return id;
}

[[nodiscard]] bool IsRealClassesStore(std::wstring_view value) noexcept {
    return EqualInsensitive(value, L"Software\\Classes");
}

OSVERSIONINFOW AssociationWindowsVersion() {
    OSVERSIONINFOW info{sizeof(info)};
    using GetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto get_version = reinterpret_cast<GetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!get_version || get_version(&info) != 0) info.dwMajorVersion = 10;
    return info; // Failure must never enable the obsolete default-setting API.
}

FileAssociationMode AssociationMode(const FileAssociationBackendOptions& options) {
    if (!IsRealClassesStore(options.current_user_classes_subkey)) return options.isolated_mode;
    const auto version = AssociationWindowsVersion();
    return SelectFileAssociationMode(version.dwMajorVersion, version.dwMinorVersion);
}

std::wstring ExplorerExtensionPath(const FileAssociationBackendOptions& options,
                                   std::wstring_view extension) {
    return JoinRegistryPath(IsRealClassesStore(options.current_user_classes_subkey)
        ? L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts"
        : JoinRegistryPath(options.current_user_classes_subkey, L"ExplorerFileExts"),
        L"." + std::wstring(extension));
}

std::wstring CapabilitiesPath(const FileAssociationBackendOptions& options) {
    return IsRealClassesStore(options.current_user_classes_subkey)
        ? L"Software\\TTPlayerRebuild\\Capabilities"
        : JoinRegistryPath(options.current_user_classes_subkey, L"Registration\\Capabilities");
}

std::wstring RegisteredApplicationsPath(const FileAssociationBackendOptions& options) {
    return IsRealClassesStore(options.current_user_classes_subkey)
        ? L"Software\\RegisteredApplications"
        : JoinRegistryPath(options.current_user_classes_subkey, L"Registration\\RegisteredApplications");
}

FileAssociationResult SetRegistrationString(std::wstring_view path,
                                           const wchar_t* name, std::wstring_view value) {
    UniqueRegKey key;
    bool existed{};
    LONG status = CreateKey(path, key, existed);
    if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry,
        i18n::Literal(L"create default-program registration"), status);
    std::optional<std::wstring> previous;
    status = ReadStringValue(key.get(), name, previous);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read default-program registration"), status);
    if (previous && *previous == value) return Success();
    status = WriteStringValue(key.get(), name, value);
    return status == ERROR_SUCCESS ? Success(true) : Win32Failure(
        FileAssociationError::registry, i18n::Literal(L"write default-program registration"), status);
}

FileAssociationResult SetOpenWith(const FileAssociationBackendOptions& options,
                                   std::wstring_view extension, const std::wstring& prog_id, bool add) {
    const auto path = JoinRegistryPath(options.current_user_classes_subkey,
        L"." + std::wstring(extension) + L"\\OpenWithProgids");
    UniqueRegKey key;
    bool existed{};
    LONG status = add ? CreateKey(path, key, existed) : OpenKey(path, KEY_QUERY_VALUE | KEY_SET_VALUE, key);
    if (!add && status == ERROR_FILE_NOT_FOUND) return Success();
    if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, i18n::Literal(L"open OpenWithProgids"), status);
    DWORD type{}, bytes{};
    status = RegQueryValueExW(key.get(), prog_id.c_str(), nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"query OpenWithProgids"), status);
    if ((add && status == ERROR_SUCCESS) || (!add && status == ERROR_FILE_NOT_FOUND)) return Success();
    status = add ? SetRegistryValue(key.get(), prog_id.c_str(), REG_NONE, nullptr, 0)
                 : DeleteRegistryValue(key.get(), prog_id.c_str());
    // Never delete the extension key or other applications' named entries.
    return status == ERROR_SUCCESS ? Success(true) : Win32Failure(
        FileAssociationError::registry, i18n::Literal(L"update OpenWithProgids"), status);
}

struct AssociationScope { std::wstring path; bool recursive{}, owners{true}, writable{true}; };
struct AssociationOwner { std::wstring path, name, executable; };
void AddScope(std::vector<AssociationScope>& scopes, std::wstring path,
              bool recursive = false, bool owners = true, bool writable = true) {
    const auto found = std::find_if(scopes.begin(), scopes.end(), [&](const auto& item) {
        return EqualInsensitive(item.path, path);
    });
    if (found == scopes.end()) scopes.push_back({std::move(path), recursive, owners, writable});
    else { found->recursive |= recursive; found->owners |= owners; found->writable |= writable; }
}
void ExtensionScopes(std::vector<AssociationScope>& scopes,
                     const FileAssociationBackendOptions& options, std::wstring_view extension) {
    AddScope(scopes, JoinRegistryPath(options.current_user_classes_subkey, ManagedProgId(options, extension)), true);
    const auto ext = JoinRegistryPath(options.current_user_classes_subkey, L"." + std::wstring(extension));
    AddScope(scopes, ext);
    AddScope(scopes, ext + L"\\OpenWithProgids", false, false);
    if (AssociationMode(options) != FileAssociationMode::user_choice) {
        AddScope(scopes, ExplorerExtensionPath(options, extension));
        if (AssociationMode(options) == FileAssociationMode::vista_windows7)
            AddScope(scopes, ExplorerExtensionPath(options, extension) + L"\\UserChoice", false, false, false);
    }
}
void ApplicationScopes(std::vector<AssociationScope>& scopes, const FileAssociationBackendOptions& options) {
    AddScope(scopes, CapabilitiesPath(options), true);
    AddScope(scopes, RegisteredApplicationsPath(options), false, false);
}
void ShellScopes(std::vector<AssociationScope>& scopes, const FileAssociationBackendOptions& options,
                 ShellIntegrationTarget target) {
    const auto shell = JoinRegistryPath(options.current_user_classes_subkey,
        target == ShellIntegrationTarget::audio_cd ? L"AudioCD\\shell" : L"Directory\\shell");
    if (target == ShellIntegrationTarget::audio_cd) AddScope(scopes, shell);
    const auto verb = shell + (target == ShellIntegrationTarget::audio_cd ? L"\\open" : L"\\Playback");
    AddScope(scopes, verb); AddScope(scopes, verb + L"\\command");
    AddScope(scopes, shell + L"\\PlayList"); AddScope(scopes, shell + L"\\PlayList\\command");
}
FileAssociationResult DiscoverManagedScopes(std::vector<AssociationScope>& scopes,
                                           const FileAssociationBackendOptions& options) {
    ApplicationScopes(scopes, options);
    UniqueRegKey classes;
    auto status = OpenKey(options.current_user_classes_subkey, KEY_READ, classes);
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_SUCCESS)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"读取现有文件关联"), status);
    if (classes) for (DWORD index = 0;; ++index) {
        wchar_t name[256]{}; DWORD size = static_cast<DWORD>(std::size(name));
        status = RegEnumKeyExW(classes.get(), index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, i18n::Literal(L"枚举现有文件关联"), status);
        const std::wstring candidate(name), prefix = ManagedProgId(options, {});
        const bool current = candidate.size() > prefix.size() && EqualInsensitive(candidate.substr(0, prefix.size()), prefix);
        const bool legacy = candidate.size() > 6 && EqualInsensitive(candidate.substr(0, 6), L"Audio.");
        if (!current && !legacy) continue;
        if (legacy && !current) {
            // Audio.* can belong to the original player. Only the rebuild's
            // explicit ownership marker permits migrating such an old record.
            UniqueRegKey key;
            if (OpenKey(JoinRegistryPath(options.current_user_classes_subkey, candidate), KEY_QUERY_VALUE, key) != ERROR_SUCCESS) continue;
            std::optional<std::wstring> owner;
            if (ReadStringValue(key.get(), kOwnerValue, owner) != ERROR_SUCCESS || !owner) continue;
        }
        const auto extension = NormalizeExtension(candidate.substr(current ? prefix.size() : 6));
        if (extension.empty()) continue;
        ExtensionScopes(scopes, options, extension);
        if (legacy && !current) AddScope(scopes, JoinRegistryPath(options.current_user_classes_subkey, candidate), true);
    }
    ShellScopes(scopes, options, ShellIntegrationTarget::audio_cd);
    ShellScopes(scopes, options, ShellIntegrationTarget::directory);
    return Success();
}
FileAssociationResult InspectScope(const AssociationScope& scope, std::vector<AssociationOwner>& owners,
                                  unsigned depth = 0) {
    if (depth > 32) return Win32Failure(FileAssociationError::registry, scope.path, ERROR_BAD_PATHNAME);
    UniqueRegKey key;
    auto status = OpenKey(scope.path, KEY_READ, key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        if (!scope.writable) return Success();
        auto parent = scope.path;
        for (auto slash = parent.find_last_of(L'\\'); slash != std::wstring::npos; slash = parent.find_last_of(L'\\')) {
            parent.resize(slash);
            status = OpenKey(parent, KEY_QUERY_VALUE | KEY_CREATE_SUB_KEY, key);
            if (status == ERROR_SUCCESS) return Success();
            if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, parent, status);
        }
        return Win32Failure(FileAssociationError::registry, scope.path, status);
    }
    if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, scope.path, status);
    if (scope.writable) {
        UniqueRegKey writable;
        status = OpenKey(scope.path, KEY_QUERY_VALUE | KEY_SET_VALUE, writable);
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, scope.path, status);
    }
    if (scope.owners) for (DWORD index = 0;; ++index) {
        wchar_t name[16384]{}; DWORD size = static_cast<DWORD>(std::size(name));
        status = RegEnumValueW(key.get(), index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, scope.path, status);
        const std::wstring value_name(name), prefix = std::wstring(kOwnerValue) + L".";
        if (!EqualInsensitive(value_name, kOwnerValue) &&
            !(value_name.size() > prefix.size() && EqualInsensitive(value_name.substr(0, prefix.size()), prefix))) continue;
        std::optional<std::wstring> owner;
        status = ReadStringValue(key.get(), name, owner);
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, scope.path, status);
        if (owner) owners.push_back({scope.path, value_name, *owner});
    }
    if (scope.recursive) for (DWORD index = 0;; ++index) {
        wchar_t name[256]{}; DWORD size = static_cast<DWORD>(std::size(name));
        status = RegEnumKeyExW(key.get(), index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, scope.path, status);
        const auto result = InspectScope({JoinRegistryPath(scope.path, name), true, scope.owners, scope.writable}, owners, depth + 1);
        if (!result) return result;
    }
    return Success();
}
FileAssociationResult PreflightAssociations(const std::vector<AssociationScope>& scopes,
    std::wstring_view executable, bool handover, std::vector<AssociationOwner>& owners, bool enforce_ownership = true) {
    FileAssociationResult conflict;
    for (const auto& scope : scopes) {
        auto result = InspectScope(scope, owners);
        if (!result) return result;
    }
    if (!enforce_ownership || handover) return Success();
    for (const auto& owner : owners) if (!EqualInsensitive(owner.executable, executable)) {
        if (conflict) conflict = OwnershipConflict(owner.path, owner.executable, executable);
        else if (std::none_of(conflict.conflicting_installations.begin(), conflict.conflicting_installations.end(),
            [&](const auto& previous) { return EqualInsensitive(previous, owner.executable); }))
            conflict.conflicting_installations.push_back(owner.executable);
    }
    return conflict;
}
FileAssociationResult TransferAssociationOwners(const std::vector<AssociationOwner>& owners,
    const FileAssociationBackendOptions& options, const std::filesystem::path& executable) {
    for (const auto& owner : owners) {
        if (EqualInsensitive(owner.executable, executable.wstring())) continue;
        UniqueRegKey key;
        auto status = OpenKey(owner.path, KEY_QUERY_VALUE | KEY_SET_VALUE, key);
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, owner.path, status);
        std::optional<std::wstring> verified;
        status = ReadStringValue(key.get(), owner.name.c_str(), verified);
        if (status != ERROR_SUCCESS || !verified || *verified != owner.executable)
            return Win32Failure(FileAssociationError::registry, owner.path, status == ERROR_SUCCESS ? ERROR_REVISION_MISMATCH : status);
        const bool capability = EqualInsensitive(owner.path, CapabilitiesPath(options));
        const bool choice_backup = owner.path.ends_with(L"\\LegacyUserChoiceBackup");
        if (capability) {
            std::optional<std::wstring> icon;
            status = ReadStringValue(key.get(), L"ApplicationIcon", icon);
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, owner.path, status);
            if (icon && EqualInsensitive(*icon, DefaultIcon(owner.executable)))
                status = WriteStringValue(key.get(), L"ApplicationIcon", DefaultIcon(executable));
            else status = ERROR_SUCCESS;
        } else if (!choice_backup) {
            const std::wstring suffix = owner.name.substr(std::wstring_view(kOwnerValue).size());
            const std::wstring value_name = suffix.empty() ? L"" : suffix.substr(1);
            const auto installed_name = std::wstring(kInstalledValue) + suffix;
            std::optional<std::wstring> installed, current;
            status = ReadStringValue(key.get(), installed_name.c_str(), installed);
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, owner.path, status);
            status = ReadStringValue(key.get(), value_name.c_str(), current);
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, owner.path, status);
            status = ERROR_SUCCESS;
            if (installed && current == installed) {
                std::optional<std::wstring> replacement;
                if (EqualInsensitive(*installed, OpenCommand(owner.executable, false))) replacement = OpenCommand(executable, false);
                else if (EqualInsensitive(*installed, OpenCommand(owner.executable, true))) replacement = OpenCommand(executable, true);
                else if (EqualInsensitive(*installed, DefaultIcon(owner.executable))) replacement = DefaultIcon(executable);
                if (replacement) {
                    status = WriteStringValue(key.get(), value_name.c_str(), *replacement);
                    if (status == ERROR_SUCCESS) status = WriteStringValue(key.get(), installed_name.c_str(), *replacement);
                }
            }
        }
        if (status == ERROR_SUCCESS) status = WriteStringValue(key.get(), owner.name.c_str(), executable.wstring());
        if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, owner.path, status);
        // Backup/BackupPresent/KeyExisted remain untouched: B inherits A's
        // original third-party recovery record instead of backing up A itself.
    }
    return Success();
}

[[nodiscard]] std::filesystem::path CanonicalForCompare(
    const std::filesystem::path& value) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(value, error);
    if (error) result = value.lexically_normal();
    return result;
}

[[nodiscard]] FileAssociationResult ResolveShortcutPath(
    ShortcutLocation location, const ShortcutOptions& options,
    std::wstring_view application_name, std::filesystem::path& output) {
    wchar_t folder[MAX_PATH]{};
    int csidl = CSIDL_DESKTOPDIRECTORY;
    switch (location) {
    case ShortcutLocation::desktop:
        csidl = CSIDL_DESKTOPDIRECTORY;
        break;
    case ShortcutLocation::programs:
        csidl = CSIDL_PROGRAMS;
        break;
    case ShortcutLocation::quick_launch:
        csidl = CSIDL_APPDATA;
        break;
    }
    const HRESULT status = SHGetFolderPathW(nullptr, csidl, nullptr,
                                            SHGFP_TYPE_CURRENT, folder);
    if (FAILED(status))
        return HResultFailure(FileAssociationError::shell,
                              L"SHGetFolderPathW", status);

    std::filesystem::path directory(folder);
    if (location == ShortcutLocation::quick_launch) {
        directory /= L"Microsoft";
        directory /= L"Internet Explorer";
        directory /= L"Quick Launch";
    } else if (location == ShortcutLocation::programs &&
               !options.programs_subdirectory.empty()) {
        const auto& subdirectory = options.programs_subdirectory;
        if (subdirectory.is_absolute() || subdirectory.has_root_name() ||
            subdirectory.has_root_directory()) {
            return Invalid(i18n::Literal(L"resolve Programs shortcut"),
                           i18n::Literal(L"programs_subdirectory must be relative"));
        }
        for (const auto& component : subdirectory) {
            if (component == L"..")
                return Invalid(i18n::Literal(L"resolve Programs shortcut"),
                               i18n::Literal(L"programs_subdirectory may not contain '..'"));
        }
        directory /= subdirectory;
    }

    std::wstring name = options.display_name.empty()
                            ? std::wstring(application_name)
                            : options.display_name;
    if (name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        return Invalid(i18n::Literal(L"resolve shortcut path"), i18n::Literal(L"invalid shortcut name"));
    }
    if (name.size() < 4 ||
        !EqualInsensitive(std::wstring_view(name).substr(name.size() - 4),
                          L".lnk")) {
        name += L".lnk";
    }
    output = directory / name;
    return Success();
}

class ScopedComInitialization {
public:
    ScopedComInitialization() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = result_ == S_OK || result_ == S_FALSE;
    }
    ~ScopedComInitialization() {
        if (uninitialize_) CoUninitialize();
    }
    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool uninitialize_{};
};

} // namespace

std::vector<AssociableExtension> BuildAssociableExtensions(
    const std::vector<plugins::ReaderFormat>& reader_formats) {
    static constexpr std::array<std::wstring_view, 9> excluded{
        L"avi", L"asf", L"wmv", L"rm", L"ram", L"rmvb",
        L"7z", L"zip", L"rar"};
    std::vector<AssociableExtension> result;
    for (const auto& reader : reader_formats) {
        // 0049D2FA repeatedly searches the serialized filter pattern for a
        // literal '.', takes bytes through the next ';', '*' or '.', and then
        // resumes at that delimiter. Preserve reader spelling here; the UI's
        // 00433B2E insertion step uppercases the label. Keep reader duplicates.
        const wchar_t* cursor = reader.pattern.c_str();
        while ((cursor = std::wcschr(cursor, L'.')) != nullptr) {
            ++cursor;
            if (*cursor == L'\0') break;
            const wchar_t* delimiter = std::wcspbrk(cursor, L";*.");
            const std::wstring extension = delimiter
                ? std::wstring(cursor, delimiter) : std::wstring(cursor);
            const bool blocked = std::ranges::any_of(
                excluded, [&](std::wstring_view value) {
                    return extension.size() == value.size() &&
                        _wcsnicmp(extension.c_str(), value.data(),
                                  value.size()) == 0;
                });
            if (!extension.empty() && !blocked) {
                result.push_back({extension, reader.description,
                                  reader.module_path});
            }
            if (!delimiter) break;
            cursor = delimiter;
        }
    }
    return result;
}

FileAssociationBackend::FileAssociationBackend(
    std::filesystem::path executable, std::wstring application_name,
    FileAssociationBackendOptions options)
    : executable_(std::move(executable)),
      application_name_(std::move(application_name)),
      options_(std::move(options)) {
    if (application_name_.empty()) application_name_ = L"TTPlayer";
    if (options_.current_user_classes_subkey.empty())
        options_.current_user_classes_subkey = L"Software\\Classes";
    while (!options_.current_user_classes_subkey.empty() &&
           options_.current_user_classes_subkey.back() == L'\\') {
        options_.current_user_classes_subkey.pop_back();
    }
}

FileAssociationResult FileAssociationBackend::ApplyChanges(
    const AssociationUpdate& update, const ShellVerbLabels& labels) {
    if (active_journal) return Invalid(i18n::Literal(L"文件关联提交"), i18n::Literal(L"不允许嵌套的关联批次"));
    RegistryJournal journal;
    if (!journal.initial_result) return journal.initial_result;
    std::vector<AssociationScope> scopes;
    for (const auto& format : update.register_formats) {
        const auto extension = NormalizeExtension(format.extension);
        if (extension.empty()) return journal.Finish(Invalid(i18n::Literal(L"文件关联提交"), i18n::Literal(L"invalid extension")), false);
        ExtensionScopes(scopes, options_, extension);
    }
    if (!update.register_formats.empty()) ApplicationScopes(scopes, options_);
    for (const auto& change : update.extensions) {
        const auto extension = NormalizeExtension(change.extension);
        if (extension.empty()) return journal.Finish(Invalid(i18n::Literal(L"文件关联提交"), i18n::Literal(L"invalid extension")), false);
        ExtensionScopes(scopes, options_, extension);
        if (!change.icon_only && change.enabled && AssociationMode(options_) == FileAssociationMode::user_choice)
            ApplicationScopes(scopes, options_);
    }
    for (const auto& change : update.shell) ShellScopes(scopes, options_, change.target);
    if (update.handover) {
        const auto discover = DiscoverManagedScopes(scopes, options_);
        if (!discover) return journal.Finish(discover, options_.notify_shell);
    }
    std::vector<AssociationOwner> owners;
    const bool claims_ownership = !update.register_formats.empty() ||
        std::any_of(update.extensions.begin(), update.extensions.end(), [](const auto& change) { return change.enabled || change.icon_only; }) ||
        std::any_of(update.shell.begin(), update.shell.end(), [](const auto& change) { return change.enabled; });
    auto result = PreflightAssociations(scopes, executable_.wstring(), update.handover, owners, claims_ownership);
    if (!result) {
        if (result.error == FileAssociationError::ownership_conflict) {
            // The consent covers the shared registration, so name every old
            // directory that handover would affect (including a historical
            // split Capabilities/ProgID state), not just the first failure.
            std::vector<AssociationScope> existing;
            if (DiscoverManagedScopes(existing, options_)) {
                for (auto& scope : existing) scope.writable = false;
                std::vector<AssociationOwner> all_owners;
                if (PreflightAssociations(existing, executable_.wstring(), true, all_owners))
                    for (const auto& owner : all_owners)
                        if (!EqualInsensitive(owner.executable, executable_.wstring()) &&
                            std::none_of(result.conflicting_installations.begin(), result.conflicting_installations.end(),
                                [&](const auto& previous) { return EqualInsensitive(previous, owner.executable); }))
                            result.conflicting_installations.push_back(owner.executable);
            }
        }
        return journal.Finish(result, options_.notify_shell);
    }
    if (update.handover) result = TransferAssociationOwners(owners, options_, executable_);
    auto quiet = options_; quiet.notify_shell = false;
    FileAssociationBackend backend(executable_, application_name_, quiet);
    if (result && !update.register_formats.empty()) result = backend.RegisterApplicationImpl(update.register_formats);
    bool requires_choice = false;
    for (const auto& change : update.extensions) {
        if (!result) break;
        if (change.icon_only) result = backend.SetExtensionIconImpl(change.extension, change.icon);
        else result = backend.SetExtensionAssociationImpl(change.extension, change.enabled,
            change.description, change.icon, labels);
        requires_choice |= result.requires_user_choice;
    }
    for (const auto& change : update.shell) {
        if (!result) break;
        result = backend.SetShellIntegrationImpl(change.target, change.enabled, labels);
    }
    result.requires_user_choice |= requires_choice;
    return journal.Finish(result, options_.notify_shell);
}
FileAssociationResult FileAssociationBackend::SetExtensionAssociation(
    std::wstring_view extension, bool enabled, std::wstring_view description,
    std::wstring_view icon, const ShellVerbLabels& labels) {
    AssociationUpdate update;
    update.extensions.push_back({std::wstring(extension), enabled, std::wstring(description), std::wstring(icon), false});
    return ApplyChanges(update, labels);
}
FileAssociationResult FileAssociationBackend::RegisterApplication(const std::vector<AssociableExtension>& formats) {
    AssociationUpdate update; update.register_formats = formats;
    return ApplyChanges(update);
}
FileAssociationResult FileAssociationBackend::SetExtensionIcon(std::wstring_view extension, std::wstring_view icon) {
    AssociationUpdate update;
    update.extensions.push_back({std::wstring(extension), true, {}, std::wstring(icon), true});
    return ApplyChanges(update);
}
FileAssociationResult FileAssociationBackend::SetShellIntegration(
    ShellIntegrationTarget target, bool enabled, const ShellVerbLabels& labels) {
    AssociationUpdate update; update.shell.push_back({target, enabled});
    return ApplyChanges(update, labels);
}
FileAssociationResult FileAssociationBackend::UnregisterApplication(const std::vector<AssociableExtension>& formats) {
    return UnregisterAssociations(formats, false);
}
FileAssociationResult FileAssociationBackend::UnregisterAllAssociations(const std::vector<AssociableExtension>& formats) {
    return UnregisterAssociations(formats, true);
}
FileAssociationResult FileAssociationBackend::UnregisterAssociations(
    const std::vector<AssociableExtension>& formats, bool include_shell) {
    if (active_journal) return Invalid(i18n::Literal(L"取消文件关联"), i18n::Literal(L"不允许嵌套的关联批次"));
    RegistryJournal journal;
    if (!journal.initial_result) return journal.initial_result;
    // An old installer/uninstaller must leave the successor's complete shared
    // registration alone, including OpenWithProgids (which has no owner value).
    UniqueRegKey key;
    auto status = OpenKey(CapabilitiesPath(options_), KEY_QUERY_VALUE, key);
    if (status == ERROR_SUCCESS) {
        std::optional<std::wstring> owner;
        status = ReadStringValue(key.get(), kOwnerValue, owner);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return journal.Finish(Win32Failure(FileAssociationError::registry, CapabilitiesPath(options_), status), options_.notify_shell);
        if (owner && !EqualInsensitive(*owner, executable_.wstring())) return journal.Finish(Success(), false);
    } else if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
        return journal.Finish(Win32Failure(FileAssociationError::registry, CapabilitiesPath(options_), status), options_.notify_shell);
    std::vector<AssociationScope> scopes;
    auto result = DiscoverManagedScopes(scopes, options_);
    if (!result) return journal.Finish(result, options_.notify_shell);
    for (const auto& format : formats) {
        const auto extension = NormalizeExtension(format.extension);
        if (extension.empty()) return journal.Finish(Invalid(i18n::Literal(L"取消文件关联"), i18n::Literal(L"invalid extension")), false);
        ExtensionScopes(scopes, options_, extension);
        auto legacy = options_; legacy.prog_id_prefix = L"Audio";
        AddScope(scopes, JoinRegistryPath(options_.current_user_classes_subkey, ManagedProgId(legacy, extension)), true);
    }
    std::vector<AssociationOwner> owners;
    result = PreflightAssociations(scopes, executable_.wstring(), false, owners, false);
    if (!result) return journal.Finish(result, options_.notify_shell);
    auto quiet = options_; quiet.notify_shell = false;
    FileAssociationBackend backend(executable_, application_name_, quiet);
    result = backend.UnregisterApplicationImpl(formats);
    if (include_shell) for (const auto target : {ShellIntegrationTarget::audio_cd, ShellIntegrationTarget::directory}) {
        if (!result) break;
        result = backend.SetShellIntegrationImpl(target, false, {});
    }
    return journal.Finish(result, options_.notify_shell);
}

AssociationQuery FileAssociationBackend::QueryExtension(
    std::wstring_view extension) const {
    AssociationQuery query;
    query.extension = NormalizeExtension(extension);
    if (query.extension.empty()) {
        query.result = Invalid(i18n::Literal(L"query extension"), i18n::Literal(L"invalid extension"));
        return query;
    }
    query.managed_prog_id = ManagedProgId(options_, query.extension);
    const std::wstring extension_key = JoinRegistryPath(
        options_.current_user_classes_subkey, L"." + query.extension);
    bool present = false;
    query.result = ReadDefaultAt(extension_key, query.current_prog_id, present);
    if (!query.result) return query;
    query.associated = present &&
                       EqualInsensitive(query.current_prog_id,
                                        query.managed_prog_id);
    if (query.associated) {
        std::wstring registered_command;
        bool command_present = false;
        query.result = ReadDefaultAt(
            JoinRegistryPath(options_.current_user_classes_subkey,
                             query.managed_prog_id + L"\\shell\\open\\command"),
            registered_command, command_present);
        if (!query.result) return query;
        query.associated = command_present && EqualInsensitive(
                                                 registered_command,
                                                 OpenCommand(executable_, false));
    }

    UniqueRegKey key;
    LONG status = OpenKey(extension_key, KEY_QUERY_VALUE, key);
    if (status == ERROR_SUCCESS) {
        std::optional<std::wstring> backup;
        status = ReadStringValue(key.get(), kBackupValue, backup);
        if (status == ERROR_SUCCESS && backup) query.backup_prog_id = *backup;
        else if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            query.result = Win32Failure(FileAssociationError::registry,
                                        i18n::Literal(L"read extension backup"), status);
            return query;
        }
    } else if (status != ERROR_FILE_NOT_FOUND) {
        query.result = Win32Failure(FileAssociationError::registry,
                                    i18n::Literal(L"open extension key"), status);
        return query;
    }

    const std::wstring icon_key = JoinRegistryPath(
        options_.current_user_classes_subkey,
        query.managed_prog_id + L"\\DefaultIcon");
    bool icon_present = false;
    query.result = ReadDefaultAt(icon_key, query.icon, icon_present);
    if (!query.result) return query;

    // UserChoice can select our ProgID without HKCU\\Classes\\.ext pointing
    // at it. Query the effective shell executable independently of that key.
    query.effective = query.associated;
    if (IsRealClassesStore(options_.current_user_classes_subkey)) {
        query.effective = false;
        const std::wstring dotted = L"." + query.extension;
        DWORD characters = 0;
        HRESULT shell_result = AssocQueryStringW(
            ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, dotted.c_str(),
            nullptr, nullptr, &characters);
        if (shell_result == S_FALSE || shell_result == E_POINTER ||
            HRESULT_CODE(shell_result) == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<wchar_t> buffer(characters + 1U, L'\0');
            shell_result = AssocQueryStringW(
                ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
                dotted.c_str(), nullptr, buffer.data(), &characters);
            if (SUCCEEDED(shell_result)) {
                query.effective = EqualInsensitive(
                    CanonicalForCompare(buffer.data()).wstring(),
                    CanonicalForCompare(executable_).wstring());
            }
        }
    }
    query.result = Success();
    return query;
}

FileAssociationResult FileAssociationBackend::SetLegacyExtensionAssociation(
    std::wstring_view extension, bool enabled, std::wstring_view description,
    std::wstring_view icon, const ShellVerbLabels& labels) {
    const std::wstring normalized = NormalizeExtension(extension);
    if (normalized.empty())
        return Invalid(i18n::Literal(L"set extension association"), i18n::Literal(L"invalid extension"));
    if (executable_.empty())
        return Invalid(i18n::Literal(L"set extension association"), i18n::Literal(L"executable is empty"));

    const std::wstring owner = executable_.wstring();
    const std::wstring prog_id = ManagedProgId(options_, normalized);
    const std::wstring extension_key = JoinRegistryPath(
        options_.current_user_classes_subkey, L"." + normalized);
    const std::wstring class_key = JoinRegistryPath(
        options_.current_user_classes_subkey, prog_id);
    const std::wstring shell_key = JoinRegistryPath(class_key, L"shell");
    const std::wstring open_key = JoinRegistryPath(shell_key, L"open");
    const std::wstring playlist_key = JoinRegistryPath(shell_key, L"PlayList");

    bool changed = false;
    auto apply = [&](std::wstring_view path, std::wstring_view value) {
        ManagedChange change = enabled
                                   ? InstallManagedDefault(path, value, owner)
                                   : RestoreManagedDefault(path, owner);
        changed = changed || change.changed;
        change.result.changed = changed;
        return change.result;
    };

    const auto mode = AssociationMode(options_);
    bool restore_overrides = true;
    if (!enabled && mode != FileAssociationMode::user_choice) {
        std::wstring current_default;
        bool present{};
        const auto read = ReadDefaultAt(extension_key, current_default, present);
        if (!read) return read;
        // Restoring an old Explorer override would mask a later .ext choice.
        restore_overrides = present && EqualInsensitive(current_default, prog_id);
        if (mode == FileAssociationMode::vista_windows7) {
            UniqueRegKey choice;
            const auto status = OpenKey(ExplorerExtensionPath(options_, normalized) + L"\\UserChoice",
                                         KEY_QUERY_VALUE, choice);
            if (status == ERROR_SUCCESS) restore_overrides = false;
            else if (status != ERROR_FILE_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read later legacy UserChoice"), status);
        }
    }
    const auto update_overrides = [&]() {
        // /unreg also calls this routine on newer Windows to restore owned
        // Classes values. That must never permit changes to modern UserChoice.
        if (mode == FileAssociationMode::user_choice) return Success();
        const auto explorer_key = ExplorerExtensionPath(options_, normalized);
        // Restore in reverse installation order so a newly created empty
        // FileExts key can be removed by its first value's ownership record.
        if (!enabled && mode == FileAssociationMode::vista_windows7) {
            auto update = ManageLegacyUserChoice(explorer_key + L"\\UserChoice",
                class_key + L"\\LegacyUserChoiceBackup", owner, false, restore_overrides);
            changed = changed || update.changed;
            if (!update.result) { update.result.changed = changed; return update.result; }
        }
        for (const auto* name : {enabled ? L"Application" : L"Progid",
                                 enabled ? L"Progid" : L"Application"}) {
            auto update = enabled
                ? InstallManagedValue(explorer_key, name, std::nullopt, owner)
                : RestoreManagedValue(explorer_key, name, owner, restore_overrides);
            changed = changed || update.changed;
            if (!update.result) { update.result.changed = changed; return update.result; }
        }
        if (enabled && mode == FileAssociationMode::vista_windows7) {
            auto update = ManageLegacyUserChoice(explorer_key + L"\\UserChoice",
                class_key + L"\\LegacyUserChoiceBackup", owner, true, false);
            changed = changed || update.changed;
            if (!update.result) { update.result.changed = changed; return update.result; }
        }
        return Success(changed);
    };

    FileAssociationResult result;
    if (enabled) {
        std::wstring type_description(description);
        if (type_description.empty())
            type_description = application_name_ + L" " + normalized +
                               L" audio file";
        const std::wstring icon_value = icon.empty()
                                            ? DefaultIcon(executable_)
                                            : std::wstring(icon);
        const std::array<std::pair<std::wstring, std::wstring>, 8> entries{{
            {class_key, type_description},
            {JoinRegistryPath(class_key, L"DefaultIcon"), icon_value},
            {shell_key, L"open"},
            {open_key, L""},
            {JoinRegistryPath(open_key, L"command"),
             OpenCommand(executable_, false)},
            {playlist_key, labels.add_to_playlist.empty()
                               ? L"Add to TTPlayer playlist"
                               : labels.add_to_playlist},
            {JoinRegistryPath(playlist_key, L"command"),
             OpenCommand(executable_, true)},
            // Publish the default only after its icon and commands exist.
            {extension_key, prog_id},
        }};
        for (const auto& entry : entries) {
            result = apply(entry.first, entry.second);
            if (!result) return result;
        }
        result = update_overrides();
        if (!result) return result;
    } else {
        result = update_overrides();
        if (!result) return result;
        const std::array<std::wstring, 8> paths{
            JoinRegistryPath(playlist_key, L"command"), playlist_key,
            JoinRegistryPath(open_key, L"command"), open_key, shell_key,
            JoinRegistryPath(class_key, L"DefaultIcon"), class_key,
            extension_key,
        };
        for (const auto& path : paths) {
            result = apply(path, L"");
            if (!result) return result;
        }
    }
    result = Success(changed);
    if (changed && options_.notify_shell) NotifyShellAssociationsChanged();
    return result;
}

FileAssociationResult FileAssociationBackend::SetExtensionIconImpl(
    std::wstring_view extension, std::wstring_view icon) {
    AssociationQuery query = QueryExtension(extension);
    if (!query.result) return query.result;
    std::wstring command;
    bool present{};
    const auto read_command = ReadDefaultAt(JoinRegistryPath(options_.current_user_classes_subkey,
        query.managed_prog_id + L"\\shell\\open\\command"), command, present);
    if (!read_command) return read_command;
    if (!present || !EqualInsensitive(command, OpenCommand(executable_, false)))
        return Win32Failure(FileAssociationError::registry,
                            i18n::Literal(L"set icon for an unmanaged association"),
                            ERROR_NOT_FOUND);
    const std::wstring icon_value = icon.empty() ? DefaultIcon(executable_)
                                                 : std::wstring(icon);
    const std::wstring key = JoinRegistryPath(
        options_.current_user_classes_subkey,
        query.managed_prog_id + L"\\DefaultIcon");
    ManagedChange change = InstallManagedDefault(key, icon_value,
                                                  executable_.wstring());
    if (change.result && change.changed && options_.notify_shell)
        NotifyShellAssociationsChanged();
    change.result.changed = change.changed;
    return change.result;
}

FileAssociationMode SelectFileAssociationMode(DWORD major, DWORD minor) noexcept {
    if (major < 6) return FileAssociationMode::xp;
    if (major == 6 && minor < 2) return FileAssociationMode::vista_windows7;
    return FileAssociationMode::user_choice;
}

DefaultAppsTarget SelectDefaultAppsTarget(DWORD major, DWORD minor, DWORD build, DWORD revision) noexcept {
    (void)minor;
    if (major < 6) return DefaultAppsTarget::folder_options;
    if (major < 10) return DefaultAppsTarget::control_panel;
    if (build >= 22631 || (build == 22621 && revision >= 1555) ||
        (build == 22000 && revision >= 1817)) return DefaultAppsTarget::application_settings;
    return DefaultAppsTarget::settings;
}

FileAssociationResult FileAssociationBackend::RegisterApplicationImpl(
    const std::vector<AssociableExtension>& formats) {
    if (executable_.empty()) return Invalid(i18n::Literal(L"register default program"), i18n::Literal(L"executable is empty"));
    // Validate the complete input before changing any keys.
    for (const auto& format : formats)
        if (NormalizeExtension(format.extension).empty())
            return Invalid(i18n::Literal(L"register default program"), i18n::Literal(L"invalid extension"));
    const auto capabilities = CapabilitiesPath(options_);
    UniqueRegKey key;
    LONG status = OpenKey(capabilities, KEY_QUERY_VALUE, key);
    if (status == ERROR_SUCCESS) {
        std::optional<std::wstring> owner;
        status = ReadStringValue(key.get(), kOwnerValue, owner);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read capabilities owner"), status);
        if (owner && !EqualInsensitive(*owner, executable_.wstring()))
            return OwnershipConflict(capabilities, *owner, executable_.wstring());
    } else if (status != ERROR_FILE_NOT_FOUND) {
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"open capabilities"), status);
    }
    key.reset();
    status = OpenKey(RegisteredApplicationsPath(options_), KEY_QUERY_VALUE, key);
    if (status == ERROR_SUCCESS) {
        std::optional<std::wstring> registered;
        status = ReadStringValue(key.get(), L"TTPlayerRebuild", registered);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read RegisteredApplications"), status);
        if (registered && !EqualInsensitive(*registered, capabilities))
            return OwnershipConflict(RegisteredApplicationsPath(options_), *registered, executable_.wstring());
    } else if (status != ERROR_FILE_NOT_FOUND) {
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"open RegisteredApplications"), status);
    }
    bool changed = false;
    const auto write = [&](std::wstring_view path, const wchar_t* name, std::wstring_view value) {
        auto result = SetRegistrationString(path, name, value);
        changed = changed || result.changed;
        return result;
    };
    for (const auto& [name, value] : std::vector<std::pair<const wchar_t*,std::wstring>>{
        {kOwnerValue, executable_.wstring()}, {L"ApplicationName", L"TTPlayerRebuild"},
        {L"ApplicationDescription", application_name_ + L" - audio and playlist player"},
        {L"ApplicationIcon", DefaultIcon(executable_)}}) {
        const auto result = write(capabilities, name, value);
        if (!result) return result;
    }
    std::unordered_set<std::wstring> visited;
    for (const auto& format : formats) {
        const auto extension = NormalizeExtension(format.extension);
        if (!visited.insert(extension).second) continue;
        const auto prog_id = ManagedProgId(options_, extension);
        const auto class_key = JoinRegistryPath(options_.current_user_classes_subkey, prog_id);
        // No .ext default is written here. A registration is merely a candidate.
        for (const auto& [path, value] : std::vector<std::pair<std::wstring,std::wstring>>{
            {class_key, format.description.empty() ? application_name_ + L" " + extension : format.description},
            {class_key + L"\\DefaultIcon", DefaultIcon(executable_)},
            {class_key + L"\\shell", L"open"},
            {class_key + L"\\shell\\open\\command", OpenCommand(executable_, false)},
            {class_key + L"\\shell\\PlayList\\command", OpenCommand(executable_, true)}}) {
            // Preserve a previously selected file-type icon when refreshing
            // the catalog, instead of replacing it with the EXE's icon.
            if (path == class_key + L"\\DefaultIcon") {
                std::wstring existing; bool present{};
                const auto read = ReadDefaultAt(path, existing, present);
                if (!read) return read;
                if (present) continue;
            }
            const auto result = InstallManagedDefault(path, value, executable_.wstring());
            if (!result.result) return result.result;
            changed = changed || result.changed;
        }
        auto result = SetOpenWith(options_, extension, prog_id, true);
        if (!result) return result;
        changed = changed || result.changed;
        result = write(capabilities + L"\\FileAssociations", (L"." + extension).c_str(), prog_id);
        if (!result) return result;
    }
    // Publish only after the ProgIDs and capabilities can be resolved.
    const auto result = write(RegisteredApplicationsPath(options_), L"TTPlayerRebuild", capabilities);
    if (!result) return result;
    if (changed && options_.notify_shell) NotifyShellAssociationsChanged();
    return Success(changed);
}

FileAssociationResult FileAssociationBackend::SetExtensionAssociationImpl(
    std::wstring_view extension, bool enabled, std::wstring_view description,
    std::wstring_view icon, const ShellVerbLabels& labels) {
    if (AssociationMode(options_) != FileAssociationMode::user_choice)
        return SetLegacyExtensionAssociation(extension, enabled, description, icon, labels);
    const auto normalized = NormalizeExtension(extension);
    if (normalized.empty()) return Invalid(i18n::Literal(L"set extension association"), i18n::Literal(L"invalid extension"));
    FileAssociationResult result;
    if (enabled) {
        result = RegisterApplicationImpl({{normalized, std::wstring(description), {}}});
        if (!result) return result;
        if (!icon.empty()) {
            const auto update = SetExtensionIconImpl(normalized, icon);
            if (!update) return update;
            result.changed = result.changed || update.changed;
        }
        // Preserve the localized Add-to-playlist verb.
        if (!labels.add_to_playlist.empty()) {
            const auto update = InstallManagedDefault(JoinRegistryPath(options_.current_user_classes_subkey,
                ManagedProgId(options_, normalized) + L"\\shell\\PlayList"),
                labels.add_to_playlist, executable_.wstring());
            if (!update.result) return update.result;
            result.changed = result.changed || update.changed;
        }
    }
    const auto query = QueryExtension(normalized);
    if (!query.result) return query.result;
    // Windows 8+ owns UserChoice; no deletion, hash forgery or default-key
    // fallback. Disabling also needs the user to choose a replacement handler.
    result.requires_user_choice = enabled != query.effective;
    return result;
}

FileAssociationResult FileAssociationBackend::OpenDefaultPrograms(HWND owner) const {
    if (!IsRealClassesStore(options_.current_user_classes_subkey))
        return Invalid(i18n::Literal(L"open default programs"), i18n::Literal(L"isolated registration cannot launch the live shell"));
    const auto version = AssociationWindowsVersion();
    DWORD revision{};
    UniqueRegKey key;
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     0, KEY_QUERY_VALUE, &raw) == ERROR_SUCCESS) {
        key.reset(raw);
        std::optional<DWORD> value;
        if (ReadDwordValue(key.get(), L"UBR", value) == ERROR_SUCCESS && value) revision = *value;
    }
    const auto target = SelectDefaultAppsTarget(version.dwMajorVersion, version.dwMinorVersion,
                                               version.dwBuildNumber, revision);
    const auto launch = [owner](const wchar_t* file, const wchar_t* arguments) {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner; info.lpVerb = L"open"; info.lpFile = file;
        info.lpParameters = arguments; info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) ? Success() : Win32Failure(
            FileAssociationError::shell, i18n::Literal(L"open system default programs"), GetLastError());
    };
    if (target == DefaultAppsTarget::settings || target == DefaultAppsTarget::application_settings) {
        auto result = launch(target == DefaultAppsTarget::application_settings
            ? L"ms-settings:defaultapps?registeredAppUser=TTPlayerRebuild"
            : L"ms-settings:defaultapps", nullptr);
        if (result) return result;
        result = launch(L"ms-settings:defaultapps", nullptr);
        if (result) return result;
    } else if (target == DefaultAppsTarget::control_panel) {
        IApplicationAssociationRegistrationUI* raw_ui = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw_ui));
        UniqueComPtr<IApplicationAssociationRegistrationUI> ui(raw_ui);
        if (SUCCEEDED(hr)) hr = ui->LaunchAdvancedAssociationUI(L"TTPlayerRebuild");
        if (SUCCEEDED(hr)) return Success();
    }
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH))
        return Win32Failure(FileAssociationError::shell, L"GetSystemDirectoryW", GetLastError());
    const auto control = std::filesystem::path(system) / L"control.exe";
    return launch(control.c_str(), target == DefaultAppsTarget::folder_options
        ? L"folders" : L"/name Microsoft.DefaultPrograms /page pageDefaultProgram");
}

FileAssociationResult FileAssociationBackend::UnregisterApplicationImpl(
    const std::vector<AssociableExtension>& legacy_formats) {
    std::unordered_set<std::wstring> extensions;
    for (const auto& format : legacy_formats) {
        const auto extension = NormalizeExtension(format.extension);
        if (extension.empty()) return Invalid(i18n::Literal(L"unregister default program"), i18n::Literal(L"invalid extension"));
        extensions.insert(extension);
    }
    const auto capabilities = CapabilitiesPath(options_);
    UniqueRegKey key;
    LONG status = OpenKey(capabilities, KEY_QUERY_VALUE, key);
    const bool has_capabilities = status == ERROR_SUCCESS;
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"open capabilities"), status);
    if (has_capabilities) {
        std::optional<std::wstring> owner;
        status = ReadStringValue(key.get(), kOwnerValue, owner);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read capabilities owner"), status);
        if (!owner || !EqualInsensitive(*owner, executable_.wstring())) return Success();
    }
    key.reset();
    status = OpenKey(capabilities + L"\\FileAssociations", KEY_QUERY_VALUE, key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read registered formats"), status);
    if (key) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]{}; DWORD size = 256;
            status = RegEnumValueW(key.get(), index, name, &size, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) return Win32Failure(FileAssociationError::registry, i18n::Literal(L"enumerate registered formats"), status);
            const auto extension = NormalizeExtension(name);
            std::optional<std::wstring> prog_id;
            if (!extension.empty() && ReadStringValue(key.get(), name, prog_id) == ERROR_SUCCESS &&
                prog_id && *prog_id == ManagedProgId(options_, extension)) extensions.insert(extension);
        }
    }
    key.reset();
    bool changed = false;
    auto legacy_options = options_;
    legacy_options.prog_id_prefix = L"Audio";
    legacy_options.notify_shell = false;
    FileAssociationBackend legacy(executable_, application_name_, legacy_options);
    for (const auto& extension : extensions) {
        UniqueRegKey owned_class;
        std::optional<std::wstring> class_owner;
        const auto class_status = OpenKey(JoinRegistryPath(options_.current_user_classes_subkey,
            ManagedProgId(options_, extension)), KEY_QUERY_VALUE, owned_class);
        if (class_status == ERROR_SUCCESS) {
            const auto read = ReadStringValue(owned_class.get(), kOwnerValue, class_owner);
            if (read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND)
                return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read association owner"), read);
        } else if (class_status != ERROR_FILE_NOT_FOUND)
            return Win32Failure(FileAssociationError::registry, i18n::Literal(L"read association owner"), class_status);
        const bool remove_open_with = class_owner && EqualInsensitive(*class_owner, executable_.wstring());
        auto result = SetLegacyExtensionAssociation(extension, false, {}, {}, {});
        if (!result) return result;
        changed = changed || result.changed;
        // Older rebuilds shared Audio.* with the original. Restore only our
        // own backups; never delete an original or another player's values.
        result = legacy.SetLegacyExtensionAssociation(extension, false, {}, {}, {});
        if (!result) return result;
        changed = changed || result.changed;
        if (remove_open_with) {
            result = SetOpenWith(options_, extension, ManagedProgId(options_, extension), false);
            if (!result) return result;
            changed = changed || result.changed;
        }
    }
    if (!has_capabilities) {
        if (changed && options_.notify_shell) NotifyShellAssociationsChanged();
        return Success(changed);
    }
    status = OpenKey(RegisteredApplicationsPath(options_), KEY_QUERY_VALUE | KEY_SET_VALUE, key);
    if (status == ERROR_SUCCESS) {
        std::optional<std::wstring> value;
        status = ReadStringValue(key.get(), L"TTPlayerRebuild", value);
        if (status == ERROR_SUCCESS && value && *value == capabilities)
            status = DeleteRegistryValue(key.get(), L"TTPlayerRebuild");
    }
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"remove RegisteredApplications entry"), status);
    // Exact private key, checked against the stored executable owner above.
    status = DeleteRegistryTree(capabilities);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Win32Failure(FileAssociationError::registry, i18n::Literal(L"remove capabilities"), status);
    if (options_.notify_shell) NotifyShellAssociationsChanged();
    return Success(true);
}

ShellIntegrationQuery FileAssociationBackend::QueryShellIntegration(
    ShellIntegrationTarget target) const {
    ShellIntegrationQuery query;
    const std::wstring command_key = JoinRegistryPath(
        options_.current_user_classes_subkey,
        target == ShellIntegrationTarget::audio_cd
            ? L"AudioCD\\shell\\open\\command"
            : L"Directory\\shell\\Playback\\command");
    bool present = false;
    query.result = ReadDefaultAt(command_key, query.command, present);
    if (!query.result) return query;
    query.associated = present && EqualInsensitive(
                                      query.command,
                                      OpenCommand(executable_, false));
    return query;
}

FileAssociationResult FileAssociationBackend::SetShellIntegrationImpl(
    ShellIntegrationTarget target, bool enabled,
    const ShellVerbLabels& labels) {
    if (executable_.empty())
        return Invalid(i18n::Literal(L"set shell integration"), i18n::Literal(L"executable is empty"));
    const std::wstring owner = executable_.wstring();
    const std::wstring target_key = JoinRegistryPath(
        options_.current_user_classes_subkey,
        target == ShellIntegrationTarget::audio_cd ? L"AudioCD" : L"Directory");
    const std::wstring shell_key = JoinRegistryPath(target_key, L"shell");
    const std::wstring playback_key = JoinRegistryPath(
        shell_key, target == ShellIntegrationTarget::audio_cd ? L"open"
                                                               : L"Playback");
    const std::wstring playlist_key = JoinRegistryPath(shell_key, L"PlayList");
    bool changed = false;
    auto apply = [&](std::wstring_view path, std::wstring_view value) {
        ManagedChange change = enabled
                                   ? InstallManagedDefault(path, value, owner)
                                   : RestoreManagedDefault(path, owner);
        changed = changed || change.changed;
        return change.result;
    };

    FileAssociationResult result;
    if (enabled) {
        if (target == ShellIntegrationTarget::audio_cd) {
            result = apply(shell_key, L"open");
            if (!result) return result;
            result = apply(playback_key, L"");
        } else {
            result = apply(playback_key, labels.playback);
        }
        if (!result) return result;
        result = apply(JoinRegistryPath(playback_key, L"command"),
                       OpenCommand(executable_, false));
        if (!result) return result;
        result = apply(playlist_key, labels.add_to_playlist);
        if (!result) return result;
        result = apply(JoinRegistryPath(playlist_key, L"command"),
                       OpenCommand(executable_, true));
        if (!result) return result;
    } else {
        for (const auto& path : std::array<std::wstring, 5>{
                 JoinRegistryPath(playlist_key, L"command"), playlist_key,
                 JoinRegistryPath(playback_key, L"command"), playback_key,
                 shell_key,
             }) {
            // Directory\shell itself is never owned by this operation.
            if (path == shell_key &&
                target == ShellIntegrationTarget::directory)
                continue;
            result = apply(path, L"");
            if (!result) return result;
        }
    }
    result = Success(changed);
    if (changed && options_.notify_shell) NotifyShellAssociationsChanged();
    return result;
}

ShortcutQuery FileAssociationBackend::QueryShortcut(
    ShortcutLocation location, const ShortcutOptions& options) const {
    ShortcutQuery query;
    query.result = ResolveShortcutPath(location, options, application_name_,
                                       query.path);
    if (!query.result) return query;
    const DWORD attributes = GetFileAttributesW(query.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            query.exists = false;
            return query;
        }
        query.result = Win32Failure(FileAssociationError::filesystem,
                                    L"GetFileAttributesW", error);
        return query;
    }
    query.exists = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return query;
}

FileAssociationResult FileAssociationBackend::CreateShortcut(
    ShortcutLocation location, const ShortcutOptions& options) const {
    if (executable_.empty())
        return Invalid(i18n::Literal(L"create shortcut"), i18n::Literal(L"executable is empty"));
    std::filesystem::path shortcut;
    FileAssociationResult result = ResolveShortcutPath(
        location, options, application_name_, shortcut);
    if (!result) return result;

    std::error_code filesystem_error;
    std::filesystem::create_directories(shortcut.parent_path(),
                                        filesystem_error);
    if (filesystem_error) {
        return Win32Failure(FileAssociationError::filesystem,
                            i18n::Literal(L"create shortcut directory"),
                            static_cast<DWORD>(filesystem_error.value()));
    }

    ScopedComInitialization com;
    if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE)
        return HResultFailure(FileAssociationError::com, L"CoInitializeEx",
                              com.Result());

    IShellLinkW* raw_link = nullptr;
    HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      reinterpret_cast<void**>(&raw_link));
    UniqueComPtr<IShellLinkW> link(raw_link);
    if (FAILED(status))
        return HResultFailure(FileAssociationError::com,
                              L"CoCreateInstance(CLSID_ShellLink)", status);

    status = link->SetPath(executable_.c_str());
    if (SUCCEEDED(status) && !options.arguments.empty())
        status = link->SetArguments(options.arguments.c_str());
    const auto working_directory = options.working_directory.empty()
                                       ? executable_.parent_path()
                                       : options.working_directory;
    if (SUCCEEDED(status) && !working_directory.empty())
        status = link->SetWorkingDirectory(working_directory.c_str());
    const std::wstring description = options.description.empty()
                                         ? application_name_
                                         : options.description;
    if (SUCCEEDED(status) && !description.empty())
        status = link->SetDescription(description.c_str());
    const auto icon_path = options.icon_path.empty() ? executable_
                                                     : options.icon_path;
    if (SUCCEEDED(status) && !icon_path.empty())
        status = link->SetIconLocation(icon_path.c_str(), options.icon_index);
    if (SUCCEEDED(status) && !options.app_user_model_id.empty()) {
        IPropertyStore* raw_store{};
        status = link->QueryInterface(IID_PPV_ARGS(&raw_store));
        UniqueComPtr<IPropertyStore> store(raw_store);
        if (SUCCEEDED(status)) {
            PROPVARIANT id{};
            id.vt = VT_LPWSTR;
            id.pwszVal = const_cast<wchar_t*>(options.app_user_model_id.c_str());
            status = store->SetValue(PKEY_AppUserModel_ID, id);
            if (SUCCEEDED(status)) status = store->Commit();
        }
    }
    if (FAILED(status))
        return HResultFailure(FileAssociationError::com,
                              i18n::Literal(L"configure IShellLinkW"), status);

    IPersistFile* raw_persist = nullptr;
    status = link->QueryInterface(IID_IPersistFile,
                                  reinterpret_cast<void**>(&raw_persist));
    UniqueComPtr<IPersistFile> persist(raw_persist);
    if (FAILED(status))
        return HResultFailure(FileAssociationError::com,
                              L"IShellLinkW::QueryInterface(IPersistFile)",
                              status);
    status = persist->Save(shortcut.c_str(), TRUE);
    if (FAILED(status))
        return HResultFailure(FileAssociationError::com,
                              L"IPersistFile::Save", status);
    return Success(true);
}

FileAssociationResult FileAssociationBackend::RemoveShortcut(
    ShortcutLocation location, const ShortcutOptions& options) const {
    std::filesystem::path shortcut;
    FileAssociationResult result = ResolveShortcutPath(
        location, options, application_name_, shortcut);
    if (!result) return result;
    if (DeleteFileW(shortcut.c_str())) return Success(true);
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return Success(false);
    return Win32Failure(FileAssociationError::filesystem, L"DeleteFileW",
                        error);
}

void FileAssociationBackend::NotifyShellAssociationsChanged() noexcept {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace ttplayer::settings
