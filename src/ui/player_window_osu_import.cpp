#include "player_window_internal.h"
#include "ttplayer/audio/archive_member.h"
#include "ttplayer/audio/osu_import.h"
#include "ttplayer/audio/osu_installation.h"
#include "ttplayer/audio/osz_archive.h"
#include "ttplayer/i18n/i18n.h"
#include <commctrl.h>
#include <mutex>

namespace ttplayer::ui {
using namespace detail;
namespace {
struct DialogTemplate {DLGTEMPLATE dialog;WORD menu,cls,title;};
HWND Control(HWND parent,const wchar_t* type,const wchar_t* text,DWORD style,int id,RECT rect) {
    i18n::MapDialogRect(parent,&rect);
    const auto child=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,
        rect.right-rect.left,rect.bottom-rect.top,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return child;
}
void Center(HWND window) {
    RECT bounds{},owner{};GetWindowRect(window,&bounds);GetWindowRect(GetParent(window),&owner);
    MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromWindow(GetParent(window),MONITOR_DEFAULTTONEAREST),&monitor))return;
    const auto width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    SetWindowPos(window,nullptr,std::max(monitor.rcWork.left,std::min((owner.left+owner.right-width)/2,monitor.rcWork.right-width)),
        std::max(monitor.rcWork.top,std::min((owner.top+owner.bottom-height)/2,monitor.rcWork.bottom-height)),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
struct Confirmation {std::wstring title,question,files;bool keep{true};};
INT_PTR CALLBACK ConfirmProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<Confirmation*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<Confirmation*>(lparam);SetWindowLongPtrW(window,DWLP_USER,lparam);SetWindowTextW(window,state->title.c_str());
        Control(window,L"STATIC",state->question.c_str(),0,100,{10,9,325,25});
        Control(window,L"STATIC",state->files.c_str(),SS_PATHELLIPSIS,101,{10,29,325,59});
        Control(window,L"STATIC",i18n::Literal(L"将复制一份 OSZ 用于导入，原文件不会直接交给游戏。"),0,102,{10,65,325,91});
        Control(window,L"BUTTON",i18n::Literal(L"保留 osz 文件"),WS_TABSTOP|BS_AUTOCHECKBOX,103,{10,96,325,111});
        CheckDlgButton(window,103,BST_CHECKED);
        Control(window,L"STATIC",i18n::Literal(L"取消保留后，仅在确认导入成功且原文件未被播放列表使用时清理。"),0,104,{10,117,325,145});
        const auto okay=Control(window,L"BUTTON",i18n::Literal(L"导入"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,{204,151,260,169});
        Control(window,L"BUTTON",i18n::Literal(L"取消"),WS_TABSTOP,IDCANCEL,{269,151,325,169});
        Center(window);SetFocus(okay);return FALSE;
    }
    if(!state)return FALSE;
    if(message==WM_COMMAND&&(LOWORD(wparam)==IDOK||LOWORD(wparam)==IDCANCEL)) {
        if(LOWORD(wparam)==IDOK)state->keep=IsDlgButtonChecked(window,103)==BST_CHECKED;
        EndDialog(window,LOWORD(wparam));return TRUE;
    }
    if(message==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}return FALSE;
}
struct Item {std::filesystem::path archive;audio::OsuImportResult result;HRESULT status{E_PENDING};};
struct Progress {
    std::atomic_bool canceled{};std::mutex mutex;
    std::wstring title,current;unsigned phase{};uint64_t done{},total{};size_t package{},packages{};bool ready{};
};
const wchar_t* Phase(unsigned value) {
    switch(value){case 1:return i18n::Literal(L"正在复制 OSZ…");case 2:return i18n::Literal(L"正在校验 OSZ 副本…");
        case 3:return i18n::Literal(L"正在检查谱面及资源…");case 4:return i18n::Literal(L"正在核对游戏曲库…");
        case 5:return i18n::Literal(L"已交给游戏，正在等待导入结果…");default:return i18n::Literal(L"正在准备导入…");}
}
INT_PTR CALLBACK ProgressProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<Progress*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<Progress*>(lparam);SetWindowLongPtrW(window,DWLP_USER,lparam);SetWindowTextW(window,state->title.c_str());
        Control(window,L"STATIC",Phase(0),0,100,{10,9,325,25});
        Control(window,L"STATIC",L"",SS_PATHELLIPSIS,101,{10,29,325,46});
        Control(window,PROGRESS_CLASSW,L"",0,102,{10,51,325,63});
        Control(window,L"STATIC",L"",0,103,{10,73,230,89});
        const auto cancel=Control(window,L"BUTTON",i18n::Literal(L"取消"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDCANCEL,{251,71,325,90});
        SendDlgItemMessageW(window,102,PBM_SETRANGE32,0,1000);Center(window);SetTimer(window,1,100,nullptr);SetFocus(cancel);return FALSE;
    }
    if(!state)return FALSE;
    if((message==WM_COMMAND&&LOWORD(wparam)==IDCANCEL)||message==WM_CLOSE) {
        state->canceled=true;EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);
        SetDlgItemTextW(window,100,i18n::Literal(L"正在停止；已交给游戏的导入可能仍会继续…"));return TRUE;
    }
    if(message==WM_TIMER){std::unique_lock lock(state->mutex);
        // EndDialog synchronously destroys the window; WM_DESTROY also takes
        // this mutex, so release it before ending the modal loop.
        if(state->ready){lock.unlock();EndDialog(window,IDOK);return TRUE;}
        if(!state->canceled) {
            SetDlgItemTextW(window,100,Phase(state->phase));SetDlgItemTextW(window,101,state->current.c_str());
            SetDlgItemTextW(window,IDCANCEL,state->phase==5?i18n::Literal(L"停止等待"):i18n::Literal(L"取消"));
        }
        const auto text=std::to_wstring(state->package+1)+L" / "+std::to_wstring(state->packages);
        SetDlgItemTextW(window,103,text.c_str());SendDlgItemMessageW(window,102,PBM_SETPOS,
            state->total?static_cast<WPARAM>(std::min(1000.0,1000.0*static_cast<double>(state->done)/static_cast<double>(state->total))):0,0);return TRUE;
    }
    if(message==WM_DESTROY){std::lock_guard lock(state->mutex);if(!state->ready)state->canceled=true;KillTimer(window,1);}return FALSE;
}
bool Installation(unsigned source,const settings::Settings& settings,audio::OsuInstallation& result) {
    const auto& executable=source==1?settings.osu_library.stable_executable:settings.osu_library.lazer_executable;
    return !executable.empty()&&audio::ReadOsuInstallation(source,executable.c_str(),result)==S_OK&&
        static_cast<unsigned>(result.kind)==source;
}
}

