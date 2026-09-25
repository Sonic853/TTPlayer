#include "ttplayer/ui/system_media_controls.h"
#include "ttplayer/platform/windows_features.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/core/text.h"
#include "ttplayer/playlist/playlist.h"
#include "ttplayer/ui/taskbar_playback.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <roapi.h>
#include <shcore.h>
#include <systemmediatransportcontrolsinterop.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

namespace ttplayer::ui {
namespace {
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;
using Command = SystemMediaControls::Command;

std::wstring TagUtf8(std::string_view value) {
    try { return core::Utf8ToWide(value); }
    catch (const std::runtime_error&) { return {}; }
}

std::wstring TagText(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    return std::wstring(value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1));
}

std::wstring TagKey(std::wstring_view value) {
    std::wstring key;
    for (wchar_t ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) key += ch;
    }
    return key;
}

struct ExtraTags {
    std::optional<std::wstring> album_artist;
    bool has_genres{};
    std::vector<std::wstring> genres;

    void Read(std::wstring_view name, std::wstring_view value) {
        const auto key = TagKey(name);
        if (key == L"albumartist" || key == L"wmalbumartist" || key == L"tpe2" || key == L"tp2") {
            if (!album_artist || album_artist->empty()) album_artist = TagText(value);
        } else if (key == L"genre" || key == L"genres" || key == L"wmgenre" || key == L"tcon" || key == L"tco") {
            has_genres = true;
            size_t start{};
            // Preserve names like Pop/Rock and R&B. Readers may return repeated
            // fields, NUL-separated ID3v2.4 values, or a joined Shell string.
            for (size_t end = 0; end <= value.size(); ++end) {
                if (end != value.size() && value[end] != L';' && value[end] != L',' &&
                    value[end] != L'\0' && value[end] != L'\r' && value[end] != L'\n') continue;
                auto genre = TagText(value.substr(start, end - start));
                start = end + 1;
                if (genre.empty()) continue;
                const bool duplicate = std::any_of(genres.begin(), genres.end(), [&](const auto& existing) {
                    return CompareStringOrdinal(existing.c_str(), -1, genre.c_str(), -1, TRUE) == CSTR_EQUAL;
                });
                if (!duplicate) genres.push_back(std::move(genre));
            }
        }
    }
};

RandomAccessStreamReference MakeThumbnail(HBITMAP cover) {
    if (!cover) return nullptr;
    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::check_hresult(factory->CreateBitmapFromHBITMAP(cover, nullptr,
        WICBitmapUsePremultipliedAlpha, bitmap.put()));
    winrt::com_ptr<IStream> stream;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    UINT width{}, height{};
    winrt::check_hresult(bitmap->GetSize(&width, &height));
    winrt::check_hresult(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    winrt::check_hresult(frame->WriteSource(bitmap.get(), nullptr));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
    winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    IRandomAccessStream random{nullptr};
    winrt::check_hresult(CreateRandomAccessStreamOverStream(stream.get(), BSOS_DEFAULT,
        winrt::guid_of<IRandomAccessStream>(), winrt::put_abi(random)));
    return RandomAccessStreamReference::CreateFromStream(random);
}

struct Request { Command command; int64_t position_ms; uint64_t source; };
struct Receiver {
    std::mutex mutex;
    HWND window{};
    uint64_t source{};
    std::deque<Request> requests;

    void Post(Command command, int64_t position = 0) {
        std::scoped_lock lock(mutex);
        if (!window) return;
        // Bound repeated hardware-key input while a modal dialog is open.
        if (requests.size() >= 64) return;
        requests.push_back({command, position, source});
        if (!PostMessageW(window, SystemMediaControls::RequestMessage(), 0, 0))
            requests.pop_back();
    }
    void NewSource() {
        std::scoped_lock lock(mutex);
        ++source;
        requests.clear();
    }
};
} // namespace

SystemMediaMetadata BuildSystemMediaMetadata(
    const playlist::Track& track, const audio::AudioMetadata& metadata) {
    SystemMediaMetadata result;
    result.title = metadata.title.empty() ? (track.title.empty()
        ? track.path.stem().wstring() : TagUtf8(track.title)) : metadata.title;
    result.artist = metadata.artist.empty() ? TagUtf8(track.artist) : metadata.artist;
    result.album = metadata.album.empty() ? TagUtf8(track.album) : metadata.album;
    ExtraTags decoded, cached;
    for (const auto& [name, value] : metadata.entries) decoded.Read(name, value);
    if (!decoded.album_artist || !decoded.has_genres) {
        for (const auto& [name, value] : track.metadata)
            cached.Read(TagUtf8(name), TagUtf8(value));
    }
    // A present-but-empty reader field overrides old playlist tags, e.g. after
    // deleting a tag. Only an absent field may fall back to the cached entry.
    result.album_artist = decoded.album_artist.value_or(cached.album_artist.value_or(L""));
    if (result.album_artist.empty()) result.album_artist = TagText(result.artist);
    result.genres = decoded.has_genres ? std::move(decoded.genres) : std::move(cached.genres);
    return result;
}

