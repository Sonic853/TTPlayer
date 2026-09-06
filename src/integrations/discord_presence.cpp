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

UniqueHandle ConnectPipe(std::string_view application_id) {
    for (int index = 0; index < 10; ++index) {
        const std::wstring name = L"\\\\?\\pipe\\discord-ipc-" +
                                  std::to_wstring(index);
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
           left.playback == right.playback;
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

std::string BuildSetActivityJson(const DiscordTrackPresence* presence,
                                 std::uint32_t process_id,
                                 std::string_view nonce,
                                 std::int64_t unix_time_seconds) {
    std::string result = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
        std::to_string(process_id) + ",\"activity\":";
    if (!presence || presence->playback == DiscordPlaybackState::stopped) {
        result += "null";
    } else {
        std::wstring title = presence->title.empty()
            ? std::wstring(L"未知曲目") : presence->title;
        std::wstring state;
        if (presence->playback == DiscordPlaybackState::paused)
            state = L"已暂停";
        if (!presence->artist.empty()) {
            if (!state.empty()) state += L" · ";
            state += presence->artist;
        }
        if (!presence->album.empty()) {
            if (!state.empty()) state += L" · ";
            state += presence->album;
        }
        if (state.empty()) state = L"正在播放";

        result += "{\"type\":2,\"details\":\"" + JsonEscape(title) +
                  "\",\"state\":\"" + JsonEscape(state) + "\"";
        if (presence->playback == DiscordPlaybackState::playing) {
            const auto position_seconds = std::max<std::int64_t>(
                0, presence->position.count() / 1000);
            const auto duration_seconds = std::max<std::int64_t>(
                0, presence->duration.count() / 1000);
            if (duration_seconds > position_seconds) {
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
    Impl() : worker_([this] { Run(); }) {}

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

    void Update(DiscordTrackPresence presence) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex_);
            if (presence.playback == DiscordPlaybackState::stopped) {
                if (!desired_) return;
                desired_.reset();
            } else {
                // RefreshPlaybackUi runs four times per second.  Discord's
                // timestamp continues on its own, so only publish a seek when
                // the observed clock departs materially from that projection.
                if (desired_ && SameStableActivity(*desired_, presence)) {
                    if (presence.playback != DiscordPlaybackState::playing &&
                        desired_->position == presence.position) return;
                    if (presence.playback == DiscordPlaybackState::playing) {
                        const auto projected = desired_->position +
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - desired_time_);
                        if (std::chrono::abs(projected - presence.position) <
                            std::chrono::seconds(3)) return;
                    }
                }
                desired_ = std::move(presence);
                desired_time_ = now;
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
            ++revision_;
        }
        changed_.notify_one();
    }

private:
    struct Snapshot {
        bool enabled{};
        std::wstring application_id;
        std::optional<DiscordTrackPresence> desired;
        std::uint64_t revision{};
    };

    Snapshot GetSnapshot() {
        std::lock_guard lock(mutex_);
        return {enabled_, application_id_, desired_, revision_};
    }

    void Run() noexcept {
        try {
            UniqueHandle pipe;
            std::wstring connected_id;
            bool published = false;
            std::uint64_t processed_revision = 0;
            auto next_probe = std::chrono::steady_clock::time_point::max();

            for (;;) {
                bool periodic = false;
                {
                    std::unique_lock lock(mutex_);
                    if (next_probe == std::chrono::steady_clock::time_point::max()) {
                        changed_.wait(lock, [&] {
                            return stopping_ || revision_ != processed_revision;
                        });
                    } else if (!changed_.wait_until(lock, next_probe, [&] {
                                   return stopping_ ||
                                          revision_ != processed_revision;
                               })) {
                        periodic = true;
                    }
                    if (stopping_) {
                        lock.unlock();
                        if (pipe && published) static_cast<void>(Publish(pipe.Get(), nullptr));
                        return;
                    }
                }

                const std::uint64_t previous_revision = processed_revision;
                const Snapshot state = GetSnapshot();
                const bool state_changed =
                    state.revision != previous_revision;
                processed_revision = state.revision;
                const bool valid = state.enabled &&
                    discord_presence_detail::IsValidApplicationId(
                        state.application_id);
                if (!valid) {
                    if (pipe && published)
                        static_cast<void>(Publish(pipe.Get(), nullptr));
                    pipe.Reset();
                    connected_id.clear();
                    published = false;
                    next_probe = std::chrono::steady_clock::time_point::max();
                    continue;
                }

                std::string application_id;
                try { application_id = core::WideToUtf8(state.application_id); }
                catch (...) { application_id.clear(); }
                if (application_id.empty()) {
                    next_probe = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
                    continue;
                }

                if (pipe && connected_id != state.application_id) {
                    if (published) static_cast<void>(Publish(pipe.Get(), nullptr));
                    pipe.Reset();
                    published = false;
                }
                // A Configure/Update may race the instant a periodic wait
                // expires.  Publish that newer revision first; treating it as
                // a pure health probe would acknowledge and then lose it.
                if (periodic && !state_changed && pipe) {
                    if (ProbeConnection(pipe.Get())) {
                        next_probe = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(15);
                        continue;
                    }
                    pipe.Reset();
                    published = false;
                }
                if (!pipe) {
                    pipe = ConnectPipe(application_id);
                    if (pipe) connected_id = state.application_id;
                }

                bool success = false;
                if (pipe) {
                    success = Publish(pipe.Get(), state.desired
                        ? &*state.desired : nullptr);
                    if (success) published = state.desired.has_value();
                }
                if (!success) {
                    pipe.Reset();
                    connected_id.clear();
                    published = false;
                    next_probe = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
                } else {
                    next_probe = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(15);
                }
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
    std::uint64_t revision_{};
    std::thread worker_;
};

DiscordPresence::DiscordPresence() : impl_(std::make_unique<Impl>()) {}
DiscordPresence::~DiscordPresence() = default;

void DiscordPresence::Configure(bool enabled, std::wstring application_id) {
    impl_->Configure(enabled, std::move(application_id));
}

void DiscordPresence::Update(DiscordTrackPresence presence) {
    impl_->Update(std::move(presence));
}

void DiscordPresence::Clear() { impl_->Clear(); }

} // namespace ttplayer::integrations
