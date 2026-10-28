#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::i18n {
// Only call Initialize/Shutdown at application-session boundaries. Language
// changes are saved for the next startup; existing HWND text stays valid.
bool Initialize(const std::filesystem::path& runtime, std::wstring_view language) noexcept;
void Shutdown() noexcept;
bool Available() noexcept;
bool EnglishLayout() noexcept;
// Map dynamic controls using the same horizontal expansion as translated templates.
BOOL MapDialogRect(HWND dialog, RECT* bounds) noexcept;
// Mark constant-array messages for extraction; translate when displaying them.
constexpr const wchar_t* Mark(const wchar_t* source) noexcept { return source; }
std::vector<std::wstring> Languages(const std::filesystem::path& runtime);
std::wstring Text(std::wstring_view source, const char* context = "app");
const wchar_t* Literal(const wchar_t* source);
std::wstring Plural(const char* context, std::wstring_view singular,
                    std::wstring_view plural, uint64_t count);
std::wstring ResourceText(HMODULE module, UINT id);
HMENU LoadMenu(HMODULE module, LPCWSTR name);
std::wstring DialogCaption(HMODULE module, UINT id);
// Empty means use the original template, including when i18n is unavailable.
std::vector<std::byte> DialogTemplate(HMODULE module, LPCWSTR name);
bool CompatibleText(std::wstring_view source, std::wstring_view translated);
}
