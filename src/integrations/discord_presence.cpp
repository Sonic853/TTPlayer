#include "ttplayer/integrations/discord_presence.h"

#include "ttplayer/core/text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <windows.h>

namespace ttplayer::integrations {
namespace {

constexpr DWORD kPipeIoTimeoutMilliseconds = 1500;
constexpr std::uint32_t kHandshakeOpcode = 0;
constexpr std::uint32_t kFrameOpcode = 1;
constexpr std::uint32_t kCloseOpcode = 2;
constexpr std::uint32_t kPingOpcode = 3;
constexpr std::uint32_t kPongOpcode = 4;
constexpr std::uint32_t kMaximumPayloadBytes = 1024U * 1024U;
// Local coalescing policy, not a claim about Discord's server-side rate limit.
constexpr auto kLyricUpdateInterval = std::chrono::seconds(2);

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(other.Release());
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }
    void Reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), iswspace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), iswspace)
                          .base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

std::wstring ResolveApplicationId(std::wstring configured) {
    return discord_presence_detail::SelectApplicationId(configured);
}

std::wstring ClampText(std::wstring_view text, size_t maximum_characters) {
    std::wstring result;
    result.reserve(std::min(text.size(), maximum_characters));
    size_t characters = 0;
    for (size_t index = 0;
         index < text.size() && characters < maximum_characters; ++index) {
        const wchar_t value = text[index];
        result.push_back(value);
        if (value >= 0xD800 && value <= 0xDBFF && index + 1 < text.size() &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
            result.push_back(text[++index]);
        }
        ++characters;
    }
    return result;
}

size_t CharacterCount(std::wstring_view text) {
    size_t count{};
    for (size_t index = 0; index < text.size(); ++index, ++count) {
        if (text[index] >= 0xD800 && text[index] <= 0xDBFF &&
            index + 1 < text.size() && text[index + 1] >= 0xDC00 &&
            text[index + 1] <= 0xDFFF) ++index;
    }
    return count;
}

std::wstring Ellipsize(std::wstring_view text, size_t budget) {
    if (CharacterCount(text) <= budget) return std::wstring(text);
    return budget == 0 ? std::wstring{} : ClampText(text, budget - 1) + L"…";
}

std::wstring ClockText(std::chrono::milliseconds position) {
    const auto seconds = std::max<std::int64_t>(0, position.count() / 1000);
    const auto two_digits = [](std::int64_t value) {
        return (value < 10 ? L"0" : L"") + std::to_wstring(value);
    };
    if (seconds < 3600)
        return two_digits(seconds / 60) + L":" + two_digits(seconds % 60);
    return std::to_wstring(seconds / 3600) + L":" +
           two_digits(seconds / 60 % 60) + L":" + two_digits(seconds % 60);
}

std::wstring ActivityStateText(const DiscordTrackPresence& presence) {
    const bool known_duration = presence.duration.count() > 0;
    const bool radio = presence.audio_kind == DiscordAudioKind::radio;
    std::wstring prefix;
    if (presence.playback == DiscordPlaybackState::paused) {
        prefix = L"已暂停 · ";
        if (radio && !known_duration) prefix += L"已收听 ";
        prefix += ClockText(discord_presence_detail::ProjectPresencePosition(
            presence, std::chrono::milliseconds::zero()));
        if (known_duration) prefix += L" / " + ClockText(presence.duration);
        else if (!radio) prefix += L" / 未知时长";
    } else if (radio) {
        prefix = known_duration ? L"电台" : L"电台直播";
    } else if (presence.audio_kind == DiscordAudioKind::network_audio) {
        prefix = known_duration ? L"网络音频" : L"网络音频 · 时长未知";
    } else if (!known_duration) {
        prefix = L"时长未知";
    }

    std::vector<std::wstring> parts;
    const bool have_lyric = presence.lyrics_enabled && !presence.lyric.empty() &&
        presence.position >= presence.lyric_start &&
        (!presence.lyric_end || presence.position < *presence.lyric_end);
    if (have_lyric) parts.push_back(L"♪ " + presence.lyric);
    if (!presence.station.empty() && presence.station != presence.title)
        parts.push_back(presence.station);
    if (auto artist = Trim(presence.artist); !artist.empty())
        parts.push_back(std::move(artist));
    if (auto album = Trim(presence.album); !album.empty())
        parts.push_back(L"专辑：" + album);
    if (parts.empty()) return prefix.empty() ? L"正在播放" : prefix;

    // Preserve the complete pause/time prefix and share the remaining 128
    // Unicode characters fairly. A long artist must not erase the album (or
    // station); unused space from short fields is assigned to the longer ones.
    constexpr size_t maximum_characters = 128;
    const size_t separators = parts.size() - (prefix.empty() ? 1 : 0);
    size_t remaining = maximum_characters - CharacterCount(prefix) - separators * 3;
    std::vector<size_t> budgets(parts.size());
    std::vector<size_t> lengths;
    for (const auto& part : parts) lengths.push_back(CharacterCount(part));
    while (remaining != 0) {
        bool allocated{};
        for (size_t index = 0; index < parts.size() && remaining != 0; ++index) {
            if (budgets[index] == lengths[index]) continue;
            const auto allocation = std::min({remaining, lengths[index] - budgets[index],
                size_t{have_lyric && index == 0 ? 3U : 1U}});
            budgets[index] += allocation;
            remaining -= allocation;
            allocated = true;
        }
        if (!allocated) break;
    }
    for (size_t index = 0; index < parts.size(); ++index) {
        if (!prefix.empty()) prefix += L" · ";
        prefix += Ellipsize(parts[index], budgets[index]);
    }
    return prefix;
}

