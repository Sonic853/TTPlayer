#include "ttplayer/platform/optional_windows_api.h"
#include "ttplayer/audio/audio_engine.h"
#include "ttplayer/audio/legacy_windows_source.h"
#include "ttplayer/plugins/plugin_manager.h"
#include "ttplayer/ui/playlist_transforms.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace ttplayer;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Decode(const fs::path& path) {
    // Production source selection must choose the XP decoder when MF is absent.
    auto reader = audio::CreateDecodedAudioSource(path, 0, nullptr, nullptr);
    if (!reader->Open(path, {})) {
        std::wcerr << reader->Error() << L'\n';
        throw std::runtime_error("Fallback decoder could not open fixture");
    }
    Require(reader->OutputFormat().nBlockAlign != 0, "Missing PCM format");
    std::vector<std::byte> bytes;
    size_t total{};
    bool end{};
    for (int iteration = 0; !end && iteration < 10000; ++iteration) {
        Require(reader->Read(4096, bytes, end), "Fallback decode failed");
        Require(bytes.size() % reader->OutputFormat().nBlockAlign == 0, "Partial PCM frame");
        total += bytes.size();
    }
    Require(end && total > 0, "Missing samples or EOF");
    Require(reader->Seek(std::chrono::milliseconds(0)), "Fallback seek failed");
    Require(reader->Read(4096, bytes, end) && !bytes.empty(), "No audio after seek");
    std::cout << "Decoded and sought " << total << " PCM bytes\n";
}
}

int wmain(int argc, wchar_t** argv) {
    const auto directory = fs::temp_directory_path() /
        (L"ttplayer-legacy-" + std::to_wstring(GetCurrentProcessId()));
    try {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM initialization failed");
        Require(!platform::HasMediaFoundation(), "Test must force absent Media Foundation");
        Require(FAILED(platform::MFStartup(MF_VERSION, MFSTARTUP_LITE)), "MF unexpectedly present");
        IMFAttributes* attributes = reinterpret_cast<IMFAttributes*>(1);
        Require(FAILED(platform::MFCreateAttributes(&attributes, 1)) && !attributes,
                "Absent API left a stale COM pointer");
        IPropertyStore* store = reinterpret_cast<IPropertyStore*>(1);
        Require(FAILED(platform::SHGetPropertyStoreFromParsingName(L"missing", nullptr,
            GPS_DEFAULT, IID_PPV_ARGS(&store))) && !store, "Missing Shell property store was not handled");
        const BYTE input[]{1, 2, 3, 4};
        IStream* stream = platform::SHCreateMemStream(input, sizeof(input));
        Require(stream != nullptr, "XP memory stream failed");
        BYTE read[4]{}; ULONG count{};
        const HRESULT result = stream->Read(read, sizeof(read), &count);
        stream->Release();
        Require(SUCCEEDED(result) && count == 4 && read[0] == 1 && read[3] == 4,
                "Memory stream content or initial position changed");
        fs::create_directories(directory);
        const auto wave = directory / L"兼容测试.wav";
        {
            std::ofstream output(wave, std::ios::binary);
            const char header[] = "RIFF\x24\x7d\x00\x00WAVEfmt \x10\x00\x00\x00"
                "\x01\x00\x01\x00\x80\x3e\x00\x00\x00\x7d\x00\x00\x02\x00\x10\x00"
                "data\x00\x7d\x00\x00";
            output.write(header, 44);
            const std::string silence(32000, '\0');
            output.write(silence.data(), silence.size());
        }
        const auto copied = directory / L"复制.wav";
        fs::copy_file(wave, copied);
        Require(fs::file_size(copied) == 32044, "filesystem create/copy failed");
        Decode(copied);
        for (int index = 1; index < argc; ++index) Decode(argv[index]);
        plugins::PluginManager plugins;
        playlist::Track track;
        track.path = wave;
        settings::ConvertSettings conversion;
        const auto converted = ui::ConvertPlaylistTrack(plugins, track,
            ui::kWaveConversionEncoder, directory / L"converted.wav", conversion, nullptr);
        if (FAILED(converted.result)) std::wcerr << converted.diagnostic << L'\n';
        Require(SUCCEEDED(converted.result) && converted.pcm_bytes > 0,
                "Native conversion was blocked by missing Media Foundation");
        // A RIFF chunk claiming bytes outside the file must fail before decode.
        const auto truncated = directory / L"truncated.wav";
        fs::copy_file(wave, truncated);
        fs::resize_file(truncated, 45);
        {
            const auto bad_reader = audio::CreateDecodedAudioSource(truncated, 0, nullptr, nullptr);
            Require(!bad_reader->Open(truncated, {}), "Truncated WAV was accepted");
        }
        // Exercise the actual playback worker with no MF startup. A deliberately
        // invalid device avoids audio output and must reach device validation.
        audio::AudioEngine engine;
        audio::PlaybackOptions options;
        options.device_type = L"invalid-device-key";
        engine.Configure(options);
        Require(!engine.Play(wave), "Invalid output unexpectedly opened");
        Require(engine.LastError().find(L"DeviceType") != std::wstring::npos,
                "Missing MF prevented the playback worker reaching native output selection");
        engine.Stop();
        fs::remove_all(directory);
        CoUninitialize();
        std::cout << "Legacy component fallback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
