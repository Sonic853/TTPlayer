#include "ttplayer/i18n/i18n.h"
#include "ttplayer/i18n/api.h"
#include "ttplayer/core/text.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace ttplayer::i18n {
namespace {
struct State {
    HMODULE module{};
    const TtpI18nApi* api{};
    void* catalog{};
    std::mutex mutex;
    std::map<std::wstring, std::wstring, std::less<>> literals;
    ~State() { if (catalog) api->close(catalog); if (module) FreeLibrary(module); }
};
std::mutex state_mutex;
std::shared_ptr<State> state;
std::shared_ptr<State> Current() {
    const std::lock_guard lock(state_mutex);
    return state;
}
const char* Domain(HMODULE module) {
    return module == GetModuleHandleW(nullptr) ? "exe" : "ttpres";
}
std::string ResourceKey(HMODULE module, const char* kind, UINT id) {
    return std::string(Domain(module)) + '/' + kind + '/' + std::to_string(id);
}
std::optional<std::wstring> Lookup(const std::shared_ptr<State>& current,
    const char* context, std::wstring_view source, std::wstring_view plural = {}, uint64_t count = 1) {
    if (!current || source.empty()) return {};
    try {
        const auto id = core::WideToUtf8(source), multiple = core::WideToUtf8(plural);
        const auto size = current->api->lookup(current->catalog, context, id.c_str(), multiple.c_str(), count, nullptr, 0);
        if (!size || size > 1024 * 1024) return {};
        std::wstring translated(size, L'\0');
        if (current->api->lookup(current->catalog, context, id.c_str(), multiple.c_str(), count,
                                translated.data(), size) != size || translated.back() != 0) return {};
        translated.pop_back();
        if (translated.empty() || translated.find(L'\0') != translated.npos ||
            !CompatibleText(count == 1 || plural.empty() ? source : plural, translated)) return {};
        return translated;
    } catch (...) { return {}; }
}
std::wstring ResourceTranslation(std::string_view context, std::wstring_view fallback) {
    // Resource tables also contain URLs. These are protocol/configuration data.
    if (fallback.starts_with(L"http://") || fallback.starts_with(L"https://")) return std::wstring(fallback);
    // Read source text from the loaded resource, including its format contract.
    // The provider can match resource contexts across CHS/CHT source languages.
    auto translated = Lookup(Current(), std::string(context).c_str(), fallback);
    if (!translated) return std::wstring(fallback);
    const auto source = fallback;
    // Compound lists/filters and command-description separators are contracts.
    for (const auto separator : {L'|', L'\n', L'\t'})
        if (std::count(source.begin(), source.end(), separator) !=
            std::count(translated->begin(), translated->end(), separator)) return std::wstring(fallback);
    const auto patterns = [](std::wstring_view text) {
        std::vector<std::wstring> result;
        for (size_t at = 0; at < text.size();) {
            const auto end = text.find(L'|', at);
            auto field = text.substr(at, end == text.npos ? end : end - at);
            if (field.starts_with(L"*.")) result.emplace_back(field);
            if (end == text.npos) break;
            at = end + 1;
        }
        return result;
    };
    if (patterns(source) != patterns(*translated)) return std::wstring(fallback);
    return *translated;
}
std::wstring RawText(HMODULE module, UINT id) {
    const wchar_t* text{};
    const int size = module ? ::LoadStringW(module, id, reinterpret_cast<LPWSTR>(&text), 0) : 0;
    return size > 0 && text ? std::wstring(text, size) : std::wstring{};
}
std::wstring SystemLanguage() {
    wchar_t language[16]{}, country[16]{};
    const auto locale = MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT);
    if (!GetLocaleInfoW(locale, LOCALE_SISO639LANGNAME, language, 16)) return {};
    if (!GetLocaleInfoW(locale, LOCALE_SISO3166CTRYNAME, country, 16) || !country[0]) return language;
    return std::wstring(language) + L'_' + country;
}