std::string JsonEscape(std::wstring_view value) {
    std::string utf8;
    try {
        utf8 = core::WideToUtf8(ClampText(value, 128));
    } catch (...) {
        return {};
    }
    std::string result;
    result.reserve(utf8.size() + 8);
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : utf8) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result.push_back(hex[(ch >> 4U) & 0xFU]);
                result.push_back(hex[ch & 0xFU]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

bool Transfer(HANDLE pipe, void* bytes, DWORD length, bool write) {
    auto* cursor = static_cast<std::uint8_t*>(bytes);
    DWORD remaining = length;
    while (remaining != 0) {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.Get();
        DWORD transferred = 0;
        const BOOL started = write
            ? WriteFile(pipe, cursor, remaining, &transferred, &operation)
            : ReadFile(pipe, cursor, remaining, &transferred, &operation);
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event.Get(), kPipeIoTimeoutMilliseconds) !=
                WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                WaitForSingleObject(event.Get(), INFINITE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
                return false;
        }
        if (transferred == 0) return false;
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

bool WriteFrame(HANDLE pipe, std::uint32_t opcode, std::string_view json) {
    auto frame = discord_presence_detail::BuildRpcFrame(opcode, json);
    return !frame.empty() && Transfer(pipe, frame.data(),
                                      static_cast<DWORD>(frame.size()), true);
}

struct RpcFrame {
    std::uint32_t opcode{};
    std::string json;
};

std::optional<RpcFrame> ReadFrame(HANDLE pipe) {
    std::array<std::uint8_t, 8> header{};
    if (!Transfer(pipe, header.data(), static_cast<DWORD>(header.size()), false))
        return std::nullopt;
    RpcFrame result;
    std::uint32_t length{};
    std::memcpy(&result.opcode, header.data(), sizeof(result.opcode));
    std::memcpy(&length, header.data() + sizeof(result.opcode), sizeof(length));
    if (length > kMaximumPayloadBytes) return std::nullopt;
    result.json.resize(length);
    if (length != 0 &&
        !Transfer(pipe, result.json.data(), length, false)) return std::nullopt;
    return result;
}

bool AwaitFrame(HANDLE pipe, std::string_view marker,
                std::uint32_t expected_opcode = kFrameOpcode) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        const auto frame = ReadFrame(pipe);
        if (!frame || frame->opcode == kCloseOpcode) return false;
        if (frame->opcode == kPingOpcode) {
            if (!WriteFrame(pipe, kPongOpcode, frame->json)) return false;
            continue;
        }
        if (frame->opcode == expected_opcode &&
            (marker.empty() || frame->json.find(marker) != std::string::npos))
            return true;
    }
    return false;
}

UniqueHandle ConnectPipe(std::string_view application_id,
                         const std::wstring& test_pipe_name) {
    for (int index = 0; index < (test_pipe_name.empty() ? 10 : 1); ++index) {
        const std::wstring name = test_pipe_name.empty()
            ? L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(index)
            : test_pipe_name;
        UniqueHandle pipe(CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!pipe) continue;
        DWORD mode = PIPE_READMODE_BYTE;
        if (!SetNamedPipeHandleState(pipe.Get(), &mode, nullptr, nullptr))
            continue;
        if (!WriteFrame(pipe.Get(), kHandshakeOpcode,
                        discord_presence_detail::BuildHandshakeJson(
                            application_id))) continue;
        if (!AwaitFrame(pipe.Get(), "READY")) continue;
        return pipe;
    }
    return {};
}

