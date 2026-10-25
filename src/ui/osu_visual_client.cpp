#include "osu_visual_client.h"
#include "ttplayer/settings/settings.h"
#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/archive_password.h"
#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/ui/cover_image.h"
#include "../audio/osu_visual_query.h"
#include <ttpcomm/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <objbase.h>

namespace ttplayer::ui::detail {
HRESULT LoadOsuLogoImage(const std::filesystem::path& path,CoverImage& output) noexcept {
    output={};
    try {
        const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
        struct Close {HANDLE file;~Close(){CloseHandle(file);}} close{file};
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file,&size))return HRESULT_FROM_WIN32(GetLastError());
        if(size.QuadPart<=0 || size.QuadPart>64LL*1024*1024)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
        DWORD count{};
        if(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if(count!=bytes.size())return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        output=DecodeCoverImage(bytes,2048);
        return output.bitmap?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    } catch(...) {return E_OUTOFMEMORY;}
}

struct OsuVisualClient::Impl {
    HMODULE module{};
    TtpOsuVisualApi api{};
    void* instance{};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::atomic<uint64_t> generation{};
    uint64_t delivered{};
    uint64_t source_revision{},logo_revision{},delivered_source{},delivered_logo{};
    std::filesystem::path source;
    std::vector<unsigned char> art;
    int logo_kind{-1};
    std::filesystem::path logo_path;
    uint64_t logo_stamp{};
    ttpcomm::client::ModuleReference comm;
    std::jthread worker;
    bool attempted{};
    HRESULT error{S_OK};
    HWND pointer_window{};
    RECT pointer_bounds{};
    bool pointer_override{};

