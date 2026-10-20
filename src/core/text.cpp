#include <ttpcomm/text.h>
#include "ttplayer/core/text.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace ttplayer::core {
std::string WideToUtf8(std::wstring_view value) { return ttpcomm::text::WideToUtf8(value); }
std::wstring Utf8ToWide(std::string_view value) { return ttpcomm::text::Utf8ToWide(value); }

std::string ReadUtf8Text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open text file");
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    if (data.starts_with("\xEF\xBB\xBF")) data.erase(0, 3);
    return data;
}

void WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes) {
    struct StagedFile {
        std::filesystem::path path;
        HANDLE handle{INVALID_HANDLE_VALUE};
        ~StagedFile() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (!path.empty()) DeleteFileW(path.c_str());
        }
    };
    const auto fail = [](DWORD error) {
        throw std::system_error(static_cast<int>(error ? error : ERROR_WRITE_FAULT),
                                std::system_category(), "cannot save file");
    };
    static std::atomic<unsigned long> sequence{};
    const auto destination = std::filesystem::absolute(path);
    const auto create_sibling = [&](StagedFile& file, bool backup = false) {
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto temporary = destination.parent_path() /
                (std::wstring(backup ? L".ttp-backup-" : L".ttp-save-") +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
                 (backup ? L".bak" : L".tmp"));
            file.handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.handle != INVALID_HANDLE_VALUE) {
                file.path = std::move(temporary);
                return;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) fail(error);
        }
        fail(ERROR_FILE_EXISTS);
    };
    StagedFile staged;
    create_sibling(staged);
    while (!bytes.empty()) {
        const auto count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1024 * 1024));
        DWORD written{};
        if (!WriteFile(staged.handle, bytes.data(), count, &written, nullptr)) fail(GetLastError());
        if (written != count) fail(ERROR_WRITE_FAULT);
        bytes.remove_prefix(written);
    }
    if (!FlushFileBuffers(staged.handle)) fail(GetLastError());
    if (!CloseHandle(staged.handle)) fail(GetLastError());
    staged.handle = INVALID_HANDLE_VALUE;
    const auto attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY))
            fail(ERROR_ACCESS_DENIED);
        // Preserve the old DACL, named streams, creation time and storage
        // attributes. Without a backup ReplaceFile's 1176 failure can remove
        // the original, so reserve an owned sibling even for small exports.
        StagedFile backup;
        create_sibling(backup, true);
        if (!CloseHandle(backup.handle)) fail(GetLastError());
        backup.handle = INVALID_HANDLE_VALUE;
        if (!ReplaceFileW(destination.c_str(), staged.path.c_str(),
                          backup.path.c_str(), 0, nullptr, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                // The original has moved to backup. Do not overwrite a file
                // another writer may have created while recovery was pending.
                const auto original = std::move(backup.path);
                backup.path.clear();
                if (!MoveFileExW(original.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                    throw std::filesystem::filesystem_error(
                        "cannot restore original file; backup retained", destination, original,
                        std::error_code(static_cast<int>(error), std::system_category()));
            }
            fail(error);
        }
    } else {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) fail(error);
        // A destination created concurrently is not ours to replace.
        if (!MoveFileExW(staged.path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            fail(GetLastError());
    }
    staged.path.clear();
}

void WriteUtf8Text(const std::filesystem::path& path, std::string_view text) {
    WriteFileAtomically(path, text);
}
}