std::string NextNonce() {
    static std::atomic<std::uint64_t> sequence{};
    return std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(++sequence);
}

bool Publish(HANDLE pipe, const DiscordTrackPresence* presence) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string nonce = NextNonce();
    const auto json = discord_presence_detail::BuildSetActivityJson(
        presence, GetCurrentProcessId(), nonce, now);
    if (!WriteFrame(pipe, kFrameOpcode, json)) return false;
    // Discord is free to insert JSON whitespace in replies, so match the
    // unique nonce value rather than depending on a particular serializer.
    return AwaitFrame(pipe, nonce);
}

bool ProbeConnection(HANDLE pipe) {
    const std::string payload = "{\"time\":" +
        std::to_string(GetTickCount64()) + "}";
    return WriteFrame(pipe, kPingOpcode, payload) &&
           AwaitFrame(pipe, {}, kPongOpcode);
}

bool SameStableActivity(const DiscordTrackPresence& left,
                        const DiscordTrackPresence& right) noexcept {
    return left.title == right.title && left.artist == right.artist &&
           left.album == right.album && left.duration == right.duration &&
           left.playback == right.playback &&
           left.audio_kind == right.audio_kind && left.station == right.station &&
           left.track_identity == right.track_identity &&
           left.timeline_revision == right.timeline_revision &&
           left.seek_pending == right.seek_pending &&
           left.lyrics_enabled == right.lyrics_enabled;
}

} // namespace

namespace discord_presence_detail {

bool IsValidApplicationId(std::wstring_view value) noexcept {
    if (value.size() < 17 || value.size() > 20) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t character) {
        return character >= L'0' && character <= L'9';
    });
}

std::wstring SelectApplicationId(std::wstring_view configured) {
    auto explicit_value = Trim(std::wstring(configured));
    if (!explicit_value.empty()) return explicit_value;
    return std::wstring(kDefaultDiscordApplicationId);
}

std::string BuildHandshakeJson(std::string_view application_id) {
    return "{\"v\":1,\"client_id\":\"" + std::string(application_id) +
           "\"}";
}

std::chrono::milliseconds ProjectPresencePosition(
    const DiscordTrackPresence& presence, std::chrono::milliseconds elapsed) {
    auto position = std::max(presence.position, std::chrono::milliseconds::zero());
    if (presence.playback == DiscordPlaybackState::playing &&
        !presence.seek_pending && elapsed.count() > 0) {
        position += std::min(elapsed, std::chrono::milliseconds::max() - position);
    }
    if (presence.duration.count() > 0) position = std::min(position, presence.duration);
    return position;
}

std::string BuildSetActivityJson(const DiscordTrackPresence* presence,
                                 std::uint32_t process_id,
                                 std::string_view nonce,
                                 std::int64_t unix_time_seconds) {
    std::string result = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
        std::to_string(process_id) + ",\"activity\":";
    if (!presence || presence->playback == DiscordPlaybackState::stopped) {
        result += "null";
    } else {
        auto title = Trim(presence->title);
        if (title.empty()) title = presence->station.empty()
            ? L"未知曲目" : presence->station;
        // Details (2) makes the member-list status show the song/program title
        // without changing the registered application name. This is local RPC;
        // its timestamps use Unix seconds, not Gateway's millisecond encoding.
        result += "{\"type\":2,\"status_display_type\":2,\"details\":\"" +
                  JsonEscape(Ellipsize(title, 128)) + "\",\"state\":\"" +
                  JsonEscape(ActivityStateText(*presence)) + "\"";
        if (presence->playback == DiscordPlaybackState::playing) {
            const auto position_seconds = ProjectPresencePosition(
                *presence, std::chrono::milliseconds::zero()).count() / 1000;
            const auto duration_seconds = std::max<std::int64_t>(
                0, presence->duration.count() / 1000);
            if (presence->duration.count() <= 0) {
                // Unknown duration is not an error and has no fictitious end
                // time. Discord counts up from the elapsed/listening position.
                result += ",\"timestamps\":{\"start\":" +
                    std::to_string(unix_time_seconds - position_seconds) + "}";
            } else if (duration_seconds > position_seconds) {
                result += ",\"timestamps\":{\"start\":" +
                    std::to_string(unix_time_seconds - position_seconds) +
                    ",\"end\":" +
                    std::to_string(unix_time_seconds + duration_seconds -
                                   position_seconds) + "}";
            }
        }
        result += "}";
    }
    result += "},\"nonce\":\"" + std::string(nonce) + "\"}";
    return result;
}