struct SystemMediaControls::Impl {
    bool apartment{};
    SystemMediaTransportControls controls{nullptr};
    winrt::event_token button_token{}, position_token{};
    bool button_registered{}, position_registered{}, has_source{};
    std::shared_ptr<Receiver> receiver = std::make_shared<Receiver>();
    std::optional<audio::PlaybackClockSnapshot> last_clock;
    std::optional<TaskbarPlaybackState> last_buttons;
    bool last_closing{};
    std::chrono::steady_clock::time_point last_timeline{};

    ~Impl() {
        {
            std::scoped_lock lock(receiver->mutex);
            receiver->window = nullptr;
            receiver->requests.clear();
        }
        if (controls) {
            if (button_registered) controls.ButtonPressed(button_token);
            if (position_registered) controls.PlaybackPositionChangeRequested(position_token);
            try { controls.IsEnabled(false); } catch (...) {}
            try { controls.DisplayUpdater().ClearAll(); controls.DisplayUpdater().Update(); } catch (...) {}
            controls = nullptr;
        }
        if (apartment) RoUninitialize();
    }

    void Initialize(HWND window) {
        winrt::check_hresult(RoInitialize(RO_INIT_SINGLETHREADED));
        apartment = true;
        const auto interop = winrt::get_activation_factory<SystemMediaTransportControls,
            ISystemMediaTransportControlsInterop>();
        winrt::check_hresult(interop->GetForWindow(window,
            winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(controls)));
        controls.IsEnabled(false);
        receiver->window = window;
        // A callback never captures PlayerWindow or this. Reset revokes the HWND
        // under the same lock used by Post, including callbacks already in flight.
        const auto target = receiver;
        button_token = controls.ButtonPressed([target](const auto&, const SystemMediaTransportControlsButtonPressedEventArgs& args) noexcept {
            try {
                switch (args.Button()) {
                case SystemMediaTransportControlsButton::Play: target->Post(Command::play); break;
                case SystemMediaTransportControlsButton::Pause: target->Post(Command::pause); break;
                case SystemMediaTransportControlsButton::Stop: target->Post(Command::stop); break;
                case SystemMediaTransportControlsButton::Previous: target->Post(Command::previous); break;
                case SystemMediaTransportControlsButton::Next: target->Post(Command::next); break;
                default: break;
                }
            } catch (...) {}
        });
        button_registered = true;
        position_token = controls.PlaybackPositionChangeRequested([target](const auto&, const PlaybackPositionChangeRequestedEventArgs& args) noexcept {
            try {
                target->Post(Command::seek,
                    std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition()).count());
            } catch (...) {}
        });
        position_registered = true;
    }
};

SystemMediaControls::SystemMediaControls() = default;
SystemMediaControls::~SystemMediaControls() = default;

UINT SystemMediaControls::RequestMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"TTPlayerRebuild.SMTC.Request.v1");
    return message;
}

void SystemMediaControls::SetSource(HWND window, const SystemMediaMetadata& metadata, HBITMAP cover) noexcept {
    // XP/Win7 never construct WinRT strings, factories or SMTC COM objects.
    // Win10+ still probes the actual factory: stripped/N editions may lack it.
    if (!platform::CurrentWindowsFeatures().SystemMediaControls() ||
        !window || !IsWindow(window) || !RequestMessage()) return;
    try {
        if (!attempted_) {
            attempted_ = true;
            auto candidate = std::make_unique<Impl>();
            candidate->Initialize(window);
            impl_ = std::move(candidate);
        }
        if (!impl_) return;
        auto& self = *impl_;
        self.receiver->NewSource();
        self.has_source = true;
        self.last_clock.reset();
        self.last_buttons.reset();
        const auto updater = self.controls.DisplayUpdater();
        updater.ClearAll();
        updater.Type(MediaPlaybackType::Music);
        const auto music = updater.MusicProperties();
        music.Title(winrt::hstring(metadata.title));
        music.Artist(winrt::hstring(metadata.artist));
        music.AlbumTitle(winrt::hstring(metadata.album));
        music.AlbumArtist(winrt::hstring(metadata.album_artist));
        const auto genres = music.Genres();
        genres.Clear();
        for (const auto& genre : metadata.genres) genres.Append(winrt::hstring(genre));
        // A corrupt artwork must not suppress title or transport controls.
        try { updater.Thumbnail(MakeThumbnail(cover)); } catch (...) { updater.Thumbnail(nullptr); }
        updater.Update();
    } catch (...) { Clear(); }
}

