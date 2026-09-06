#include "ttplayer/settings/file_association.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>

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
    if (!length || !allocated) return L"Windows error " + std::to_wstring(error);
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

[[nodiscard]] LONG OpenKey(std::wstring_view path, REGSAM access,
                           UniqueRegKey& key) {
    HKEY raw = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER,
                                      std::wstring(path).c_str(), 0, access,
                                      &raw);
    key.reset(raw);
    return status;
}

[[nodiscard]] LONG CreateKey(std::wstring_view path, UniqueRegKey& key,
                             bool& existed) {
    HKEY raw = nullptr;
    DWORD disposition = 0;
    const LONG status = RegCreateKeyExW(
        HKEY_CURRENT_USER, std::wstring(path).c_str(), 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE |
                                    KEY_ENUMERATE_SUB_KEYS,
        nullptr, &raw, &disposition);
    key.reset(raw);
    existed = disposition == REG_OPENED_EXISTING_KEY;
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
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.data()), bytes);
}

[[nodiscard]] LONG WriteDwordValue(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

[[nodiscard]] LONG DeleteValueIfPresent(HKEY key,
                                        const wchar_t* name) noexcept {
    const LONG status = RegDeleteValueW(key, name);
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

[[nodiscard]] ManagedChange InstallManagedDefault(
    std::wstring_view path, std::wstring_view value,
    std::wstring_view owner_identity) {
    UniqueRegKey key;
    bool key_existed = false;
    LONG status = CreateKey(path, key, key_existed);
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             L"RegCreateKeyExW(" + std::wstring(path) + L")",
                             status), false};
    }

    std::optional<std::wstring> owner;
    status = ReadStringValue(key.get(), kOwnerValue, owner);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             L"read association owner", status), false};
    }
    if (owner && !EqualInsensitive(*owner, owner_identity)) {
        return {Win32Failure(FileAssociationError::registry,
                             L"association key is managed by another executable",
                             ERROR_SHARING_VIOLATION), false};
    }

    std::optional<std::wstring> previous;
    status = ReadStringValue(key.get(), nullptr, previous);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             L"read registry default value", status), false};
    }

    if (!owner) {
        if (previous) {
            status = WriteStringValue(key.get(), kBackupValue, *previous);
            if (status != ERROR_SUCCESS)
                return {Win32Failure(FileAssociationError::registry,
                                     L"save registry default backup", status),
                        false};
        } else {
            status = DeleteValueIfPresent(key.get(), kBackupValue);
            if (status != ERROR_SUCCESS)
                return {Win32Failure(FileAssociationError::registry,
                                     L"clear stale registry backup", status),
                        false};
        }
        status = WriteDwordValue(key.get(), kBackupPresentValue,
                                 previous ? 1U : 0U);
        if (status == ERROR_SUCCESS)
            status = WriteDwordValue(key.get(), kKeyExistedValue,
                                     key_existed ? 1U : 0U);
        if (status == ERROR_SUCCESS)
            status = WriteStringValue(key.get(), kOwnerValue, owner_identity);
        if (status != ERROR_SUCCESS)
            return {Win32Failure(FileAssociationError::registry,
                                 L"save association ownership metadata", status),
                    false};
    }

    status = WriteStringValue(key.get(), kInstalledValue, value);
    if (status == ERROR_SUCCESS)
        status = WriteStringValue(key.get(), nullptr, value);
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             L"write registry default value", status), false};
    }
    return {Success(!previous || *previous != value),
            !previous || *previous != value};
}