std::vector<std::uint8_t> BuildRpcFrame(std::uint32_t opcode,
                                        std::string_view json) {
    if (json.size() > std::numeric_limits<std::uint32_t>::max()) return {};
    const auto length = static_cast<std::uint32_t>(json.size());
    std::vector<std::uint8_t> result(sizeof(opcode) + sizeof(length) + length);
    std::memcpy(result.data(), &opcode, sizeof(opcode));
    std::memcpy(result.data() + sizeof(opcode), &length, sizeof(length));
    std::memcpy(result.data() + sizeof(opcode) + sizeof(length), json.data(),
                length);
    return result;
}

} // namespace discord_presence_detail

class DiscordPresence::Impl final {
public:
    explicit Impl(std::wstring test_pipe_name = {})
        : test_pipe_name_(std::move(test_pipe_name)), worker_([this] { Run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    void Configure(bool enabled, std::wstring application_id) {
        application_id = ResolveApplicationId(std::move(application_id));
        {
            std::lock_guard lock(mutex_);
            if (enabled_ == enabled && application_id_ == application_id)
                return;
            enabled_ = enabled;
            application_id_ = std::move(application_id);
            ++revision_;
        }
        changed_.notify_one();
    }

    void Update(DiscordTrackPresence presence,
                std::chrono::steady_clock::time_point observed_at) {
        const auto now = std::chrono::steady_clock::now();
        observed_at = std::min(observed_at, now);
        {
            std::lock_guard lock(mutex_);
            if (presence.playback == DiscordPlaybackState::stopped) {
                if (!desired_) return;
                desired_.reset();
                clock_anchor_.reset();
                ++urgent_revision_;
            } else {
                bool urgent = !desired_ || !SameStableActivity(*desired_, presence);
                if (!urgent && presence.playback == DiscordPlaybackState::paused)
                    urgent = desired_->position != presence.position;
                if (!urgent && clock_anchor_) {
                    const auto projected = discord_presence_detail::ProjectPresencePosition(
                        *clock_anchor_, std::chrono::duration_cast<std::chrono::milliseconds>(
                            observed_at - clock_anchor_time_));
                    // Recovery for unannounced clock discontinuities only.
                    // Explicit seeks use timeline_revision, with NO size threshold.
                    urgent = std::chrono::abs(projected - presence.position) >=
                        std::chrono::seconds(3);
                }
                const bool lyric_changed = !desired_ || desired_->lyric != presence.lyric;
                // Always retain the newest audio sample, even when the visible
                // activity is unchanged. Reconnect must not use a minutes-old
                // sample just because regular clock updates were deduplicated.
                desired_ = std::move(presence);
                desired_time_ = observed_at;
                if (!urgent && !lyric_changed) return;
                if (urgent) {
                    clock_anchor_ = desired_;
                    clock_anchor_time_ = observed_at;
                    ++urgent_revision_;
                }
            }
            ++revision_;
        }
        changed_.notify_one();
    }

    void Clear() {
        {
            std::lock_guard lock(mutex_);
            if (!desired_) return;
            desired_.reset();
            clock_anchor_.reset();
            ++revision_;
            ++urgent_revision_;
        }
        changed_.notify_one();
    }

private:
    struct Snapshot {
        bool enabled{};
        std::wstring application_id;
        std::optional<DiscordTrackPresence> desired;
        std::uint64_t revision{};
        std::chrono::steady_clock::time_point observed_at;
        std::uint64_t urgent_revision{};
        bool stopping{};
    };

    Snapshot GetSnapshot() {
        std::lock_guard lock(mutex_);
        return {enabled_, application_id_, desired_, revision_, desired_time_,
                urgent_revision_, stopping_};
    }

    void WaitForChange(std::uint64_t revision,
                       std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        const auto changed = [&] { return stopping_ || revision_ != revision; };
        if (deadline == std::chrono::steady_clock::time_point::max())
            changed_.wait(lock, changed);
        else changed_.wait_until(lock, deadline, changed);
    }

    void Run() noexcept {
        try {
            UniqueHandle pipe;
            std::wstring connected_id;
            bool published = false;
            std::uint64_t processed_revision{};
            std::uint64_t processed_urgent_revision{};
            bool needs_publish{};
            auto next_probe = std::chrono::steady_clock::time_point::max();
            auto retry_after = std::chrono::steady_clock::time_point::min();
            auto next_lyric_send = std::chrono::steady_clock::time_point::min();
            std::wstring retry_id;

            for (;;) {
                const Snapshot state = GetSnapshot();
                if (state.stopping) {
                    if (pipe && published) static_cast<void>(Publish(pipe.Get(), nullptr));
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                const bool valid = state.enabled &&
                    discord_presence_detail::IsValidApplicationId(
                        state.application_id);
                if (!valid) {
                    if (pipe && published)
                        static_cast<void>(Publish(pipe.Get(), nullptr));
                    pipe.Reset();
                    connected_id.clear();
                    published = false;
                    needs_publish = false;
                    retry_after = std::chrono::steady_clock::time_point::min();
                    WaitForChange(state.revision, std::chrono::steady_clock::time_point::max());
                    continue;
                }

                std::string application_id;
                try { application_id = core::WideToUtf8(state.application_id); }
                catch (...) { application_id.clear(); }
                if (application_id.empty()) {
                    WaitForChange(state.revision, now + std::chrono::seconds(5));
                    continue;
                }

                if (pipe && connected_id != state.application_id) {
                    if (published) static_cast<void>(Publish(pipe.Get(), nullptr));
                    pipe.Reset();
                    published = false;
                }
                if (!pipe) {
                    if (retry_id == state.application_id && now < retry_after) {
                        WaitForChange(state.revision, retry_after);
                        continue;
                    }
                    pipe = ConnectPipe(application_id, test_pipe_name_);
                    if (pipe) {
                        connected_id = state.application_id;
                        needs_publish = true;
                        next_probe = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    } else {
                        retry_id = state.application_id;
                        retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    }
                    // Connect/READY can block. Re-read ALL state afterwards,
                    // including a seek, pause, new track or disable during it.
                    continue;
                }

                if (needs_publish || state.revision != processed_revision) {
                    const bool urgent = needs_publish ||
                        state.urgent_revision != processed_urgent_revision || !state.desired;
                    if (!urgent && now < next_lyric_send) {
                        WaitForChange(state.revision, next_lyric_send);
                        continue;
                    }
                    // Do not queue individual lyric lines. At the send deadline
                    // this snapshot is the newest UI sample, not the first line
                    // which originally woke the thread. Expired lines are hidden
                    // by ActivityStateText if I/O/UI timing crosses a boundary.
                    auto projected = state.desired;
                    if (projected) projected->position =
                        discord_presence_detail::ProjectPresencePosition(*projected,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - state.observed_at));
                    if (Publish(pipe.Get(), projected ? &*projected : nullptr)) {
                        published = state.desired.has_value();
                        processed_revision = state.revision;
                        processed_urgent_revision = state.urgent_revision;
                        needs_publish = false;
                        next_lyric_send = std::chrono::steady_clock::now() + kLyricUpdateInterval;
                        next_probe = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    } else {
                        pipe.Reset();
                        published = false;
                        retry_id = state.application_id;
                        retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    }
                    continue;
                }
                if (now >= next_probe) {
                    if (!ProbeConnection(pipe.Get())) {
                        pipe.Reset();
                        published = false;
                        retry_id = state.application_id;
                        retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    }
                    next_probe = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    continue;
                }
                WaitForChange(state.revision, next_probe);
            }
        } catch (...) {
            // Presence is optional.  No transport or allocation failure may
            // take down the player or escape its background thread.
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_{};
    bool enabled_{};
    std::wstring application_id_;
    std::optional<DiscordTrackPresence> desired_;
    std::chrono::steady_clock::time_point desired_time_{};
    std::optional<DiscordTrackPresence> clock_anchor_;
    std::chrono::steady_clock::time_point clock_anchor_time_{};
    std::uint64_t revision_{};
    std::uint64_t urgent_revision_{};
    const std::wstring test_pipe_name_;
    std::thread worker_;
};

DiscordPresence::DiscordPresence() : impl_(std::make_unique<Impl>()) {}
DiscordPresence::DiscordPresence(std::wstring test_pipe_name)
    : impl_(std::make_unique<Impl>(std::move(test_pipe_name))) {}
DiscordPresence::~DiscordPresence() = default;

void DiscordPresence::Configure(bool enabled, std::wstring application_id) {
    impl_->Configure(enabled, std::move(application_id));
}

void DiscordPresence::Update(DiscordTrackPresence presence,
                              std::chrono::steady_clock::time_point observed_at) {
    impl_->Update(std::move(presence), observed_at);
}

void DiscordPresence::Clear() { impl_->Clear(); }

} // namespace ttplayer::integrations
