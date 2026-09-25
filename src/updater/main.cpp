#include "ttplayer/update/update.h"
#include "ttplayer/ui/plain_blue_link.h"
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
struct State {
    HWND window{},player{};
    HANDLE instance_mutex{};
    std::filesystem::path directory,session;
    settings::Settings settings;
    update::Version current;
    std::optional<update::Release> latest;
    std::thread worker;
    std::atomic<bool> cancel{},done{},applying{};
    std::atomic<uint64_t> received{},total{};
    std::wstring error;
    bool busy{},installing{},closing{},success{};
    ~State(){cancel=true;if(worker.joinable()) worker.join();if(instance_mutex) CloseHandle(instance_mutex);}
    void Status(const std::wstring& text){SetDlgItemTextW(window,1006,text.c_str());}
    void FinishWorker() {if(worker.joinable()) worker.join();busy=false;}
    template<class Work> void StartWorker(Work&& work) {
        try {worker=std::thread(std::forward<Work>(work));}
        catch(const std::exception& e) {error=update::ErrorText(e);done.store(true,std::memory_order_release);}
    }
    void Check() {
        if(busy) return;
        busy=true;installing=false;success=false;cancel=false;done=false;error.clear();latest.reset();
        SetDlgItemTextW(window,1002,L"正在检查…");Status(L"正在检查新版本…");
        EnableWindow(GetDlgItem(window,1008),FALSE);EnableWindow(GetDlgItem(window,1003),FALSE);
        StartWorker([this] {
            const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            try {
                update::Http http(directory,settings.network);
                if(update::IsXp() && !http.HasProvider()) error=update::kMissingHttps;
                else latest=update::Check(http,settings.general.update_source==1 ? update::Source::github : update::Source::gitee,[this]{return cancel.load();});
            } catch(const std::exception& e){error=update::ErrorText(e);}
            if(SUCCEEDED(initialized)) CoUninitialize();done.store(true,std::memory_order_release);
        });
    }
    HANDLE PlayerHandle(HWND candidate) {
        if(!candidate || !IsWindow(candidate)) return nullptr;
        DWORD pid{};GetWindowThreadProcessId(candidate,&pid);
        HANDLE handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
        if(!handle) throw std::runtime_error("Cannot wait for the player process");
        wchar_t path[32768]{};
        if(!GetModuleFileNameExW(handle,nullptr,path,static_cast<DWORD>(std::size(path))) ||
            _wcsicmp(path,(directory/L"TTPlayerRebuild.exe").c_str())!=0) {CloseHandle(handle);return nullptr;}
        return handle;
    }
    void Install() {
        if(busy) return;
        if(!latest || latest->version<=current) {Check();return;}
        busy=true;installing=true;cancel=false;done=false;applying=false;success=false;error.clear();received=0;total=0;
        Status(L"正在下载更新…");EnableWindow(GetDlgItem(window,1008),FALSE);EnableWindow(GetDlgItem(window,1003),FALSE);
        StartWorker([this] {
            struct Processes {std::vector<HANDLE> handles;~Processes(){for(auto handle:handles) CloseHandle(handle);}} processes;
            try {
                update::Http http(directory,settings.network);
                if(update::IsXp() && !http.HasProvider()) throw std::runtime_error(coreMessage());
                // Each attempt has its own directory; canceled/failed payloads
                // can never be reused as a completed update.
                const auto stage=session/(L"download-"+std::to_wstring(GetTickCount64()));
                if(!std::filesystem::create_directory(stage)) throw std::runtime_error("Cannot create a fresh download directory");
                struct Staging {std::filesystem::path path;~Staging(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{stage};
                const auto prepared=update::PreparePackage(http,*latest,stage,[this]{return cancel.load();},[this](uint64_t n,uint64_t all){received=n;total=all;});
                if(cancel) throw std::runtime_error("Canceled");
                // Probe write access before asking the user to stop playback.
                wchar_t probe[MAX_PATH]{};
                if(!GetTempFileNameW(directory.c_str(),L"ttu",0,probe)) throw std::runtime_error("播放器目录不可写，请以具有该目录写入权限的用户运行更新器。");
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
                        throw std::runtime_error("播放器暂时无法退出，请保存正在编辑的内容后重试。");
                    const auto deadline=GetTickCount64()+120000;
                    for(;;) {
                        const auto wait=WaitForSingleObject(process,100);
                        if(wait==WAIT_OBJECT_0) break;
                        if(wait!=WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for the player to exit");
                        if(cancel) throw std::runtime_error("Canceled");
                        if(GetTickCount64()>deadline) throw std::runtime_error("播放器尚未退出，请关闭播放器后重试。");
                    }
                }
                if(cancel) throw std::runtime_error("Canceled");
                applying=true;
                update::ReplacePlayer(directory,prepared,latest->version);
                const auto next_updater=prepared.parent_path()/L"TTPUpdater.exe";
                if(std::filesystem::is_regular_file(next_updater)) {
                    // This process runs from session/, so its installed image
                    // is free to receive the next updater without self-locking.
                    try { update::ReplaceUpdater(directory,next_updater,latest->version); }
                    catch(const std::exception& reason) {
                        error=L"播放器已更新，但更新器替换失败，请从发行包手动更新 TTPUpdater.exe。\n"+update::ErrorText(reason);
                    }
                }
                const auto exe=directory/L"TTPlayerRebuild.exe";std::wstring command=L"\""+exe.wstring()+L"\"";
                STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
                if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&si,&pi)) {
                    const auto backup=directory/L"TTPlayerRebuild.exe.bak";
                    if(!MoveFileExW(backup.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("新版启动失败，请将 TTPlayerRebuild.exe.bak 恢复为 TTPlayerRebuild.exe。");
                    throw std::runtime_error("新版启动失败，已恢复旧程序。");
                }
                CloseHandle(pi.hThread);
                const auto input=WaitForInputIdle(pi.hProcess,15000);
                DWORD code{};GetExitCodeProcess(pi.hProcess,&code);
                CloseHandle(pi.hProcess);
                if(code!=STILL_ACTIVE && code!=0) throw std::runtime_error("新版启动后异常退出，旧程序保留为 TTPlayerRebuild.exe.bak。");
                if(input==WAIT_TIMEOUT && error.empty()) error=L"文件已更新，程序启动较慢；旧程序已保留为 TTPlayerRebuild.exe.bak。";
                success=true;
            } catch(const std::exception& e){error=update::ErrorText(e);}
            done.store(true,std::memory_order_release);
        });
    }
    static std::string coreMessage(){return "缺少 HTTPS 组件，请前往发布页手动下载更新";}
    void Poll() {
        if(!busy) return;
        const uint64_t all=total,n=received;
        SendDlgItemMessageW(window,1007,PBM_SETPOS,all ? static_cast<WPARAM>(std::min<uint64_t>(99,n*100/all)) : 0,0);
        if(applying) {EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);Status(L"正在替换文件并启动播放器…");}
        if(!done.load(std::memory_order_acquire)) return;
        FinishWorker();applying=false;EnableWindow(GetDlgItem(window,IDCANCEL),TRUE);EnableWindow(GetDlgItem(window,1003),TRUE);
        if(closing) {DestroyWindow(window);return;}
        if(!error.empty()) {
            Status(error);MessageBoxW(window,error.c_str(),L"软件更新",MB_OK|(success?MB_ICONINFORMATION:MB_ICONEXCLAMATION));
            if(!installing) SetDlgItemTextW(window,1002,L"检查失败");
        } else if(installing) Status(L"更新完成，旧程序已保留为 TTPlayerRebuild.exe.bak。");
        else {
            SetDlgItemTextW(window,1002,latest ? latest->version.Text().c_str() : L"无可用发行包");
            Status(latest && latest->version>current ? L"发现新版本，可以立即更新。" : L"当前没有可安装的新版本。");
        }
        if(success) {current=update::FileVersion(directory/L"TTPlayerRebuild.exe");SetDlgItemTextW(window,1001,current.Text().c_str());SendDlgItemMessageW(window,1007,PBM_SETPOS,100,0);}
        SetDlgItemTextW(window,1008,latest && latest->version>current ? L"立即更新" : L"重新检查");
        EnableWindow(GetDlgItem(window,1008),TRUE);
    }
    void Close() {
        if(applying) return;
        if(busy) {closing=true;cancel=true;Status(L"正在取消，请稍候…");return;}
        DestroyWindow(window);
    }
    static INT_PTR CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,DWLP_USER));
        if(msg==WM_INITDIALOG) {
            s=reinterpret_cast<State*>(lp);s->window=hwnd;SetWindowLongPtrW(hwnd,DWLP_USER,LONG_PTR(s));
            SetDlgItemTextW(hwnd,1001,s->current.Text().c_str());
            SendDlgItemMessageW(hwnd,1003,CB_ADDSTRING,0,LPARAM(L"Gitee"));SendDlgItemMessageW(hwnd,1003,CB_ADDSTRING,0,LPARAM(L"GitHub"));
            SendDlgItemMessageW(hwnd,1003,CB_SETCURSEL,s->settings.general.update_source==1?1:0,0);
            ui::PlainBlueLink::Attach(GetDlgItem(hwnd,1004),update::kGitHubPage);
            ui::PlainBlueLink::Attach(GetDlgItem(hwnd,1005),update::kGiteePage);
            SetTimer(hwnd,1,100,nullptr);s->Check();return TRUE;
        }
        if(!s) return FALSE;
        try {
            if(msg==WM_TIMER) {s->Poll();return TRUE;}
            if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)) {s->Close();return TRUE;}
            if(msg==WM_COMMAND && LOWORD(wp)==1008) {s->Install();return TRUE;}
            if(msg==WM_COMMAND && LOWORD(wp)==1003 && HIWORD(wp)==CBN_SELCHANGE) {
                s->settings.general.update_source=SendDlgItemMessageW(hwnd,1003,CB_GETCURSEL,0,0)==1 ? 1 : 0;s->Check();return TRUE;}
            if(msg==WM_DESTROY) {PostQuitMessage(0);return TRUE;}
        } catch(const std::exception& e){MessageBoxW(hwnd,update::ErrorText(e).c_str(),L"软件更新",MB_OK|MB_ICONERROR);}
        return FALSE;
    }
};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    using namespace ttplayer;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS|ICC_LINK_CLASS};InitCommonControlsEx(&controls);
    try {
        State state;state.directory=update::ExecutablePath().parent_path();
        int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);
        struct Args{LPWSTR* p;~Args(){LocalFree(p);}}guard{args};
        bool temporary=false;
        for(int i=1;i<count;++i) {
            const std::wstring arg=args[i];
            if(arg==L"--directory" && i+1<count) {state.directory=std::filesystem::absolute(args[++i]);temporary=true;}
            else if(arg==L"--player-window" && i+1<count) state.player=reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(args[++i],nullptr,10)));
        }
        state.current=update::FileVersion(state.directory/L"TTPlayerRebuild.exe");
        if(!temporary) {
            wchar_t folder[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,folder)) throw std::runtime_error("Cannot locate temporary directory");
            GUID guid{};CoCreateGuid(&guid);wchar_t unique[40]{};StringFromGUID2(guid,unique,40);
            const auto session=std::filesystem::path(folder)/(L"TTPUpdater-"+std::wstring(unique));std::filesystem::create_directory(session);
            const auto exe=session/L"TTPUpdater.exe";
            if(!CopyFileW(update::ExecutablePath().c_str(),exe.c_str(),TRUE)) throw std::runtime_error("Cannot prepare updater");
            std::wstring cmd=L"\""+exe.wstring()+L"\" --directory \""+state.directory.wstring()+L"\" --player-window "+std::to_wstring(reinterpret_cast<uintptr_t>(state.player));
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
            MessageBoxW(nullptr,L"此目录的更新器已经打开。",L"软件更新",MB_OK|MB_ICONINFORMATION);CoUninitialize();return 0;
        }
        const auto window=CreateDialogParamW(instance,MAKEINTRESOURCEW(101),nullptr,State::Proc,LPARAM(&state));
        if(!window) throw std::runtime_error("Cannot create updater dialog");
        ShowWindow(window,show);MSG message{};
        while(GetMessageW(&message,nullptr,0,0)>0) if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    } catch(const std::exception& e){MessageBoxW(nullptr,update::ErrorText(e).c_str(),L"TTPUpdater",MB_OK|MB_ICONERROR);CoUninitialize();return 1;}
    CoUninitialize();return 0;
}
