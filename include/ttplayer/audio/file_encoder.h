#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <windows.h>
#include <mmreg.h>

namespace ttplayer::audio {

struct LameEncoderOptions {
    int mode{}; // 0 CBR, 1 VBR, 2 ABR
    int bitrate{192};
    int quality{2};
};

// Optional compatibility encoder: the DLL must be adjacent to the running
// executable. No PATH/CWD probing and no external lame.exe process.
[[nodiscard]] std::filesystem::path LameEncoderPath();
[[nodiscard]] bool LameEncoderAvailable();

class FileEncoder {
public:
    FileEncoder();
    ~FileEncoder();
    HRESULT Open(const std::filesystem::path& path,
                 const WAVEFORMATEX& format, bool lame,
                 const LameEncoderOptions& options = {});
    HRESULT Write(std::span<const std::byte> pcm);
    HRESULT Finalize();
    FileEncoder(const FileEncoder&) = delete;
    FileEncoder& operator=(const FileEncoder&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttplayer::audio
