#include "ttplayer/lyrics/online_search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>

namespace ttplayer::lyrics {
struct OnlineSearch::State {
    std::mutex mutex;
    std::condition_variable changed;
    SearchSnapshot snapshot;
    bool canceled{};
    int download{-1};
};

namespace {
using Phase = SearchPhase;
class Callback final : public plugins::LyricSearchCallback {
public:
    explicit Callback(std::shared_ptr<OnlineSearch::State> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != plugins::kLyricCallbackId &&
            iid != plugins::kLyricHostId) return E_NOINTERFACE;
        *value = static_cast<plugins::LyricSearchCallback*>(this);
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnResults(int count, LPCWSTR* artists, LPCWSTR* titles) override {
        if (count < 0 || count > 10000 || (count && (!artists || !titles))) return E_INVALIDARG;
        return Update([&](SearchSnapshot& s) {
            if (s.phase != Phase::searching) return;
            s.results.clear();
            for (int i = 0; i < count; ++i)
                s.results.push_back({artists[i] ? artists[i] : L"", titles[i] ? titles[i] : L""});
            s.phase = Phase::results;
        });
    }
    HRESULT STDMETHODCALLTYPE OnDownload(int length, LPCWSTR text,
        LPCWSTR extra_title, LPCWSTR extra_url) override {
        if (length <= 0 || length > 2 * 1024 * 1024 || !text)
            return OnError(L"Invalid lyric download");
        return Update([&](SearchSnapshot& s) {
            if (s.phase != Phase::downloading) return;
            s.text.assign(text, static_cast<size_t>(length));
            while (!s.text.empty() && s.text.back() == L'\0') s.text.pop_back();
            s.extra_title = extra_title ? extra_title : L"";
            s.extra_url = extra_url ? extra_url : L"";
            s.phase = Phase::downloaded;
        });
    }
    HRESULT STDMETHODCALLTYPE OnError(LPCWSTR text) override {
        return Update([&](SearchSnapshot& s) {
            if (s.phase == Phase::downloaded) return;
            s.error = text ? text : L""; s.phase = Phase::failed;
        });
    }
    HRESULT STDMETHODCALLTYPE OnServer(LPCWSTR name) override {
        return Update([&](SearchSnapshot& s) { s.server = name ? name : L""; });
    }
private:
    template<class F> HRESULT Update(F&& update) noexcept {
        try {
            std::lock_guard lock(state_->mutex);
            if (state_->canceled) return E_ABORT;
            update(state_->snapshot);
            ++state_->snapshot.revision;
            state_->changed.notify_all();
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<OnlineSearch::State> state_;
};

void Run(const std::shared_ptr<OnlineSearch::State>& state,
         const std::shared_ptr<plugins::PluginManager>& library,
         size_t provider, const settings::NetworkSettings& network,
         const std::wstring& artist, const std::wstring& title) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* callback = new Callback(state);
    try {
        {
            std::lock_guard lock(state->mutex);
            if (state->canceled) {
                callback->Release();
                if (SUCCEEDED(com)) CoUninitialize();
                return;
            }
        }
        plugins::LyricNetworkConfig config;
        config.proxy_type = network.proxy_type;
        config.server = network.proxy_server.c_str();
        config.port = static_cast<DWORD>(std::clamp(network.proxy_port, 0, 65535));
        config.username = network.proxy_username.c_str();
        config.password = network.proxy_password.c_str();
        HRESULT hr = E_NOINTERFACE;
        auto session = library ? library->CreateLyricSearch(provider, callback, &config, &hr) : nullptr;
        // FUN_0043B9CE retries creator zero, not a hard-coded HTTP endpoint.
        if (!session && library && provider != 0) {
            provider = 0;
            session = library->CreateLyricSearch(provider, callback, &config, &hr);
        }
        {
            std::lock_guard lock(state->mutex);
            state->snapshot.provider = provider;
        }
        bool canceled;
        { std::lock_guard lock(state->mutex); canceled = state->canceled; }
        if (canceled) {
            // Closing during Initialize must not start a fresh network request.
        } else if (!session || FAILED(hr = session->Search(artist.c_str(), title.c_str()))) {
            callback->OnError(L"");
        } else {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(65);
            bool extra_read = false;
            std::unique_lock lock(state->mutex);
            while (!state->canceled) {
                const auto phase = state->snapshot.phase;
                if (phase == Phase::failed) break;
                if (phase == Phase::results && !extra_read) {
                    extra_read = true;
                    lock.unlock();
                    LPWSTR extra_title{}, extra_url{};
                    session->GetExtra(&extra_title, &extra_url);
                    lock.lock();
                    state->snapshot.extra_title = extra_title ? extra_title : L"";
                    state->snapshot.extra_url = extra_url ? extra_url : L"";
                    ++state->snapshot.revision;
                    CoTaskMemFree(extra_title); CoTaskMemFree(extra_url);
                }
                if (state->download >= 0) {
                    const int index = std::exchange(state->download, -1);
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(65);
                    lock.unlock();
                    hr = session->Download(index);
                    if (FAILED(hr)) callback->OnError(L"");
                    lock.lock();
                    continue;
                }
                if (phase != Phase::results && phase != Phase::downloaded &&
                    std::chrono::steady_clock::now() >= deadline) {
                    state->snapshot.phase = Phase::failed;
                    ++state->snapshot.revision;
                    break;
                }
                state->changed.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
        // session (and DLL's internal worker) is released here, never by WM_CLOSE.
    } catch (...) { callback->OnError(L""); }
    callback->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}
}

OnlineSearch::OnlineSearch(const plugins::PluginManager& plugins, size_t provider,
    settings::NetworkSettings network, std::wstring artist, std::wstring title)
    : state_(std::make_shared<State>()) {
    state_->snapshot.provider = provider;
    auto retained = plugins.RetainForBackground();
    std::thread([state = state_, retained = std::move(retained), provider,
        network = std::move(network), artist = std::move(artist), title = std::move(title)] {
        try { Run(state, retained, provider, network, artist, title); }
        catch (...) {
            std::lock_guard lock(state->mutex);
            if (!state->canceled) {
                state->snapshot.phase = Phase::failed;
                ++state->snapshot.revision;
            }
        }
    }).detach();
}
OnlineSearch::~OnlineSearch() { Cancel(); }
void OnlineSearch::Cancel() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->canceled = true;
    state_->snapshot.phase = Phase::canceled;
    ++state_->snapshot.revision;
    state_->changed.notify_all();
}
SearchSnapshot OnlineSearch::Snapshot() const {
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}
bool OnlineSearch::Download(int index) {
    std::lock_guard lock(state_->mutex);
    if (state_->canceled || (state_->snapshot.phase != Phase::results &&
        state_->snapshot.phase != Phase::downloaded) || index < 0 ||
        static_cast<size_t>(index) >= state_->snapshot.results.size()) return false;
    state_->download = index;
    state_->snapshot.text.clear();
    state_->snapshot.phase = Phase::downloading;
    ++state_->snapshot.revision;
    state_->changed.notify_all();
    return true;
}

std::wstring LyricFileName(std::wstring value) {
    for (auto& c : value)
        if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
    if (value.size() > 180) value.resize(180);
    if (value.empty()) value = L"lyrics";
    // Prevent DOS devices and alternate paths even when a server returns a filename.
    auto stem = value.substr(0, value.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
         stem[3] >= L'0' && stem[3] <= L'9')) value.insert(value.begin(), L'_');
    if (value.size() < 4 || _wcsicmp(value.c_str() + value.size() - 4, L".lrc")) value += L".lrc";
    return value;
}

size_t BestSearchResult(const std::vector<SearchResult>& results,
                       std::wstring_view artist, std::wstring_view title) {
    // Online branch of 0044497F (param_2 == 0), with 00444450/004444BA's
    // symmetric, word-boundary-aware comparison. Filename-only fallback
    // scores 1..3 belong to the LOCAL search branch and are not used here.
    auto normalize = [](std::wstring_view input) {
        if (input.empty()) return std::wstring{};
        const DWORD flags = LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE;
        const int count = LCMapStringW(LOCALE_USER_DEFAULT, flags, input.data(),
            static_cast<int>(input.size()), nullptr, 0);
        std::wstring result(input);
        if (count > 0) {
            result.resize(count);
            LCMapStringW(LOCALE_USER_DEFAULT, flags, input.data(),
                static_cast<int>(input.size()), result.data(), count);
        }
        const auto start = result.find_first_not_of(L" \t\r\n");
        return start == result.npos ? std::wstring{} :
            result.substr(start, result.find_last_not_of(L" \t\r\n") - start + 1);
    };
    auto separator = [](wchar_t c) {
        WORD type{};
        GetStringTypeW(CT_CTYPE1, &c, 1, &type);
        return !(type & (C1_ALPHA | C1_DIGIT));
    };
    auto stripped = [&](const std::wstring& input) {
        // 0044434E and the ten pairs in DAT_0053B910.
        constexpr std::wstring_view pairs = L"()[]{}<>（）［］｛｝《》【】“”";
        size_t start = 0;
        while (start < std::min<size_t>(input.size(), 4) && input[start] >= L'0' && input[start] <= L'9') ++start;
        if (start == 0 || start >= 4 || start >= input.size() - 1 || !separator(input[start])) start = 0;
        std::wstring result;
        for (size_t i = start; i < input.size(); ++i) {
            if (!separator(input[i])) { result += input[i]; continue; }
            const auto pair = pairs.find(input[i]);
            if (pair != pairs.npos && pair % 2 == 0) {
                const auto end = input.find(pairs[pair + 1], i + 1);
                if (end != input.npos) i = end;
            }
        }
        return result;
    };
    auto one_way = [&](const std::wstring& haystack, const std::wstring& needle) {
        if (needle.empty()) return 0;
        const auto at = haystack.find(needle);
        if (at == haystack.npos) return 0;
        if (haystack.size() == needle.size()) return 1;
        return (at == 0 || separator(haystack[at - 1])) &&
            (at + needle.size() == haystack.size() || separator(haystack[at + needle.size()])) ? -1 : 0;
    };
    auto match = [&](const std::wstring& a, const std::wstring& b) {
        const int value = one_way(a, b);
        return value ? value : one_way(b, a);
    };
    const auto a = normalize(artist), t = normalize(title);
    const auto clean_a = stripped(a), clean_t = stripped(t);
    int best = -1; size_t selected = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto ra = normalize(results[i].artist), rt = normalize(results[i].title);
        const int title_match = match(t, rt);
        const int clean_title_match = title_match ? 0 : match(clean_t, stripped(rt));
        int score = title_match == 1 ? 6 : title_match == -1 ? 5 :
            clean_title_match == 1 ? 5 : clean_title_match == -1 ? 4 : 0;
        if (score >= 4) {
            const int artist_match = match(a, ra);
            const int clean_artist_match = artist_match ? 0 : match(clean_a, stripped(ra));
            score += artist_match == 1 ? 3 : artist_match == -1 ? 2 :
                clean_artist_match == 1 ? 2 : clean_artist_match == -1 ? 1 : 0;
        }
        if (score > best) { best = score; selected = i; }
        if (score == 9) break;
    }
    return selected;
}

std::filesystem::path DownloadDirectory(const settings::LyricSettings& settings,
    const std::filesystem::path& media, const std::filesystem::path& runtime) {
    const bool local = media.wstring().find(L"://") == std::wstring::npos;
    if (local && !media.parent_path().empty() &&
        (settings.save_to_sound_folder || settings.download_folder.empty())) return media.parent_path();
    const auto folder = settings.download_folder.empty() ? std::filesystem::path(L"Lyrics") : settings.download_folder;
    return (folder.is_absolute() ? folder : runtime / folder).lexically_normal();
}

bool SaveDownloadedLyric(const std::filesystem::path& path, std::wstring_view text, bool overwrite) {
    if (text.empty() || text.size() > 2 * 1024 * 1024) return false;
    BOOL loss = FALSE;
    const UINT acp = GetACP();
    const bool utf8_acp = acp == CP_UTF8;
    int count = WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, utf8_acp ? nullptr : &loss);
    UINT codepage = count <= 0 || loss ? CP_UTF8 : CP_ACP;
    count = WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    std::string bytes(codepage == CP_UTF8 || utf8_acp ? "\xef\xbb\xbf" : "");
    const auto prefix = bytes.size(); bytes.resize(prefix + count);
    if (!WideCharToMultiByte(codepage, 0, text.data(), static_cast<int>(text.size()),
        bytes.data() + prefix, count, nullptr, nullptr)) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(path.parent_path().c_str(), L"lrc", 0, temporary)) return false;
    const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written{};
        ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        CloseHandle(file);
        if (ok) ok = MoveFileExW(temporary, path.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0) != FALSE;
    }
    if (!ok) DeleteFileW(temporary); // Only our generated temporary file.
    return ok;
}
} // namespace ttplayer::lyrics