std::vector<std::filesystem::path> PlayerWindow::SelectedOsuPackages() const {
    std::vector<std::filesystem::path> paths;
    for(const auto row:playlist_selected_rows_) {
        const auto* track=VisiblePlaylistTrack(row);if(!track)return {};
        audio::ArchiveMemberPath member;const auto path=audio::ParseArchiveMemberPath(track->path.native(),member)?member.archive:track->path;
        if(!audio::IsOszArchive(path))return {};
        std::error_code error;const auto full=std::filesystem::canonical(path,error);
        if(error||!std::filesystem::is_regular_file(full,error)||error)return {};
        if(std::none_of(paths.begin(),paths.end(),[&](const auto& p){return _wcsicmp(p.c_str(),full.c_str())==0;}))paths.push_back(full);
    }
    return paths;
}
void PlayerWindow::AppendOsuImportMenu(HMENU menu) const {
    if(!menu||!audio::HasOsuImportSupport())return;
    try {
        if(SelectedOsuPackages().empty())return;
        audio::OsuInstallation stable,lazer;const bool has_stable=Installation(1,settings_,stable),has_lazer=Installation(2,settings_,lazer);
        if(!has_stable&&!has_lazer)return;
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);const UINT flags=MF_STRING|(osu_importing_?MF_GRAYED:0);
        if(has_stable)AppendMenuW(menu,flags,kPlaylistImportOsu,i18n::Literal(L"导入到 osu!"));
        if(has_lazer)AppendMenuW(menu,flags,kPlaylistImportOsuLazer,i18n::Literal(L"导入到 osu!lazer"));
    }catch(...){}
}
void PlayerWindow::ImportOsuSongs(unsigned source) {
    if(osu_importing_||source<1||source>2||!audio::HasOsuImportSupport())return;
    const HWND owner=playlist_window_?playlist_window_:window_;
    const auto title=i18n::Text(source==1?L"导入到 osu!":L"导入到 osu!lazer");
    try {
        const auto packages=SelectedOsuPackages();if(packages.empty())return;
        audio::OsuInstallation installation;
        if(!Installation(source,settings_,installation)) {
            MessageBoxW(owner,i18n::Literal(L"无法正确识别所选游戏程序，请在选项的 osu! 路径设置中检查。"),title.c_str(),MB_OK|MB_ICONWARNING);return;
        }
        osu_importing_=true;struct Reset {bool& flag;~Reset(){flag=false;}} reset{osu_importing_};
        Confirmation confirmation;confirmation.title=title;
        confirmation.question=i18n::Text(source==1?L"是否将所选 OSZ 导入到 osu!？":L"是否将所选 OSZ 导入到 osu!lazer？");
        confirmation.files=packages.size()==1?packages.front().filename().native():
            i18n::Text(L"所选谱面包：")+std::to_wstring(packages.size())+L"\r\n"+packages.front().filename().native()+L" …";
        const DialogTemplate confirm{{WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME,0,0,0,0,335,179},0,0,0};
        if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&confirm.dialog,owner,ConfirmProc,reinterpret_cast<LPARAM>(&confirmation))!=IDOK)return;
        std::vector<Item> results;for(const auto& path:packages)results.push_back({path});
        Progress progress;progress.title=title;progress.packages=results.size();
        const auto module=ttpcomm_module_;
        std::thread worker([&]{try {
            for(size_t i=0;i<results.size();++i){if(progress.canceled)break;
                {std::lock_guard lock(progress.mutex);progress.package=i;progress.phase=0;progress.current=results[i].archive.filename().native();}
                // These packages were selected from live playlist entries. Keep
                // their sources until a separate, explicit reference migration.
                results[i].status=audio::ImportOsuPackage(source,installation.executable,results[i].archive,confirmation.keep,true,module,
                    [&]{return progress.canceled.load();},[&](unsigned phase,uint64_t done,uint64_t total,const wchar_t* path){
                        std::lock_guard lock(progress.mutex);progress.phase=phase;progress.done=done;progress.total=total;progress.current=path?path:L"";
                    },results[i].result);
            }
            }catch(...) {for(auto& item:results)if(item.status==E_PENDING){item.status=E_FAIL;break;}}
            std::lock_guard lock(progress.mutex);progress.ready=true;
        });
        const DialogTemplate running{{WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME,0,0,0,0,335,100},0,0,0};
        if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&running.dialog,owner,ProgressProc,reinterpret_cast<LPARAM>(&progress))==-1)progress.canceled=true;
        worker.join();size_t confirmed{},pending{},failed{},referenced{};HRESULT first_error=S_OK;std::filesystem::path pending_task;
        for(const auto& item:results){if(item.status==E_PENDING)continue;
            if(FAILED(item.status)){if(item.status!=HRESULT_FROM_WIN32(ERROR_CANCELLED)){++failed;if(SUCCEEDED(first_error))first_error=item.status;}}
            else if(item.result.state==1){++confirmed;if(item.result.retention==2)++referenced;}
            else {++pending;if(pending_task.empty())pending_task=item.result.task_directory;}
        }
        if(confirmed)RefreshOsuLibraries(false,source);
        auto message=i18n::Text(L"已确认导入：")+std::to_wstring(confirmed)+L"\r\n"+
            i18n::Text(L"等待游戏确认：")+std::to_wstring(pending)+L"\r\n"+i18n::Text(L"失败：")+std::to_wstring(failed);
        if(confirmation.keep)message+=L"\r\n\r\n"+i18n::Text(L"原 osz 文件已保留。");
        else if(referenced)message+=L"\r\n\r\n"+i18n::Text(L"原 osz 文件仍被播放列表使用，已保留，避免歌曲或背景失效。");
        if(pending)message+=L"\r\n\r\n"+i18n::Text(L"已将副本交给游戏，尚未确认完成。原文件和任务记录已保留；稍后再次选择导入可继续核验。")+L"\r\n"+pending_task.native();
        if(progress.canceled)message+=L"\r\n\r\n"+i18n::Text(L"已停止本次操作；已经交给游戏的导入可能仍会继续。");
        if(FAILED(first_error)) {
            wchar_t error[1024]{},code[32]{};swprintf_s(code,L"\r\n0x%08X",static_cast<unsigned>(first_error));message+=code;
            if(HRESULT_FACILITY(first_error)==FACILITY_WIN32&&FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,HRESULT_CODE(first_error),0,error,1024,nullptr))message+=L"\r\n"+std::wstring(error);
        }
        MessageBoxW(owner,message.c_str(),title.c_str(),MB_OK|(failed?MB_ICONWARNING:MB_ICONINFORMATION));
    }catch(...) {MessageBoxW(owner,i18n::Literal(L"无法准备 OSZ 导入。原文件未交给游戏。"),title.c_str(),MB_OK|MB_ICONWARNING);}
}
}