void SystemMediaControls::Clear() noexcept {
    if (!impl_) return;
    auto& self = *impl_;
    if (!self.has_source && !self.last_clock) return;
    self.has_source = false;
    self.receiver->NewSource();
    self.last_clock.reset();
    self.last_buttons.reset();
    try { self.controls.IsEnabled(false); } catch (...) {}
    try {
        self.controls.PlaybackStatus(MediaPlaybackStatus::Closed);
        const auto updater = self.controls.DisplayUpdater();
        updater.ClearAll();
        updater.Update();
    } catch (...) {}
}

void SystemMediaControls::Reset() noexcept { impl_.reset(); attempted_ = false; }

void SystemMediaControls::Update(const audio::PlaybackClockSnapshot& clock,
    const TaskbarPlaybackState& buttons, bool closing) noexcept {
    if (!impl_ || !impl_->has_source) return;
    if (clock.state == audio::PlaybackState::failed) { Clear(); return; }
    try {
        auto& self = *impl_;
        const bool state_changed = !self.last_clock || self.last_clock->state != clock.state;
        const bool buttons_changed = !self.last_buttons || *self.last_buttons != buttons || self.last_closing != closing;
        if (state_changed || buttons_changed) {
            const bool active = clock.state == audio::PlaybackState::playing || clock.state == audio::PlaybackState::paused;
            self.controls.IsPlayEnabled(buttons.play_pause_enabled);
            self.controls.IsPauseEnabled(buttons.play_pause_enabled && active);
            self.controls.IsStopEnabled(buttons.play_pause_enabled && active);
            self.controls.IsPreviousEnabled(buttons.previous_enabled);
            self.controls.IsNextEnabled(buttons.next_enabled);
            MediaPlaybackStatus status = MediaPlaybackStatus::Stopped;
            if (clock.state == audio::PlaybackState::playing) status = MediaPlaybackStatus::Playing;
            else if (clock.state == audio::PlaybackState::paused) status = MediaPlaybackStatus::Paused;
            else if (clock.state == audio::PlaybackState::opening) status = MediaPlaybackStatus::Changing;
            self.controls.PlaybackStatus(status);
            self.controls.IsEnabled(!closing);
        }
        const auto now = std::chrono::steady_clock::now();
        if (state_changed || buttons_changed || !self.last_clock ||
            self.last_clock->timeline_revision != clock.timeline_revision ||
            self.last_clock->duration != clock.duration ||
            self.last_clock->seek_pending != clock.seek_pending ||
            now - self.last_timeline >= std::chrono::seconds(5)) {
            const auto duration = std::max(clock.duration, std::chrono::milliseconds::zero());
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime({});
            timeline.MinSeekTime({});
            timeline.EndTime(duration);
            const bool seekable = !closing && buttons.play_pause_enabled &&
                (clock.state == audio::PlaybackState::playing || clock.state == audio::PlaybackState::paused);
            timeline.MaxSeekTime(seekable ? duration : std::chrono::milliseconds::zero());
            timeline.Position(std::clamp(clock.position, std::chrono::milliseconds::zero(), duration));
            self.controls.UpdateTimelineProperties(timeline);
            self.last_timeline = now;
        }
        self.last_clock = clock;
        self.last_buttons = buttons;
        self.last_closing = closing;
    } catch (...) { /* Optional shell integration must never abort playback. */ }
}

void SystemMediaControls::DispatchPending(const std::function<void(Command, int64_t)>& dispatch) {
    if (!impl_) return;
    const auto receiver = impl_->receiver;
    std::deque<Request> requests;
    {
        std::scoped_lock lock(receiver->mutex);
        requests.swap(receiver->requests);
    }
    for (const auto& request : requests) {
        {
            std::scoped_lock lock(receiver->mutex);
            if (!receiver->window || request.source != receiver->source) continue;
        }
        dispatch(request.command, request.position_ms);
    }
}
} // namespace ttplayer::ui