[[nodiscard]] ManagedChange RestoreManagedDefault(
    std::wstring_view path, std::wstring_view owner_identity) {
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
    status = ReadStringValue(key.get(), kOwnerValue, owner);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return {Win32Failure(FileAssociationError::registry,
                             L"read association owner", status), false};
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
    const auto read_optional_string = [&](const wchar_t* name,
                                          std::optional<std::wstring>& target) {
        const LONG result = ReadStringValue(key.get(), name, target);
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    };
    status = read_optional_string(nullptr, current);
    if (status == ERROR_SUCCESS)
        status = read_optional_string(kInstalledValue, installed);
    if (status == ERROR_SUCCESS)
        status = read_optional_string(kBackupValue, backup);
    LONG value_status = ReadDwordValue(key.get(), kBackupPresentValue,
                                       backup_present);
    if (value_status != ERROR_SUCCESS && value_status != ERROR_FILE_NOT_FOUND)
        status = value_status;
    value_status = ReadDwordValue(key.get(), kKeyExistedValue, key_existed);
    if (value_status != ERROR_SUCCESS && value_status != ERROR_FILE_NOT_FOUND)
        status = value_status;
    if (status != ERROR_SUCCESS) {
        return {Win32Failure(FileAssociationError::registry,
                             L"read association backup metadata", status),
                false};
    }

    bool changed = false;
    // Do not overwrite a default which the user or Windows changed after this
    // backend installed it (notably a protected UserChoice transition).
    if (installed && current && *installed == *current) {
        if (backup_present.value_or(0U) != 0U && backup) {
            status = WriteStringValue(key.get(), nullptr, *backup);
        } else {
            status = RegDeleteValueW(key.get(), nullptr);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
        if (status != ERROR_SUCCESS) {
            return {Win32Failure(FileAssociationError::registry,
                                 L"restore registry default value", status),
                    false};
        }
        changed = true;
    }

    for (const wchar_t* name : {kOwnerValue, kInstalledValue, kBackupValue,
                                kBackupPresentValue, kKeyExistedValue}) {
        status = DeleteValueIfPresent(key.get(), name);
        if (status != ERROR_SUCCESS) {
            auto failure = Win32Failure(FileAssociationError::registry,
                                        L"remove association metadata",
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
        status = RegDeleteKeyW(HKEY_CURRENT_USER, std::wstring(path).c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            return {Win32Failure(FileAssociationError::registry,
                                 L"remove empty association key", status),
                    changed};
        }
        changed = true;
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
                            L"read registry default value", status);
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
            return Invalid(L"resolve Programs shortcut",
                           L"programs_subdirectory must be relative");
        }
        for (const auto& component : subdirectory) {
            if (component == L"..")
                return Invalid(L"resolve Programs shortcut",
                               L"programs_subdirectory may not contain '..'");
        }
        directory /= subdirectory;
    }

    std::wstring name = options.display_name.empty()
                            ? std::wstring(application_name)
                            : options.display_name;
    if (name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        return Invalid(L"resolve shortcut path", L"invalid shortcut name");
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
        // resumes at that delimiter.  It deliberately does not canonicalize
        // case or de-duplicate extensions contributed by different readers.
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

AssociationQuery FileAssociationBackend::QueryExtension(
    std::wstring_view extension) const {
    AssociationQuery query;
    query.extension = NormalizeExtension(extension);
    if (query.extension.empty()) {
        query.result = Invalid(L"query extension", L"invalid extension");
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
                                        L"read extension backup", status);
            return query;
        }
    } else if (status != ERROR_FILE_NOT_FOUND) {
        query.result = Win32Failure(FileAssociationError::registry,
                                    L"open extension key", status);
        return query;
    }

    const std::wstring icon_key = JoinRegistryPath(
        options_.current_user_classes_subkey,
        query.managed_prog_id + L"\\DefaultIcon");
    bool icon_present = false;
    query.result = ReadDefaultAt(icon_key, query.icon, icon_present);
    if (!query.result) return query;

    if (query.associated && IsRealClassesStore(
                                options_.current_user_classes_subkey)) {
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

FileAssociationResult FileAssociationBackend::SetExtensionAssociation(
    std::wstring_view extension, bool enabled, std::wstring_view description,
    std::wstring_view icon, const ShellVerbLabels& labels) {
    const std::wstring normalized = NormalizeExtension(extension);
    if (normalized.empty())
        return Invalid(L"set extension association", L"invalid extension");
    if (executable_.empty())
        return Invalid(L"set extension association", L"executable is empty");

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
        return change.result;
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
            {extension_key, prog_id},
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
        }};
        for (const auto& entry : entries) {
            result = apply(entry.first, entry.second);
            if (!result) return result;
        }
    } else {
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

FileAssociationResult FileAssociationBackend::SetExtensionIcon(
    std::wstring_view extension, std::wstring_view icon) {
    AssociationQuery query = QueryExtension(extension);
    if (!query.result) return query.result;
    if (!query.associated)
        return Win32Failure(FileAssociationError::registry,
                            L"set icon for an unmanaged association",
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

FileAssociationResult FileAssociationBackend::SetShellIntegration(
    ShellIntegrationTarget target, bool enabled,
    const ShellVerbLabels& labels) {
    if (executable_.empty())
        return Invalid(L"set shell integration", L"executable is empty");
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
        return Invalid(L"create shortcut", L"executable is empty");
    std::filesystem::path shortcut;
    FileAssociationResult result = ResolveShortcutPath(
        location, options, application_name_, shortcut);
    if (!result) return result;

    std::error_code filesystem_error;
    std::filesystem::create_directories(shortcut.parent_path(),
                                        filesystem_error);
    if (filesystem_error) {
        return Win32Failure(FileAssociationError::filesystem,
                            L"create shortcut directory",
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
    if (FAILED(status))
        return HResultFailure(FileAssociationError::com,
                              L"configure IShellLinkW", status);

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
