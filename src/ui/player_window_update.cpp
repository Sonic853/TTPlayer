#include "ttplayer/ui/player_window.h"
#include "ttplayer/update/update.h"
#include "ttplayer/core/text.h"
#include "player_window_internal.h"
#include "update_controls.h"
#include "update_notice.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace ttplayer::ui {
namespace {
int Today() {
    FILETIME time{};GetSystemTimeAsFileTime(&time);
    const uint64_t ticks=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    return static_cast<int>(ticks/(10000000ULL*60*60*24));
}
}
struct PlayerWindow::UpdateCheckState {
    std::atomic<bool> canceled{},done{},manual{};
    int source{};
    std::optional<update::Release> release;
    std::wstring error;
};
void PlayerWindow::CheckForUpdates(bool manual) {
    if(close_after_skin_window_fade_) return;
    if(update_check_) {
        if(update_check_->source==settings_.general.update_source) {if(manual) update_check_->manual=true;return;}
        update_check_->canceled=true;update_check_.reset();
    }
    const int day=Today();
    if(!manual && (settings_.general.check_update_days<=0 ||
        GetTickCount64()<update_retry_tick_ ||
        (settings_.general.last_checked_day<=day && day-settings_.general.last_checked_day<settings_.general.check_update_days))) return;
    update_retry_tick_=GetTickCount64()+60ULL*60*1000;
    auto state=std::make_shared<UpdateCheckState>();state->source=settings_.general.update_source;state->manual=manual;
    update_check_=state;
    const auto runtime=update::ExecutablePath().parent_path();const auto network=settings_.network;
    if(options_pages_.size()>1 && options_pages_[1]) SetDlgItemTextW(options_pages_[1],detail::kUpdateStatus,L"正在检查新版本…");
    try { std::thread([state,runtime,network] {
        try {
            update::Http http(runtime,network);
            if(update::IsXp() && !http.HasProvider()) state->error=update::kMissingHttps;
            else state->release=update::Check(http,state->source==1 ? update::Source::github : update::Source::gitee,[&]{return state->canceled.load();});
        } catch(const std::exception& error){state->error=update::ErrorText(error);}
        state->done.store(true,std::memory_order_release);
    }).detach(); } catch(const std::exception& error) {
        state->error=update::ErrorText(error);state->done.store(true,std::memory_order_release);
    }
}
void PlayerWindow::PollUpdateCheck() {
    if(!update_check_) {CheckForUpdates(false);return;}
    if(!update_check_->done.load(std::memory_order_acquire)) return;
    const auto state=std::move(update_check_);
    if(state->canceled || state->source!=settings_.general.update_source || close_after_skin_window_fade_) return;
    std::wstring status;
    if(!state->error.empty()) {
        status=state->error==update::kMissingHttps ? state->error : L"检查更新失败："+state->error;
        if(state->manual) MessageBoxW(options_window_ ? options_window_ : window_,status.c_str(),L"软件更新",MB_OK|MB_ICONINFORMATION);
    } else {
        settings_.general.last_checked_day=Today();
        try {
            const auto local=update::FileVersion(update::ExecutablePath());
            if(state->release && state->release->version>local) {
                status=L"发现新版本："+state->release->version.Text();
                const auto identity=std::to_wstring(state->source)+L":"+state->release->version.Text();
                if(state->manual || settings_.general.last_notified_update!=identity) {
                    if(update_notice_ && IsWindow(update_notice_)) DestroyWindow(update_notice_);
                    update_notice_=ShowUpdateNotice(window_);
                    if(update_notice_) settings_.general.last_notified_update=identity;
                }
            } else {
                status=state->release ? L"当前已是最新版本。" : L"尚无可用的完整发行包，请前往发布页查看。";
                if(state->manual) MessageBoxW(options_window_ ? options_window_ : window_,status.c_str(),L"软件更新",MB_OK|MB_ICONINFORMATION);
            }
            settings::SaveWindowState(settings_.source_path,settings_);
        } catch(const std::exception& error) {status=L"检查更新失败："+update::ErrorText(error);}
    }
    if(options_pages_.size()>1 && options_pages_[1]) SetDlgItemTextW(options_pages_[1],detail::kUpdateStatus,status.c_str());
}
void PlayerWindow::CancelUpdateCheck() {
    if(update_check_) update_check_->canceled=true;
    update_check_.reset();
    if(update_notice_ && IsWindow(update_notice_)) DestroyWindow(update_notice_);
    update_notice_=nullptr;
}
void PlayerWindow::OpenUpdater() {
    try {
        FlushDeferredOptionsRuntime();
        settings::SaveWindowState(settings_.source_path,settings_);
        const auto runtime=update::ExecutablePath().parent_path(),file=runtime/L"TTPUpdater.exe";
        if(!std::filesystem::is_regular_file(file)) throw std::runtime_error("TTPUpdater.exe is missing; extract the complete release ZIP");
        std::wstring command=L"\""+file.wstring()+L"\" --player-window "+std::to_wstring(reinterpret_cast<uintptr_t>(window_));
        STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        if(!CreateProcessW(file.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,runtime.c_str(),&si,&pi))
            throw std::runtime_error("Cannot start TTPUpdater.exe ("+std::to_string(GetLastError())+")");
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    } catch(const std::exception& error) {MessageBoxW(options_window_ ? options_window_ : window_,update::ErrorText(error).c_str(),L"软件更新",MB_OK|MB_ICONERROR);}
}
}