// Read and rebuild standard/extended resource templates with explicit bounds.
// Only captions change. Resource ordinals, styles, fonts and creation data stay
// intact, and translated buffers belong to the dialog/page adapter.
class Template {
    const std::byte* bytes_{};
    size_t size_{}, at_{};
    std::vector<std::byte> output_;
    bool changed_{};
    HMODULE module_{};
    UINT id_{};
    void Check(size_t count) const { if (at_ > size_ || count > size_ - at_) throw std::runtime_error("dialog bounds"); }
    uint16_t WordAt(size_t at) const {
        if (at > size_ || size_ - at < 2) throw std::runtime_error("dialog word");
        uint16_t value{}; std::memcpy(&value, bytes_ + at, 2); return value;
    }
    uint32_t DwordAt(size_t at) const {
        if (at > size_ || size_ - at < 4) throw std::runtime_error("dialog dword");
        uint32_t value{}; std::memcpy(&value, bytes_ + at, 4); return value;
    }
    void Copy(size_t count) { Check(count); output_.insert(output_.end(), bytes_ + at_, bytes_ + at_ + count); at_ += count; }
    void Align() {
        const size_t next = (at_ + 3) & ~size_t(3);
        Check(next - at_); at_ = next;
        while (output_.size() % 4) output_.push_back(std::byte{});
    }
    std::wstring Field(const std::string& context = {}) {
        const size_t begin = at_;
        if (WordAt(at_) == 0xffff) { Copy(4); return {}; }
        while (WordAt(at_) != 0) at_ += 2;
        const auto length = (at_ - begin) / 2;
        at_ += 2;
        std::wstring source(length, L'\0');
        if (length) std::memcpy(source.data(), bytes_ + begin, length * 2);
        auto translated = context.empty() || source.empty() ? source : ResourceTranslation(context, source);
        changed_ |= translated != source;
        const auto* data = reinterpret_cast<const std::byte*>(translated.c_str());
        output_.insert(output_.end(), data, data + (translated.size() + 1) * 2);
        return translated;
    }
public:
    std::wstring caption;
    Template(HMODULE module, UINT id, const void* data, size_t size)
        : bytes_(static_cast<const std::byte*>(data)), size_(size), module_(module), id_(id) {}
    std::vector<std::byte> Run() {
        const bool extended = WordAt(0) == 1 && WordAt(2) == 0xffff;
        const DWORD style = DwordAt(extended ? 12 : 0);
        const WORD count = WordAt(extended ? 16 : 8);
        Copy(extended ? 26 : 18);
        Field(); Field();
        const auto key = ResourceKey(module_, "dialog", id_);
        caption = Field(key + "/title");
        if (style & DS_SETFONT) { Copy(extended ? 6 : 2); Field(); }
        std::map<UINT, unsigned> occurrences;
        for (unsigned i = 0; i < count; ++i) {
            Align();
            const auto control_style = DwordAt(at_ + (extended ? 8 : 0));
            UINT control = extended ? DwordAt(at_ + 20) : WordAt(at_ + 16);
            if (control == 0xffff) control = 0xffffffff;
            const auto occurrence = occurrences[control]++;
            Copy(extended ? 24 : 18);
            const auto ordinal = WordAt(at_) == 0xffff ? WordAt(at_ + 2) : 0;
            const auto klass = Field();
            const bool label = ordinal == 0x80 ||
                (ordinal == 0x82 && (control_style & SS_TYPEMASK) != SS_ICON &&
                 (control_style & SS_TYPEMASK) != SS_BITMAP) ||
                _wcsicmp(klass.c_str(), L"Button") == 0 ||
                _wcsicmp(klass.c_str(), L"Static") == 0 || _wcsicmp(klass.c_str(), L"SysLink") == 0;
            Field(label ? key + "/control/" + std::to_string(control) + '/' + std::to_string(occurrence) : "");
            const auto extra = WordAt(at_);
            Copy(2);
            // Extended templates specify following bytes; standard templates
            // include the size WORD in a non-empty creation-data array.
            if (extra) { if (!extended && extra < 2) throw std::runtime_error("dialog data"); Copy(extended ? extra : extra - 2); }
        }
        return changed_ ? output_ : std::vector<std::byte>{};
    }
};
std::pair<const void*, DWORD> DialogData(HMODULE module, LPCWSTR name) {
    if (!module) return {};
    const auto resource = FindResourceW(module, name, RT_DIALOG);
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    return loaded ? std::pair<const void*, DWORD>{LockResource(loaded), SizeofResource(module, resource)} : std::pair<const void*, DWORD>{};
}
void TranslateMenu(HMENU menu, const std::string& key, const std::string& parent = {}) {
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        const auto path = parent.empty() ? std::to_string(index) : parent + '.' + std::to_string(index);
        const int size = GetMenuStringW(menu, index, nullptr, 0, MF_BYPOSITION);
        if (size > 0) {
            std::wstring source(static_cast<size_t>(size) + 1, L'\0');
            GetMenuStringW(menu, index, source.data(), size + 1, MF_BYPOSITION); source.resize(size);
            auto translated = ResourceTranslation(key + "/item/" + path, source);
            if (translated != source) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_STRING; item.dwTypeData = translated.data();
                SetMenuItemInfoW(menu, index, TRUE, &item);
            }
        }
        if (const auto child = GetSubMenu(menu, index)) TranslateMenu(child, key, path);
    }
}
}