    ~Impl() {
        worker.request_stop();
        ++generation;
        wake.notify_all();
        if(worker.joinable())worker.join();
        // The renderer's lifetime is owned by the player's UI thread.
        if(instance){api.detach(instance);api.destroy(instance);}
        if(module)FreeLibrary(module);
    }
    bool Ensure() {
        if(instance)return true;
        if(attempted)return false;
        attempted=true;
        wchar_t exe[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,exe,32768);
        if(!length || length>=32768){error=E_FAIL;return false;}
        const auto path=std::filesystem::path(exe).parent_path()/L"AddIn"/L"ttp_osu.dll";
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module){error=HRESULT_FROM_WIN32(GetLastError());return false;}
        if(!audio::detail::QueryOsuVisualApi(module,api)) {
            error=E_NOINTERFACE;return false;
        }
        error=api.create(&instance);
        if(FAILED(error) || !instance)return false;
        worker=std::jthread([this](std::stop_token stop){LoadAssets(stop);});
        return true;
    }
    void LoadAssets(std::stop_token stop) {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        while(!stop.stop_requested()) {
            std::unique_lock lock(mutex);
            wake.wait(lock,stop,[&]{return generation.load()!=delivered;});
            if(stop.stop_requested())break;
            const auto version=generation.load();
            delivered=version;
            const auto source_version=source_revision,logo_version=logo_revision;
            const bool source_changed=source_version!=delivered_source;
            const bool logo_changed=logo_version!=delivered_logo;
            const auto path=source;
            const auto encoded=art;
            const auto wanted_logo=logo_kind;
            const auto custom_logo_path=logo_path;
            ttpcomm::client::ModuleReference module_ref(comm.get());
            lock.unlock();
            std::vector<unsigned char> timing;
            CoverImage image{},custom_logo{};
            const auto canceled=[&]{return stop.stop_requested() || generation.load()!=version;};
            try {
                const audio::ScopedArchiveBackgroundRead background_read;
                audio::ArchiveMemberPath member;
                if(source_changed && audio::ParseArchiveMemberPath(path.native(),member) && audio::IsOszArchive(member.archive)) {
                    const auto selected=audio::ResolveOszMedia(member.archive,module_ref.get(),canceled,{},member.member);
                    if(!selected.empty() && !selected.front().beatmap.empty())
                        timing=audio::ReadArchiveMember({member.archive,selected.front().beatmap,audio::ArchiveKind::zip},module_ref.get(),canceled);
                }
                if(source_changed && !canceled() && !encoded.empty())image=DecodeCoverImage(encoded,2048);
                if(logo_changed && !canceled() && wanted_logo==2 && api.logo)
                    LoadOsuLogoImage(custom_logo_path,custom_logo);
            } catch(...) {
                // A corrupt optional timing/background must not stop playback.
                timing.clear();
            }
            lock.lock();
            if(!canceled() && instance) {
                if(logo_changed && api.logo && wanted_logo!=2)
                    api.logo(instance,static_cast<uint32_t>(wanted_logo),nullptr);
                if(logo_changed && custom_logo.bitmap && api.logo) {
                    DIBSECTION logo_dib{};
                    if(GetObjectW(custom_logo.bitmap,sizeof(logo_dib),&logo_dib)==sizeof(logo_dib) && logo_dib.dsBm.bmBits) {
                        const TtpOsuVisualImage pixels{sizeof(pixels),static_cast<uint32_t>(custom_logo.size.cx),
                            static_cast<uint32_t>(custom_logo.size.cy),static_cast<uint32_t>(logo_dib.dsBm.bmWidthBytes),
                            static_cast<const uint8_t*>(logo_dib.dsBm.bmBits)};
                        api.logo(instance,2,&pixels);
                    }
                }
                // Logo requests share cancellation with track assets, but must
                // not reapply timing (which resets beat/particle state) or art.
                if(logo_changed)delivered_logo=logo_version;
                if(source_changed) {
                if(FAILED(api.timing(instance,timing.data(),static_cast<uint32_t>(timing.size()))))
                    api.timing(instance,nullptr,0);
                DIBSECTION dib{};
                if(image.bitmap && GetObjectW(image.bitmap,sizeof(dib),&dib)==sizeof(dib) && dib.dsBm.bmBits) {
                    const TtpOsuVisualImage pixels{sizeof(pixels),static_cast<uint32_t>(image.size.cx),
                        static_cast<uint32_t>(image.size.cy),static_cast<uint32_t>(dib.dsBm.bmWidthBytes),
                        static_cast<const uint8_t*>(dib.dsBm.bmBits)};
                    api.image(instance,&pixels);
                } else api.image(instance,nullptr);
                delivered_source=source_version;
                }
            }
            lock.unlock();
            if(image.bitmap)DeleteObject(image.bitmap);
            if(custom_logo.bitmap)DeleteObject(custom_logo.bitmap);
        }
        if(SUCCEEDED(com))CoUninitialize();
    }
};
OsuVisualClient::OsuVisualClient():impl_(std::make_unique<Impl>()){}
OsuVisualClient::~OsuVisualClient()=default;
void OsuVisualClient::Configure(const settings::VisualSettings& settings) {
    std::scoped_lock lock(impl_->mutex);
    impl_->pointer_window=nullptr;impl_->pointer_bounds={};impl_->pointer_override=false;
    if(!impl_->Ensure())return;
    const TtpOsuVisualOptions options{sizeof(options),static_cast<uint32_t>(settings.osu_flags),
        0x5454504fU,settings.osu_accent,settings.osu_background,static_cast<double>(settings.osu_bpm)};
    impl_->api.configure(impl_->instance,&options);
    const int kind=std::clamp(settings.osu_logo,0,2);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    uint64_t stamp{};
    if(kind==2 && GetFileAttributesExW(settings.osu_logo_path.c_str(),GetFileExInfoStandard,&attributes))
        stamp=(static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime)<<32) |
            attributes.ftLastWriteTime.dwLowDateTime;
    if(impl_->api.logo && (impl_->logo_kind!=kind || impl_->logo_path!=settings.osu_logo_path ||
                           impl_->logo_stamp!=stamp)) {
        impl_->logo_kind=kind;impl_->logo_path=settings.osu_logo_path;impl_->logo_stamp=stamp;
        ++impl_->logo_revision;
        ++impl_->generation;impl_->wake.notify_all();
    }
}
void OsuVisualClient::Source(const std::filesystem::path& path,
    std::span<const unsigned char> art, HMODULE comm) {
    std::scoped_lock lock(impl_->mutex);
    if(!impl_->instance)return;
    if(impl_->source==path && impl_->art.size()==art.size() &&
       std::equal(art.begin(),art.end(),impl_->art.begin()))return;
    const bool changed=impl_->source!=path;
    impl_->source=path;impl_->art.assign(art.begin(),art.end());
    impl_->comm=ttpcomm::client::ModuleReference(comm);
    if(changed){impl_->api.timing(impl_->instance,nullptr,0);impl_->api.image(impl_->instance,nullptr);}
    ++impl_->source_revision;
    ++impl_->generation;
    impl_->wake.notify_all();
}
void OsuVisualClient::Viewport(HWND window,const RECT& bounds) {
    std::scoped_lock lock(impl_->mutex);
    impl_->pointer_window=window;impl_->pointer_bounds=bounds;impl_->pointer_override=true;
}
void OsuVisualClient::Update(const audio::ExtendedVisualizationSamples& samples,
    const audio::PlaybackClockSnapshot& clock, HWND window, float volume) {
    std::scoped_lock lock(impl_->mutex);
    if(!impl_->instance)return;
    TtpOsuVisualFrame frame{};frame.size=sizeof(frame);
    frame.playback=static_cast<uint32_t>(clock.state);
    frame.now_ms=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    frame.position_ms=static_cast<double>(clock.position.count());
    frame.timeline=clock.timeline_revision;frame.revision=samples.revision;
    frame.sample_rate=samples.sample_rate;
    frame.count=!clock.seek_pending && samples.timeline_revision==clock.timeline_revision
        ?static_cast<uint32_t>(samples.count):0;
    frame.left=samples.left.data();frame.right=samples.right.data();frame.volume=std::clamp(volume,0.0f,1.0f);
    frame.pcm_cursor_offset=static_cast<uint32_t>(std::min<uint64_t>(frame.count,
        samples.playback_frame>samples.first_frame?samples.playback_frame-samples.first_frame:0));
    if(frame.count && clock.state==audio::PlaybackState::playing && frame.sample_rate)
        frame.position_ms=static_cast<double>(samples.playback_frame)*1000.0/frame.sample_rate;
    frame.pointer_x=frame.pointer_y=0.5f;
    POINT point{};RECT bounds{};
    if(impl_->pointer_override) {
        window=impl_->pointer_window;bounds=impl_->pointer_bounds;
    } else if(window) GetClientRect(window,&bounds);
    if(window && GetCursorPos(&point) && ScreenToClient(window,&point) &&
       bounds.right>bounds.left && bounds.bottom>bounds.top) {
        frame.pointer_inside=PtInRect(&bounds,point)?1:0;
        if(frame.pointer_inside) {
            frame.pointer_x=static_cast<float>(point.x-bounds.left)/(bounds.right-bounds.left);
            frame.pointer_y=static_cast<float>(point.y-bounds.top)/(bounds.bottom-bounds.top);
        }
    }
    impl_->api.update(impl_->instance,&frame);
}
void OsuVisualClient::Paint(HDC dc,const RECT& bounds,bool gpu) {
    std::scoped_lock lock(impl_->mutex);
    if(!impl_->instance) {
        const int saved=SaveDC(dc);
        SetDCBrushColor(dc,RGB(24,20,32));FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(230,230,230));
        RECT text=bounds;DrawTextW(dc,L"请更新 AddIn\\ttp_osu.dll 以使用节拍星环",-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(saved)RestoreDC(dc,saved);return;
    }
    if(gpu) {
        const HWND window=WindowFromDC(dc);RECT client{};
        if(window && GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId() &&
           GetClientRect(window,&client) && EqualRect(&client,&bounds) &&
           impl_->api.present(impl_->instance,window)==S_OK)return;
    }
    impl_->api.paint(impl_->instance,dc,&bounds);
}
void OsuVisualClient::Detach() {
    std::scoped_lock lock(impl_->mutex);
    if(impl_->instance)impl_->api.detach(impl_->instance);
}
void OsuVisualClient::Release() {
    if(!impl_->attempted)return;
    // Do not hold Impl::mutex while joining: the asset worker needs it to
    // finish/cancel delivery. VisualRuntime serializes all adapter access.
    // Reset revisions as well, so re-entry loads the same track/logo anew.
    impl_=std::make_unique<Impl>();
}
}
