#pragma once
#include "ttplayer/audio/audio_engine.h"
#include <objidl.h>

namespace ttplayer::audio {
// Recovered 004C4548 / 004C4809 / 004C48D1 boundary: a seekable HTTP
// byte stream, independent of the codec and the output device. The download
// worker follows 004D8570/004EA90B: prefetch continues without decoder reads;
// consumers wait for verified cache ranges. ICY metadata follows consumption.
struct HttpAudioStream : IStream {
    virtual ~HttpAudioStream() = default;
    virtual int DownloadedPercent() const noexcept = 0;
    virtual HRESULT LastError() const noexcept = 0;
    virtual AudioMetadata Metadata() const = 0;
    virtual bool Sequential() const noexcept = 0;
    virtual bool CanPersist() const noexcept = 0;
    virtual HRESULT BufferToEnd(uint64_t limit) noexcept = 0;
};
enum class HttpStreamUse { playback, background };
// Optional music-window resolver. Called on the HTTP producer worker, never
// while holding a registry/cache lock. Reserved sources must not reach DNS.
using PersonalMediaResolver = std::function<HRESULT(const std::wstring&,
    const std::function<bool()>&, std::wstring&)>;
void SetPersonalMediaResolver(PersonalMediaResolver resolver);
struct ResolvedMediaSource { std::wstring url, authorization; };
using PersonalMediaResolverEx = std::function<HRESULT(const std::wstring&,
    const std::function<bool()>&, ResolvedMediaSource&)>;
void SetPersonalMediaResolverEx(PersonalMediaResolverEx resolver);
// Matching live finite-file requests share a producer, cache and entity snapshot.
// Each returned stream has its own cursor/cancellation. Releasing one consumer
// never cancels another. Unknown-length/ICY streams remain private.
HRESULT OpenHttpAudioStream(const std::wstring& url, const PlaybackOptions& options,
                            HttpAudioStream** stream,
                            HttpStreamUse use = HttpStreamUse::playback) noexcept;
}