bool Initialize(const std::filesystem::path& runtime, std::wstring_view language) noexcept {
    Shutdown();
    try {
        if (runtime.empty() || language == L"source") return false;
        const auto selected = language.empty() || language == L"auto" ? SystemLanguage() : std::wstring(language);
        if (selected.empty()) return false;
        auto loaded = std::make_shared<State>();
        loaded->module = LoadLibraryW((runtime / L"ttp_i18n.dll").c_str());
        if (!loaded->module) return false;
        const auto get = reinterpret_cast<TtpI18nGetApiFn>(GetProcAddress(loaded->module, "TtpI18n_GetApi"));
        if (!get) return false;
        loaded->api = get(TTP_I18N_ABI_VERSION);
        if (!loaded->api || loaded->api->size < sizeof(TtpI18nApi) ||
            loaded->api->version != TTP_I18N_ABI_VERSION || !loaded->api->open ||
            !loaded->api->close || !loaded->api->lookup) return false;
        loaded->catalog = loaded->api->open((runtime / L"i18n").c_str(), selected.c_str());
        if (!loaded->catalog) return false;
        const std::lock_guard lock(state_mutex);
        state = std::move(loaded); return true;
    } catch (...) { return false; }
}
void Shutdown() noexcept { const std::lock_guard lock(state_mutex); state.reset(); }
bool Available() noexcept { return !!Current(); }
std::vector<std::wstring> Languages(const std::filesystem::path& runtime) {
    std::vector<std::wstring> result;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(runtime / L"i18n", error); !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().wstring();
        if (name.empty() || name.size() > 64 || name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@") != name.npos) continue;
        const auto base = it->path() / L"LC_MESSAGES";
        if (std::filesystem::is_regular_file(base / L"ttplayer.mo", error) ||
            std::filesystem::is_regular_file(base / L"ttplayer.po", error)) result.push_back(name);
        error.clear();
    }
    std::sort(result.begin(), result.end());
    return result;
}
std::wstring Text(std::wstring_view source, const char* context) {
    auto translated = Lookup(Current(), context, source);
    return translated ? std::move(*translated) : std::wstring(source);
}
const wchar_t* Literal(const wchar_t* source) {
    if (!source) return nullptr;
    const auto current = Current();
    if (!current) return source;
    const std::lock_guard lock(current->mutex);
    const auto found = current->literals.find(source);
    if (found != current->literals.end()) return found->second.c_str();
    auto translated = Lookup(current, "app", source);
    return current->literals.emplace(source, translated ? *translated : source).first->second.c_str();
}
std::wstring Plural(const char* context, std::wstring_view singular, std::wstring_view plural, uint64_t count) {
    auto translated = Lookup(Current(), context, singular, plural, count);
    return translated ? std::move(*translated) : std::wstring(count == 1 ? singular : plural);
}
std::wstring ResourceText(HMODULE module, UINT id) {
    return ResourceTranslation(ResourceKey(module, "string", id), RawText(module, id));
}
HMENU LoadMenu(HMODULE module, LPCWSTR name) {
    const auto menu = ::LoadMenuW(module, name);
    if (menu && IS_INTRESOURCE(name) && Available())
        TranslateMenu(menu, ResourceKey(module, "menu", static_cast<UINT>(reinterpret_cast<ULONG_PTR>(name))));
    return menu;
}
std::vector<std::byte> DialogTemplate(HMODULE module, LPCWSTR name) {
    if (!Available() || !IS_INTRESOURCE(name)) return {};
    try {
        const auto [data, size] = DialogData(module, name);
        return data ? Template(module, static_cast<UINT>(reinterpret_cast<ULONG_PTR>(name)), data, size).Run() : std::vector<std::byte>{};
    } catch (...) { return {}; }
}
std::wstring DialogCaption(HMODULE module, UINT id) {
    try {
        const auto [data, size] = DialogData(module, MAKEINTRESOURCEW(id));
        if (!data) return {};
        Template parser(module, id, data, size); parser.Run(); return parser.caption;
    } catch (...) { return {}; }
}

bool CompatibleText(std::wstring_view source, std::wstring_view translated) {
    const auto tokens = [](std::wstring_view text) {
        std::vector<std::wstring> printf_tokens, named;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != L'%') continue;
            const auto start = i;
            if (++i == text.size()) { printf_tokens.emplace_back(L"%"); break; }
            if (text[i] == L'%') { printf_tokens.emplace_back(L"%%"); continue; }
            if (text[i] == L'(') {
                const auto end = text.find(L')', i);
                if (end != text.npos) { named.emplace_back(text.substr(start, end - start + 1)); i = end; continue; }
            }
            std::wstring token;
            while (i < text.size() && std::wstring_view(L"-+ #0.123456789*").find(text[i]) != std::wstring_view::npos) {
                if (text[i] == L'*') printf_tokens.emplace_back(L"*");
                ++i;
            }
            while (i < text.size() && std::wstring_view(L"hljztLIw3264").find(text[i]) != std::wstring_view::npos) token += text[i++];
            if (i < text.size() && std::wstring_view(L"diuoxXfFeEgGaAcCsSpn").find(text[i]) != std::wstring_view::npos) {
                token += text[i]; printf_tokens.push_back(token);
            } else if (i < text.size() && text[i] == L'$') printf_tokens.emplace_back(L"invalid-positional-format");
            else printf_tokens.emplace_back(text.substr(start, i < text.size() ? i - start + 1 : text.size() - start));
        }
        std::sort(named.begin(), named.end());
        printf_tokens.insert(printf_tokens.end(), named.begin(), named.end());
        return printf_tokens;
    };
    return tokens(source) == tokens(translated);
}
}
