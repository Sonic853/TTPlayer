#include "ttplayer/audio/midi_player.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ttplayer::audio {

bool IsMidiPath(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".mid") == 0 ||
           _wcsicmp(extension.c_str(), L".midi") == 0 ||
           _wcsicmp(extension.c_str(), L".rmi") == 0;
}

WAVEFORMATEX MidiReaderFormat() noexcept {
    return {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
}

long MidiVolumeLevel(float volume) noexcept {
    // 004E2C72 truncates the logarithmic amplitude to hundredths of a dB.
    if (!(volume > 0.0F)) return -10000;
    return static_cast<long>(std::clamp(
        2000.0 * std::log10(std::min<double>(volume, 1.0)), -10000.0, 0.0));
}

long MidiBalanceLevel(int balance) noexcept {
    // 004E2CCE attenuates the opposite channel using remaining amplitude.
    balance = std::clamp(balance, -100, 100);
    if (balance == 100) return 10000;
    if (balance == -100) return -10000;
    const double remaining = (100 - std::abs(balance)) / 100.0;
    const long attenuation = static_cast<long>(2000.0 * std::log10(remaining));
    return balance > 0 ? -attenuation : attenuation;
}

MidiPlayer::~MidiPlayer() {
    if (control_) control_->Stop();
}

HRESULT MidiPlayer::Open(const std::filesystem::path& path) {
    // RenderFile adds to an existing graph; never reuse it for another file.
    if (control_) control_->Stop();
    events_.Reset();
    audio_.Reset();
    seeking_.Reset();
    control_.Reset();
    graph_.Reset();
    duration_ = {};
    HRESULT result = CoCreateInstance(CLSID_FilterGraph, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(graph_.GetAddressOf()));
    if (FAILED(result)) return result;
    result = graph_->RenderFile(path.c_str(), nullptr);
    if (FAILED(result)) return result;
    result = graph_.As(&control_);
    if (FAILED(result)) return result;
    graph_.As(&seeking_);
    graph_.As(&audio_);
    graph_.As(&events_);
    LONGLONG duration{};
    if (seeking_ && SUCCEEDED(seeking_->GetDuration(&duration)))
        duration_ = std::chrono::milliseconds(std::max<LONGLONG>(0, duration / 10000));
    return S_OK;
}

HRESULT MidiPlayer::Run() {
    if (!control_) return E_NOINTERFACE;
    OAFilterState state{};
    if (SUCCEEDED(control_->GetState(100, &state)) && state == State_Running)
        return S_OK;
    return control_->Run();
}

HRESULT MidiPlayer::Pause() {
    if (!control_) return E_NOINTERFACE;
    OAFilterState state{};
    if (SUCCEEDED(control_->GetState(100, &state)) && state == State_Paused)
        return S_OK;
    return control_->Pause();
}

HRESULT MidiPlayer::Stop() {
    if (!control_) return E_NOINTERFACE;
    const HRESULT result = control_->Stop();
    if (SUCCEEDED(result)) static_cast<void>(Seek(std::chrono::milliseconds::zero()));
    return result;
}

HRESULT MidiPlayer::Seek(std::chrono::milliseconds position) {
    if (!seeking_) return E_NOINTERFACE;
    if (position.count() < 0 || position.count() >
        std::numeric_limits<LONGLONG>::max() / 10000) return E_INVALIDARG;
    LONGLONG target = position.count() * 10000;
    LONGLONG first{}, last{};
    if (SUCCEEDED(seeking_->GetAvailable(&first, &last)) &&
        (target < first || target > last)) return E_INVALIDARG;
    return seeking_->SetPositions(&target, AM_SEEKING_AbsolutePositioning,
                                  nullptr, AM_SEEKING_NoPositioning);
}

HRESULT MidiPlayer::Position(std::chrono::milliseconds& position) const {
    if (!seeking_) return E_NOINTERFACE;
    LONGLONG current{};
    const HRESULT result = seeking_->GetCurrentPosition(&current);
    if (SUCCEEDED(result)) position = std::chrono::milliseconds(current / 10000);
    return result;
}

HRESULT MidiPlayer::SetVolume(float volume) {
    return audio_ ? audio_->put_Volume(MidiVolumeLevel(volume)) : E_NOINTERFACE;
}

HRESULT MidiPlayer::SetBalance(int balance) {
    return audio_ ? audio_->put_Balance(MidiBalanceLevel(balance)) : E_NOINTERFACE;
}

HRESULT MidiPlayer::PollEvents(bool& complete) {
    complete = false;
    if (!events_) return S_OK;
    long code{};
    LONG_PTR first{}, second{};
    HRESULT failure = S_OK;
    while (events_->GetEvent(&code, &first, &second, 0) == S_OK) {
        if (code == EC_COMPLETE) complete = true;
        if (code == EC_ERRORABORT) failure = FAILED(static_cast<HRESULT>(first))
            ? static_cast<HRESULT>(first) : E_FAIL;
        events_->FreeEventParams(code, first, second);
    }
    return failure;
}

} // namespace ttplayer::audio
