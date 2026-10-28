#include "ttplayer/i18n/i18n.h"
#include "archive_password_dialog.h"
#include "modern_file_dialog.h"
#include "ttplayer/audio/archive_password.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "../app/resource_ids.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace ttplayer::ui {
namespace {
struct Request {
    std::filesystem::path path;bool retry{};
    std::filesystem::path expected;bool volume{},browsing{};
    std::optional<std::filesystem::path> volume_answer;
    std::atomic_bool abandoned{};
    std::atomic_bool finished{};
    std::atomic_uint waiters{};
    HANDLE done{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::optional<audio::ArchivePasswordAnswer> answer;
    ~Request(){if(done)CloseHandle(done);if(answer)SecureZeroMemory(answer->password.data(),answer->password.size()*sizeof(wchar_t));}
};
std::atomic<HWND> owner{};
HWND active_dialog{},modal_parent{};bool showing{};
std::mutex mutex;std::deque<std::shared_ptr<Request>> pending;
std::vector<std::weak_ptr<Request>> requests;
INT_PTR CALLBACK Dialog(HWND dialog,UINT message,WPARAM wp,LPARAM lp){
    auto* r=reinterpret_cast<Request*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG){
        r=reinterpret_cast<Request*>(lp);SetWindowLongPtrW(dialog,DWLP_USER,lp);active_dialog=dialog;
        SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_NAME,r->path.filename().c_str());
        SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_STATUS,r->retry?i18n::Literal(L"密码未通过校验，请重试。加密数据损坏也可能导致校验失败。"):i18n::Literal(L"请输入此压缩包的密码："));
        SendDlgItemMessageW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,EM_LIMITTEXT,1024,0);
        if(r->volume){
            SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_STATUS,(i18n::Literal(L"找不到所需分卷：\r\n")+r->expected.native()).c_str());
            SendDlgItemMessageW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,EM_LIMITTEXT,32767,0);
            SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,r->expected.c_str());
        }
        RECT base{},rect{};GetWindowRect(GetParent(dialog),&base);GetWindowRect(dialog,&rect);
        MONITORINFO m{sizeof(m)};GetMonitorInfoW(MonitorFromWindow(GetParent(dialog),MONITOR_DEFAULTTONEAREST),&m);
        const int w=rect.right-rect.left,h=rect.bottom-rect.top;
        SetWindowPos(dialog,nullptr,std::max(m.rcWork.left,std::min(m.rcWork.right-w,base.left+(base.right-base.left-w)/2)),
            std::max(m.rcWork.top,std::min(m.rcWork.bottom-h,base.top+(base.bottom-base.top-h)/2)),0,0,SWP_NOSIZE|SWP_NOZORDER);
        SetTimer(dialog,1,100,nullptr);SetFocus(GetDlgItem(dialog,IDC_ARCHIVE_PASSWORD_EDIT));return FALSE;
    }
    if(message==WM_TIMER && r && (r->abandoned || !owner.load())){
        if(r->browsing){const auto popup=GetLastActivePopup(dialog);if(popup!=dialog)PostMessageW(popup,WM_CLOSE,0,0);}
        else EndDialog(dialog,IDCANCEL);return TRUE;
    }
    if(message==WM_COMMAND && LOWORD(wp)==IDC_ARCHIVE_PASSWORD_REMEMBER && r && r->volume){
        detail::ModernOpenFileOptions options;options.owner=dialog;options.initial_path=r->expected;
        options.title=i18n::Literal(L"选择分卷：")+r->expected.filename().native();options.filters={{i18n::Literal(L"所有文件"),L"*.*"}};
        r->browsing=true;auto selected=detail::ModernOpenFile(options);r->browsing=false;
        if(selected && !r->abandoned)SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,selected->c_str());return TRUE;
    }
    if(message==WM_COMMAND && LOWORD(wp)==IDOK && r){
        if(r->volume){
            std::vector<wchar_t> text(32768);GetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,text.data(),static_cast<int>(text.size()));
            const auto a=GetFileAttributesW(text.data());
            if(a==INVALID_FILE_ATTRIBUTES || (a&FILE_ATTRIBUTE_DIRECTORY)){
                SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_STATUS,i18n::Literal(L"所选文件不存在或无法访问，请重新选择。\r\n取消不会修改压缩包。"));return TRUE;
            }
            r->volume_answer=std::filesystem::absolute(text.data());EndDialog(dialog,IDOK);return TRUE;
        }
        wchar_t text[1025]{};GetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,text,1025);
        r->answer=audio::ArchivePasswordAnswer{text,IsDlgButtonChecked(dialog,IDC_ARCHIVE_PASSWORD_REMEMBER)==BST_CHECKED};
        SecureZeroMemory(text,sizeof(text));SetDlgItemTextW(dialog,IDC_ARCHIVE_PASSWORD_EDIT,L"");EndDialog(dialog,IDOK);return TRUE;
    }
    if(message==WM_CLOSE || message==WM_COMMAND && LOWORD(wp)==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_DESTROY){active_dialog=nullptr;KillTimer(dialog,1);}return FALSE;
}
void Show(const std::shared_ptr<Request>& r){
    if(r->abandoned || !owner.load()){
        r->finished=true;SetEvent(r->done);
        if(const auto window=owner.load())PostMessageW(window,kMsgArchivePassword,0,0);
        return;
    }
    showing=true;
    ShowWtlModalDialog(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(r->volume?IDD_ARCHIVE_VOLUME:IDD_ARCHIVE_PASSWORD),
        IsWindow(modal_parent)?modal_parent:owner.load(),Dialog,reinterpret_cast<LPARAM>(r.get()));
    showing=false;r->finished=true;SetEvent(r->done);if(owner.load())PostMessageW(owner.load(),kMsgArchivePassword,0,0);
}
std::shared_ptr<Request> Submit(const std::filesystem::path& path,const std::filesystem::path& expected,bool retry,const std::function<bool()>& canceled){
    const auto parent=owner.load();if(!parent)return {};
    if(GetCurrentThreadId()==GetWindowThreadProcessId(parent,nullptr)){
        auto r=std::make_shared<Request>();r->path=path;r->retry=retry;r->expected=expected;r->volume=!expected.empty();if(!r->done)return {};
        if(showing)return {};Show(r);return r;
    }
    std::shared_ptr<Request> r;
    {
        std::lock_guard lock(mutex);
        std::erase_if(requests,[](const auto& weak){const auto p=weak.lock();return !p || p->finished || p->abandoned;});
        for(const auto& weak:requests)if(auto p=weak.lock();p && p->retry==retry && !_wcsicmp(p->path.c_str(),path.c_str()) && !_wcsicmp(p->expected.c_str(),expected.c_str())){r=std::move(p);break;}
        if(!r){r=std::make_shared<Request>();r->path=path;r->retry=retry;r->expected=expected;r->volume=!expected.empty();if(!r->done)return {};
            pending.push_back(r);requests.push_back(r);}
        ++r->waiters;
    }
    struct Waiting {Request& r;~Waiting(){if(--r.waiters==0 && !r.finished)r.abandoned=true;}} waiting{*r};
    if(!PostMessageW(parent,kMsgArchivePassword,0,0)){r->abandoned=true;return {};}
    while(WaitForSingleObject(r->done,25)==WAIT_TIMEOUT){
        if(!owner.load() || canceled && canceled())return {};
    }
    return r;
}
std::optional<audio::ArchivePasswordAnswer> Ask(const std::filesystem::path& path,bool retry,const std::function<bool()>& canceled){
    auto r=Submit(path,{},retry,canceled);return r?r->answer:std::nullopt;
}
std::optional<std::filesystem::path> AskVolume(const std::filesystem::path& path,const std::filesystem::path& expected,const std::function<bool()>& canceled){
    auto r=Submit(path,expected,false,canceled);return r?r->volume_answer:std::nullopt;
}
}
void SetArchivePasswordModalParent(HWND window){modal_parent=window;}
void InstallArchivePasswordDialog(HWND window,const std::filesystem::path& store){
    auto path=store;
    if(path.empty()){wchar_t module[32768]{};const auto n=GetModuleFileNameW(nullptr,module,32768);
        if(n && n<32768)path=std::filesystem::path(module).parent_path()/L"ArchivePasswords.dat";}
    owner=window;audio::ConfigureArchivePasswords(path,Ask,[](HRESULT error){
        if(const auto window=owner.load())PostMessageW(window,kMsgArchivePasswordStorageFailure,static_cast<WPARAM>(error),0);
    });
    audio::ConfigureArchiveVolumes(AskVolume);
}
void ReportArchivePasswordStorageFailure(HRESULT error){
    if(const auto window=owner.load()){
        wchar_t code[16]{};swprintf_s(code,L"%08X",static_cast<unsigned>(error));
        const auto text=std::wstring(i18n::Literal(L"密码已验证，但未能保存记住的密码。请检查播放器目录是否可写。\r\n错误代码：0x"))+code;
        MessageBoxW(window,text.c_str(),i18n::Literal(L"压缩包密码"),MB_OK|MB_ICONWARNING);
    }
}
void DispatchArchivePasswordDialog(){
    if(showing)return;std::shared_ptr<Request> next;
    {std::lock_guard lock(mutex);if(!pending.empty()){next=std::move(pending.front());pending.pop_front();}}
    if(next)Show(next);
}
void CloseArchivePasswordDialog(){
    owner=nullptr;modal_parent=nullptr;
    if(active_dialog){auto* r=reinterpret_cast<Request*>(GetWindowLongPtrW(active_dialog,DWLP_USER));
        if(r && r->browsing){r->abandoned=true;const auto popup=GetLastActivePopup(active_dialog);if(popup!=active_dialog)PostMessageW(popup,WM_CLOSE,0,0);}
        else EndDialog(active_dialog,IDCANCEL);}
    std::lock_guard lock(mutex);for(auto& r:pending){r->abandoned=true;SetEvent(r->done);}pending.clear();
}
}
