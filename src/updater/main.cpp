#include "ttplayer/i18n/i18n.h"
#include "ttplayer/core/text.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/update/update.h"
#include "ttplayer/ui/plain_blue_link.h"
#include "resource.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <psapi.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <thread>

namespace {
using namespace ttplayer;
struct Page {
    std::optional<update::Release> latest;
    std::vector<update::AddInUpdate> addins;
    std::filesystem::path cache;
    std::string archive_hash;
    std::wstring status;
    bool checked{};
    void ClearCache() {if(!cache.empty()) {std::error_code ignored;std::filesystem::remove_all(cache,ignored);cache.clear();}archive_hash.clear();}
    ~Page(){ClearCache();}
};
struct State {
    HWND window{},player{};
    HANDLE instance_mutex{};
    std::filesystem::path directory,session,player_executable,language_cache;
    settings::Settings settings;
    update::Version current;
    std::array<Page,3> pages;
    int active{};
    std::thread worker;
    std::atomic<bool> cancel{},done{},applying{};
    std::atomic<uint64_t> received{},total{};
    std::wstring error;
    std::filesystem::path backup_directory;
    bool busy{},installing{},closing{},success{},populating{};
    ~State(){
        cancel=true;if(worker.joinable()) worker.join();
        i18n::Shutdown();
        if(!language_cache.empty()) {std::error_code ignored;std::filesystem::remove_all(language_cache,ignored);}
        if(instance_mutex) CloseHandle(instance_mutex);
    }
    update::PackageKind Kind() const {return active==1 ? update::PackageKind::codecs : active==2 ? update::PackageKind::extras : update::PackageKind::player;}
    HWND List() const {return GetDlgItem(window,active==1?IDC_CODEC_LIST:IDC_EXTRA_LIST);}
    void Status(const std::wstring& text){pages[active].status=text;SetDlgItemTextW(window,1006,text.c_str());}
    std::filesystem::path NewStage() const {
        GUID guid{};wchar_t unique[40]{};
        if(FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,unique,40)) throw std::runtime_error("Cannot create staging identity");
        const auto stage=session/(L"download-"+std::wstring(unique));
        if(!std::filesystem::create_directory(stage)) throw std::runtime_error("Cannot create a fresh download directory");
        return stage;
    }
    void InitializeLanguage() {
        // The updater must not keep the installed i18n DLL/catalogs open while
        // replacing them. Retain the current UI language from a private snapshot.
        const auto dll=directory/L"AddIn/ttp_i18n.dll";
        if(settings.general.language==L"source" || !std::filesystem::is_regular_file(dll)) return;
        try {
            language_cache=NewStage();std::filesystem::create_directory(language_cache/L"AddIn");
            std::filesystem::copy_file(dll,language_cache/L"AddIn/ttp_i18n.dll");
            if(std::filesystem::is_directory(directory/L"i18n")) for(const auto& locale:std::filesystem::directory_iterator(directory/L"i18n")) {
                const auto name=locale.path().filename().wstring();
                if(!locale.is_directory() || locale.is_symlink() || name.empty() || name.size()>64 ||
                    name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@")!=name.npos) continue;
                for(const auto* filename:{L"ttplayer.po",L"ttplayer.mo"}) {
                    const auto source=locale.path()/filename;
                    if(!std::filesystem::is_regular_file(source) || std::filesystem::is_symlink(source)) continue;
                    std::filesystem::create_directories(language_cache/L"i18n"/name);
                    std::filesystem::copy_file(source,language_cache/L"i18n"/name/filename);
                }
            }
            i18n::Initialize(language_cache,settings.general.language);
        } catch(const std::exception&) {i18n::Shutdown();} // Source-language fallback, as for a missing provider.
    }
    std::vector<std::filesystem::path> Selected() const {
        std::vector<std::filesystem::path> result;
        if(active) for(size_t i=0;i<pages[active].addins.size();++i)
            if(pages[active].addins[i].can_update && ListView_GetCheckState(List(),static_cast<int>(i))) result.push_back(pages[active].addins[i].file);
        return result;
    }
    void Controls() {
        const auto& page=pages[active];
        SetDlgItemTextW(window,1008,active ? i18n::Literal(L"更新所选插件") : page.latest && page.latest->version>current ? i18n::Literal(L"立即更新") : i18n::Literal(L"重新检查"));
        EnableWindow(GetDlgItem(window,1008),!busy && (!active || !Selected().empty()));
        for(const int id:{1003,IDC_UPDATE_TABS,IDC_RECHECK,IDC_CODEC_LIST,IDC_EXTRA_LIST,IDC_SELECT_UPDATES,IDC_CLEAR_SELECTION})
            EnableWindow(GetDlgItem(window,id),!busy);
    }
    void Populate() {
        if(!active) return;
        populating=true;const auto list=List();ListView_DeleteAllItems(list);
        for(size_t index=0;index<pages[active].addins.size();++index) {
            const auto& addin=pages[active].addins[index];auto name=addin.file.filename().wstring();
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(index);item.pszText=name.data();
            const int row=ListView_InsertItem(list,&item);
            ListView_SetItemText(list,row,1,const_cast<wchar_t*>(addin.current_version.c_str()));
            ListView_SetItemText(list,row,2,const_cast<wchar_t*>(addin.next_version.c_str()));
            ListView_SetItemText(list,row,3,const_cast<wchar_t*>(addin.local_newer ? i18n::Literal(L"本地版本较新") : !addin.can_update ? i18n::Literal(L"已是最新") : addin.installed ? i18n::Literal(L"可更新") : i18n::Literal(L"可安装")));
            // Do not silently install optional add-ins the user does not have.
            ListView_SetCheckState(list,row,addin.can_update && addin.installed);
        }
        populating=false;
    }
    void Layout() {
        const auto place=[&](int id,int x,int y,int width,int height) {
            RECT bounds{x,y,x+width,y+height};i18n::MapDialogRect(window,&bounds);
            SetWindowPos(GetDlgItem(window,id),nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
        };
        if(!active) {
            // Keep the original 280 x 155 dialog size; only tighten the version
            // rows to make room for the tab strip. Buttons retain their old positions.
            place(IDC_UPDATE_TABS,12,4,256,18);
            place(IDC_CURRENT_LABEL,12,25,70,12);place(1001,84,25,184,12);
            place(IDC_LATEST_LABEL,12,41,70,12);place(1002,84,41,184,12);
            place(IDC_SOURCE_LABEL,12,58,65,12);place(1003,84,54,120,60);
            place(1004,12,75,120,14);place(1005,148,75,120,14);
            place(1006,12,91,256,20);place(1007,12,114,256,6);
            place(1008,118,130,72,18);place(IDCANCEL,196,130,72,18);
        } else {
            place(IDC_UPDATE_TABS,12,4,406,184);
            for(const int id:{IDC_CODEC_LIST,IDC_EXTRA_LIST}) place(id,20,28,390,128);
            place(IDC_SELECT_UPDATES,220,163,90,18);place(IDC_CLEAR_SELECTION,316,163,90,18);
            place(IDC_SOURCE_LABEL,12,194,60,12);place(1003,76,190,92,60);
            place(1004,192,193,106,14);place(1005,312,193,106,14);
            // Two status lines suffice; do not reserve a large empty block
            // between the usual one-line hint and the progress bar.
            place(1006,12,212,406,16);place(1007,12,232,406,6);
            place(IDC_RECHECK,138,244,90,18);place(1008,234,244,88,18);place(IDCANCEL,328,244,90,18);
            for(const int id:{IDC_CODEC_LIST,IDC_EXTRA_LIST}) {
                RECT bounds{};const auto list=GetDlgItem(window,id);GetClientRect(list,&bounds);
                const int width=bounds.right-GetSystemMetrics(SM_CXVSCROLL)-4;
                for(int column=0;column<4;++column) ListView_SetColumnWidth(list,column,width*(column==0?31:23)/100);
            }
        }
        RECT bounds{0,0,active?430:280,active?268:155};i18n::MapDialogRect(window,&bounds);
        AdjustWindowRectEx(&bounds,GetWindowLongW(window,GWL_STYLE),FALSE,GetWindowLongW(window,GWL_EXSTYLE));
        const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        RECT current_bounds{};GetWindowRect(window,&current_bounds);
        MONITORINFO monitor{sizeof(monitor)};
        if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) {
            current_bounds.left=std::max(monitor.rcWork.left,std::min(current_bounds.left,monitor.rcWork.right-width));
            current_bounds.top=std::max(monitor.rcWork.top,std::min(current_bounds.top,monitor.rcWork.bottom-height));
        }
        SetWindowPos(window,nullptr,current_bounds.left,current_bounds.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void ShowPage() {
        Layout();
        for(const int id:{IDC_CURRENT_LABEL,IDC_LATEST_LABEL,1001,1002}) ShowWindow(GetDlgItem(window,id),active?SW_HIDE:SW_SHOW);
        ShowWindow(GetDlgItem(window,IDC_CODEC_LIST),active==1?SW_SHOW:SW_HIDE);
        ShowWindow(GetDlgItem(window,IDC_EXTRA_LIST),active==2?SW_SHOW:SW_HIDE);
        for(const int id:{IDC_SELECT_UPDATES,IDC_CLEAR_SELECTION,IDC_RECHECK}) ShowWindow(GetDlgItem(window,id),active?SW_SHOW:SW_HIDE);
        SetDlgItemTextW(window,1006,pages[active].status.c_str());
        SendDlgItemMessageW(window,1007,PBM_SETPOS,0,0);
        if(!pages[active].checked) Check();else Controls();
    }
    void FinishWorker() {if(worker.joinable()) worker.join();busy=false;}
    template<class Work> void StartWorker(Work&& work) {
        try {worker=std::thread([task=std::forward<Work>(work)]() mutable {
            const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);task();
            if(SUCCEEDED(initialized)) CoUninitialize();
        });}
        catch(const std::exception& e) {error=update::ErrorText(e);done.store(true,std::memory_order_release);}
    }
    void Check() {
        if(busy) return;
        auto& page=pages[active];page.latest.reset();page.addins.clear();page.ClearCache();
        busy=true;installing=false;success=false;cancel=false;done=false;applying=false;received=0;total=0;error.clear();
        if(!active) SetDlgItemTextW(window,1002,i18n::Literal(L"正在检查…"));else Populate();
        Status(active ? i18n::Literal(L"正在检查插件更新并下载合集以读取版本…") : i18n::Literal(L"正在检查新版本…"));Controls();
        const auto kind=Kind();
        StartWorker([this,&page,kind] {
            try {
                update::Http http(directory,settings.network);
                if(update::IsXp() && !http.HasProvider()) error=i18n::Text(update::kMissingHttps);
                else {
                    page.latest=update::Check(http,settings.general.update_source==1 ? update::Source::github : update::Source::gitee,[this]{return cancel.load();},update::PlayerNameForPath(player_executable),kind);
                    if(kind!=update::PackageKind::player && page.latest) {
                        page.cache=NewStage();
                        const auto payload=update::PrepareAddInPackage(http,*page.latest,page.cache,[this]{return cancel.load();},[this](uint64_t n,uint64_t all){received=n;total=all;});
                        page.addins=update::InspectAddInPackage(directory,payload,kind);
                        page.archive_hash=update::Sha256(page.cache/L"package.zip.part");
                    }
                }
            } catch(const std::exception& e){error=update::ErrorText(e);}
            done.store(true,std::memory_order_release);
        });
    }
    HANDLE PlayerHandle(HWND candidate,std::filesystem::path* executable=nullptr) {
        if(!candidate || !IsWindow(candidate)) return nullptr;
        DWORD pid{};GetWindowThreadProcessId(candidate,&pid);
        HANDLE handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        if(!handle) throw std::runtime_error("Cannot wait for the player process");
        wchar_t path[32768]{};
        if(!GetModuleFileNameExW(handle,nullptr,path,static_cast<DWORD>(std::size(path))) ||
            (_wcsicmp(path,(directory/update::kPlayerFileName).c_str())!=0 &&
             _wcsicmp(path,(directory/update::kRebuildPlayerFileName).c_str())!=0) ||
            !update::IsPlayerExecutable(path)) {CloseHandle(handle);return nullptr;}
        if(executable) *executable=path;
        return handle;
    }
    void Install() {
        if(busy) return;
        auto& page=pages[active];const auto kind=Kind();
        if(!active && (!page.latest || page.latest->version<=current)) {Check();return;}
        const auto selected=Selected();
        if(active && selected.empty()) {Status(i18n::Literal(L"请勾选要更新或安装的插件。"));return;}
        busy=true;installing=true;cancel=false;done=false;applying=false;success=false;error.clear();backup_directory.clear();received=0;total=0;
        Status(active ? i18n::Literal(L"正在校验所选插件…") : i18n::Literal(L"正在下载更新…"));Controls();
        StartWorker([this,&page,kind,selected] {
            struct Processes {std::vector<HANDLE> handles;~Processes(){for(auto handle:handles) CloseHandle(handle);}} processes;
            try {
                // Each attempt has its own directory; canceled/failed payloads
                // can never be reused as a completed update.
                const auto stage=NewStage();
                struct Staging {std::filesystem::path path;~Staging(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{stage};
                std::filesystem::path prepared;
                if(kind==update::PackageKind::player) {
                    update::Http http(directory,settings.network);
                    if(update::IsXp() && !http.HasProvider()) throw std::runtime_error(coreMessage());
                    prepared=update::PreparePackage(http,*page.latest,stage,[this]{return cancel.load();},[this](uint64_t n,uint64_t all){received=n;total=all;});
                } // Release the installed HTTPS DLL before replacing package files.
                else {
                    const auto zip=page.cache/L"package.zip.part";
                    if(page.archive_hash.empty() || update::Sha256(zip)!=page.archive_hash) throw std::runtime_error("Cached add-in package changed; check for updates again");
                    prepared=stage/L"payload";update::ExtractPackage(zip,prepared,kind);
                }
                if(kind==update::PackageKind::player && _wcsicmp(prepared.filename().c_str(),player_executable.filename().c_str())!=0)
                    throw std::runtime_error("The update package targets a different player executable");
                if(cancel) throw std::runtime_error("Canceled");
                // Probe write access before asking the user to stop playback.
                wchar_t probe[MAX_PATH]{};
                if(!GetTempFileNameW(directory.c_str(),L"ttu",0,probe)) throw std::runtime_error(core::WideToUtf8(i18n::Text(L"播放器目录不可写，请以具有该目录写入权限的用户运行更新器。")));
                DeleteFileW(probe);
                std::vector<HWND> candidates;
                EnumWindows([](HWND candidate,LPARAM data)->BOOL {
                    wchar_t klass[64]{};GetClassNameW(candidate,klass,64);
                    if(wcscmp(klass,L"TTPlayer_PlayerWnd")==0) reinterpret_cast<std::vector<HWND>*>(data)->push_back(candidate);
                    return TRUE;
                },LPARAM(&candidates));
                if(player && IsWindow(player) && std::find(candidates.begin(),candidates.end(),player)==candidates.end()) candidates.push_back(player);
                for(const auto candidate:candidates) if(const auto process=PlayerHandle(candidate)) {
                    processes.handles.push_back(process);
                    DWORD_PTR accepted{};
                    if(!SendMessageTimeoutW(candidate,update::kPrepareInstallMessage,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,5000,&accepted) || !accepted)
                        throw std::runtime_error(core::WideToUtf8(i18n::Text(L"播放器暂时无法退出，请保存正在编辑的内容后重试。")));
                    const auto deadline=GetTickCount64()+120000;
                    for(;;) {
                        const auto wait=WaitForSingleObject(process,100);
                        if(wait==WAIT_OBJECT_0) break;
                        if(wait!=WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for the player to exit");
                        if(cancel) throw std::runtime_error("Canceled");
                        if(GetTickCount64()>deadline) throw std::runtime_error(core::WideToUtf8(i18n::Text(L"播放器尚未退出，请关闭播放器后重试。")));
                    }
                }
                if(cancel) throw std::runtime_error("Canceled");
                applying=true;
                auto installation=kind==update::PackageKind::player ? std::make_unique<update::PackageInstall>(directory,prepared.parent_path(),page.latest->version) :
                    std::make_unique<update::PackageInstall>(directory,prepared,kind,selected);
                installation->Apply();
                try {
                    const auto exe=player_executable;std::wstring command=L"\""+exe.wstring()+L"\"";
                    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
                    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&si,&pi)) {
                        throw std::runtime_error(core::WideToUtf8(i18n::Text(L"新版启动失败。")));
                    }
                    CloseHandle(pi.hThread);
                    const auto input=WaitForInputIdle(pi.hProcess,15000);
                    DWORD code=STILL_ACTIVE;GetExitCodeProcess(pi.hProcess,&code);
                    CloseHandle(pi.hProcess);
                    if(code!=STILL_ACTIVE && code!=0) throw std::runtime_error(core::WideToUtf8(i18n::Text(L"新版启动后异常退出。")));
                    backup_directory=installation->BackupDirectory();
                    installation->Commit();
                    if(input==WAIT_TIMEOUT) error=i18n::Literal(L"文件已更新，程序启动较慢。旧文件备份：\n")+backup_directory.wstring();
                    success=true;
                } catch(const std::exception& reason) {
                    installation->Rollback();
                    throw std::runtime_error(std::string(reason.what())+core::WideToUtf8(i18n::Text(L" 已恢复更新前的全部文件。")));
                }
                if(kind!=update::PackageKind::player) page.addins=update::InspectAddInPackage(directory,prepared,kind);
            } catch(const std::exception& e){error=update::ErrorText(e);}
            done.store(true,std::memory_order_release);
        });
    }
    static std::string coreMessage(){return core::WideToUtf8(i18n::Text(L"缺少 HTTPS 组件，请前往发布页手动下载更新"));}
    void Poll() {
        if(!busy) return;
        const uint64_t all=total,n=received;
        SendDlgItemMessageW(window,1007,PBM_SETPOS,all ? static_cast<WPARAM>(std::min<uint64_t>(99,n*100/all)) : 0,0);
        if(applying) {EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);Status(i18n::Literal(L"正在替换文件并启动播放器…"));}
        if(!done.load(std::memory_order_acquire)) return;
        FinishWorker();applying=false;EnableWindow(GetDlgItem(window,IDCANCEL),TRUE);
        if(closing) {DestroyWindow(window);return;}
        auto& page=pages[active];page.checked=true;
        if(!error.empty()) {
            Status(error);MessageBoxW(window,error.c_str(),i18n::Literal(L"软件更新"),MB_OK|(success?MB_ICONINFORMATION:MB_ICONEXCLAMATION));
            if(!installing) {page.latest.reset();page.addins.clear();page.ClearCache();if(!active) SetDlgItemTextW(window,1002,i18n::Literal(L"检查失败"));}
        } else if(installing) Status(i18n::Literal(L"更新完成。旧文件备份：")+backup_directory.wstring());
        else if(!active) {
            SetDlgItemTextW(window,1002,page.latest ? page.latest->version.Text().c_str() : i18n::Literal(L"无可用发行包"));
            Status(page.latest && page.latest->version>current ? i18n::Literal(L"发现新版本，可以立即更新。") : i18n::Literal(L"当前没有可安装的新版本。"));
        } else {
            Status(!page.latest ? i18n::Literal(L"无可用发行包") : i18n::Literal(L"请勾选要更新或安装的插件。未安装的插件默认不勾选。"));
        }
        if(success) {current=update::FileVersion(player_executable);SetDlgItemTextW(window,1001,current.Text().c_str());SendDlgItemMessageW(window,1007,PBM_SETPOS,100,0);}
        if(success && !active) for(size_t i=1;i<pages.size();++i) {
            pages[i].checked=false;pages[i].addins.clear();pages[i].latest.reset();pages[i].ClearCache();
        }
        if(!installing || success) Populate();
        Controls();
    }
    void Close() {
        if(applying) return;
        if(busy) {closing=true;cancel=true;Status(i18n::Literal(L"正在取消，请稍候…"));return;}
        DestroyWindow(window);
    }
    static INT_PTR CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,DWLP_USER));
        if(msg==WM_INITDIALOG) {
            s=reinterpret_cast<State*>(lp);s->window=hwnd;SetWindowLongPtrW(hwnd,DWLP_USER,LONG_PTR(s));
            // Preserve existing translation contexts after assigning IDs to the
            // formerly anonymous labels so they can be hidden with the page.
            SetDlgItemTextW(hwnd,IDC_CURRENT_LABEL,i18n::Text(L"当前版本号：","updater/dialog/101/control/4294967295/0").c_str());
            SetDlgItemTextW(hwnd,IDC_LATEST_LABEL,i18n::Text(L"最新版本号：","updater/dialog/101/control/4294967295/1").c_str());
            SetDlgItemTextW(hwnd,IDC_SOURCE_LABEL,i18n::Text(L"更新源：","updater/dialog/101/control/4294967295/2").c_str());
            SetDlgItemTextW(hwnd,1001,s->current.Text().c_str());
            SendDlgItemMessageW(hwnd,1003,CB_ADDSTRING,0,LPARAM(L"Gitee"));SendDlgItemMessageW(hwnd,1003,CB_ADDSTRING,0,LPARAM(L"GitHub"));
            SendDlgItemMessageW(hwnd,1003,CB_SETCURSEL,s->settings.general.update_source==1?1:0,0);
            ui::PlainBlueLink::Attach(GetDlgItem(hwnd,1004),update::kGitHubPage);
            ui::PlainBlueLink::Attach(GetDlgItem(hwnd,1005),update::kGiteePage);
            for(const auto* label:{i18n::Literal(L"主程序"),i18n::Literal(L"音频插件"),i18n::Literal(L"拓展插件")}) {
                TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(label);
                TabCtrl_InsertItem(GetDlgItem(hwnd,IDC_UPDATE_TABS),TabCtrl_GetItemCount(GetDlgItem(hwnd,IDC_UPDATE_TABS)),&item);
            }
            for(const int id:{IDC_CODEC_LIST,IDC_EXTRA_LIST}) {
                const auto list=GetDlgItem(hwnd,id);
                ListView_SetExtendedListViewStyle(list,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_LABELTIP|LVS_EX_DOUBLEBUFFER);
                RECT bounds{};GetClientRect(list,&bounds);const int width=bounds.right-GetSystemMetrics(SM_CXVSCROLL)-4;
                int column=0;
                for(const auto* label:{i18n::Literal(L"插件"),i18n::Literal(L"当前版本"),i18n::Literal(L"更新版本"),i18n::Literal(L"状态")}) {
                    LVCOLUMNW item{};item.mask=LVCF_TEXT|LVCF_WIDTH;item.pszText=const_cast<wchar_t*>(label);
                    item.cx=width*(column==0?31:23)/100;ListView_InsertColumn(list,column++,&item);
                }
            }
            SetTimer(hwnd,1,100,nullptr);s->ShowPage();return TRUE;
        }
        if(!s) return FALSE;
        try {
            if(msg==WM_TIMER) {s->Poll();return TRUE;}
            if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)) {s->Close();return TRUE;}
            if(msg==WM_COMMAND && LOWORD(wp)==1008) {s->Install();return TRUE;}
            if(msg==WM_COMMAND && LOWORD(wp)==IDC_RECHECK) {s->Check();return TRUE;}
            if(msg==WM_COMMAND && (LOWORD(wp)==IDC_SELECT_UPDATES || LOWORD(wp)==IDC_CLEAR_SELECTION)) {
                if(!s->busy && s->active) {
                    s->populating=true;
                    for(size_t i=0;i<s->pages[s->active].addins.size();++i)
                        ListView_SetCheckState(s->List(),static_cast<int>(i),LOWORD(wp)==IDC_SELECT_UPDATES && s->pages[s->active].addins[i].can_update);
                    s->populating=false;s->Controls();
                }
                return TRUE;
            }
            if(msg==WM_COMMAND && LOWORD(wp)==1003 && HIWORD(wp)==CBN_SELCHANGE) {
                if(s->busy) return TRUE;
                s->settings.general.update_source=SendDlgItemMessageW(hwnd,1003,CB_GETCURSEL,0,0)==1 ? 1 : 0;
                for(auto& page:s->pages) {page.checked=false;page.latest.reset();page.addins.clear();page.status.clear();page.ClearCache();}
                s->Check();return TRUE;}
            if(msg==WM_NOTIFY) {
                const auto* notice=reinterpret_cast<const NMHDR*>(lp);
                if(notice->idFrom==IDC_UPDATE_TABS && notice->code==TCN_SELCHANGING && s->busy) {SetWindowLongPtrW(hwnd,DWLP_MSGRESULT,TRUE);return TRUE;}
                if(notice->idFrom==IDC_UPDATE_TABS && notice->code==TCN_SELCHANGE && !s->busy) {
                    const int selected=TabCtrl_GetCurSel(notice->hwndFrom);
                    if(selected>=0 && selected<3) {s->active=selected;s->ShowPage();}return TRUE;
                }
                if((notice->idFrom==IDC_CODEC_LIST || notice->idFrom==IDC_EXTRA_LIST) && s->active && !s->busy && !s->populating) {
                    const auto* change=reinterpret_cast<const NMLISTVIEW*>(lp);
                    if(notice->code==LVN_ITEMCHANGING && (change->uChanged&LVIF_STATE) && (change->uNewState&LVIS_STATEIMAGEMASK)==INDEXTOSTATEIMAGEMASK(2) &&
                        change->iItem>=0 && static_cast<size_t>(change->iItem)<s->pages[s->active].addins.size() && !s->pages[s->active].addins[change->iItem].can_update) {
                        SetWindowLongPtrW(hwnd,DWLP_MSGRESULT,TRUE);return TRUE;
                    }
                    if(notice->code==LVN_ITEMCHANGED) {s->Controls();return TRUE;}
                }
            }
            if(msg==WM_DESTROY) {PostQuitMessage(0);return TRUE;}
        } catch(const std::exception& e){MessageBoxW(hwnd,update::ErrorText(e).c_str(),i18n::Literal(L"软件更新"),MB_OK|MB_ICONERROR);}
        return FALSE;
    }
};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    using namespace ttplayer;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS|ICC_LINK_CLASS|ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    try {
        State state;state.directory=update::ExecutablePath().parent_path();
        int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);
        struct Args{LPWSTR* p;~Args(){LocalFree(p);}}guard{args};
        bool temporary=false;
        std::wstring player_name;
        for(int i=1;i<count;++i) {
            const std::wstring arg=args[i];
            if(arg==L"--directory" && i+1<count) {state.directory=std::filesystem::absolute(args[++i]);temporary=true;}
            else if(arg==L"--player-window" && i+1<count) state.player=reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(args[++i],nullptr,10)));
            else if(arg==L"--player-name" && i+1<count) player_name=args[++i];
        }
        // Older players pass only their window. Resolve that process before
        // moving the updater to its temporary session, then retain the name.
        if(player_name.empty() && state.player) {
            if(const auto process=state.PlayerHandle(state.player,&state.player_executable)) CloseHandle(process);
            if(!state.player_executable.empty()) player_name=state.player_executable.filename().wstring();
        }
        state.player_executable=update::InstalledPlayerPath(state.directory,player_name);
        state.settings=settings::LoadRuntimeSettings(state.directory);
        state.session=update::ExecutablePath().parent_path();
        if(temporary) state.InitializeLanguage();else i18n::Initialize(state.directory,state.settings.general.language);
        state.current=update::FileVersion(state.player_executable);
        if(!temporary) {
            wchar_t folder[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,folder)) throw std::runtime_error("Cannot locate temporary directory");
            GUID guid{};CoCreateGuid(&guid);wchar_t unique[40]{};StringFromGUID2(guid,unique,40);
            const auto session=std::filesystem::path(folder)/(L"TTPUpdater-"+std::wstring(unique));std::filesystem::create_directory(session);
            const auto exe=session/L"TTPUpdater.exe";
            if(!CopyFileW(update::ExecutablePath().c_str(),exe.c_str(),TRUE)) throw std::runtime_error("Cannot prepare updater");
            std::wstring cmd=L"\""+exe.wstring()+L"\" --directory \""+state.directory.wstring()+L"\" --player-window "+std::to_wstring(reinterpret_cast<uintptr_t>(state.player))+
                L" --player-name \""+state.player_executable.filename().wstring()+L"\"";
            STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
            if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,state.directory.c_str(),&si,&pi)) throw std::runtime_error("Cannot start temporary updater");
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CoUninitialize();return 0;
        }
        state.session=update::ExecutablePath().parent_path();state.settings=settings::LoadRuntimeSettings(state.directory);
        auto identity=state.directory.wstring();CharLowerBuffW(identity.data(),static_cast<DWORD>(identity.size()));
        uint64_t hash=14695981039346656037ULL;for(const auto c:identity) {hash^=c;hash*=1099511628211ULL;}
        state.instance_mutex=CreateMutexW(nullptr,FALSE,(L"Local\\TTPlayerUpdater-"+std::to_wstring(hash)).c_str());
        if(!state.instance_mutex) throw std::runtime_error("Cannot create updater instance lock");
        if(GetLastError()==ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr,i18n::Literal(L"此目录的更新器已经打开。"),i18n::Literal(L"软件更新"),MB_OK|MB_ICONINFORMATION);CoUninitialize();return 0;
        }
        const auto window=ui::CreateWtlDialog(instance,MAKEINTRESOURCEW(101),nullptr,State::Proc,LPARAM(&state));
        if(!window) throw std::runtime_error("Cannot create updater dialog");
        ShowWindow(window,show);MSG message{};
        while(GetMessageW(&message,nullptr,0,0)>0) if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    } catch(const std::exception& e){MessageBoxW(nullptr,update::ErrorText(e).c_str(),L"TTPUpdater",MB_OK|MB_ICONERROR);CoUninitialize();return 1;}
    CoUninitialize();return 0;
}
