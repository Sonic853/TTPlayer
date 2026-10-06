#include "disc_lookup_dialog.h"
#include "ttplayer/ui/wtl_dialogs.h"
#include "ttplayer/core/text.h"
#include <commctrl.h>
#include <objbase.h>
#include <future>
#include <atomic>

namespace ttplayer::ui {
namespace {
constexpr int kDialog=5901,kCandidates=5902,kTracks=5903,kStatus=5904,kMatch=5905;
struct State {
    std::filesystem::path path;const plugins::PluginManager* manager{};HMODULE comm{};
    settings::NetworkSettings network;audio::DiscQuerySource source;
    std::vector<audio::DiscCandidate> candidates;std::optional<audio::DiscRelease> selected;
    std::future<void> worker;std::atomic_bool cancel{};std::wstring error;
    int operation{};bool closing{},saved{},automatic{};
};
void Status(HWND dialog,const wchar_t* text){SetDlgItemTextW(dialog,kStatus,text);}
template<class Work> void Start(HWND dialog,State& s,int operation,Work work) {
    if(s.worker.valid())return;
    s.operation=operation;s.error.clear();s.cancel=false;
    EnableWindow(GetDlgItem(dialog,IDOK),FALSE);EnableWindow(GetDlgItem(dialog,kCandidates),FALSE);
    Status(dialog,operation==1?L"正在读取光盘布局并查询 MusicBrainz…":operation==2?L"正在读取所选发行版的曲目…":L"正在保存曲目信息…");
    try {
        s.worker=std::async(std::launch::async,[&s,work]{
            const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            try {work();}catch(const std::exception& e){try{s.error=core::Utf8ToWide(e.what());}catch(...){s.error=L"操作失败";}}
            catch(...){s.error=L"操作失败";}
            if(SUCCEEDED(hr))CoUninitialize();
        });
        SetTimer(dialog,1,50,nullptr);
    }catch(...){Status(dialog,L"无法启动查询线程");}
}
void Select(HWND dialog,State& s) {
    const auto index=SendDlgItemMessageW(dialog,kCandidates,CB_GETCURSEL,0,0);
    if(index<0 || static_cast<size_t>(index)>=s.candidates.size())return;
    s.selected.reset();ListView_DeleteAllItems(GetDlgItem(dialog,kTracks));
    const auto candidate=s.candidates[index];
    SetDlgItemTextW(dialog,kMatch,candidate.exact?L"Disc ID 匹配。请核对发行版与曲目信息。":L"近似匹配：请核对整张光盘和每首曲目后再保存。");
    Start(dialog,s,2,[&s,candidate]{s.selected=audio::ReadMusicBrainzRelease(candidate,s.source.layout,s.network,[&s]{return s.cancel.load();});});
}
void Close(HWND dialog,State& s) {
    if(s.worker.valid()) {s.closing=true;s.cancel=true;EnableWindow(GetDlgItem(dialog,IDCANCEL),FALSE);Status(dialog,L"正在取消，请稍候…");}
    else EndDialog(dialog,s.saved?IDOK:IDCANCEL);
}
INT_PTR CALLBACK Proc(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<State*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,lparam);
        const auto list=GetDlgItem(dialog,kTracks);ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        RECT rect{};GetClientRect(list,&rect);
        const wchar_t* names[]={L"音轨",L"标题",L"艺术家"};
        for(int i=0;i<3;++i){LVCOLUMNW column{LVCF_TEXT|LVCF_WIDTH};column.pszText=const_cast<wchar_t*>(names[i]);column.cx=i==0?48:(rect.right-52)/2;ListView_InsertColumn(list,i,&column);}
        RECT owner{},window{};GetWindowRect(GetParent(dialog),&owner);GetWindowRect(dialog,&window);
        SetWindowPos(dialog,nullptr,owner.left+(owner.right-owner.left-window.right+window.left)/2,
            owner.top+(owner.bottom-owner.top-window.bottom+window.top)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
        Start(dialog,*state,1,[state]{
            state->source=audio::PrepareDiscQuery(state->path,state->manager,state->comm,{},[state]{return state->cancel.load();});
            state->candidates=audio::QueryMusicBrainz(state->source.layout,state->network,[state]{return state->cancel.load();});
        });return TRUE;
    }
    if(!state)return FALSE;auto& s=*state;
    if(message==WM_TIMER && wparam==1 && s.worker.valid() && s.worker.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
        s.worker.get();KillTimer(dialog,1);
        if(s.closing){EndDialog(dialog,s.saved?IDOK:IDCANCEL);return TRUE;}
        EnableWindow(GetDlgItem(dialog,kCandidates),TRUE);
        if(!s.error.empty()) {
            if(s.automatic && s.operation!=3 && !s.network.show_info_when_fail) {EndDialog(dialog,IDCANCEL);return TRUE;}
            Status(dialog,s.error.c_str());if(s.operation==3)EnableWindow(GetDlgItem(dialog,IDOK),TRUE);return TRUE;
        }
        if(s.operation==1) {
            for(const auto& item:s.candidates) {
                const auto title=item.album+L" — "+item.artist+L" ["+item.date+L" "+item.country+L"] 第 "+std::to_wstring(item.medium)+L" 碟 "+item.label;
                SendDlgItemMessageW(dialog,kCandidates,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));
            }
            if(s.candidates.empty())Status(dialog,L"没有找到匹配的发行版。未修改曲目信息。");
            else {SendDlgItemMessageW(dialog,kCandidates,CB_SETCURSEL,0,0);Select(dialog,s);}
        } else if(s.operation==2 && s.selected) {
            const auto list=GetDlgItem(dialog,kTracks);int row{};
            for(const auto& [number,fields]:s.selected->tracks) {
                auto n=std::to_wstring(number),title=audio::DiscField(fields,L"Title"),artist=audio::DiscField(fields,L"Artist");
                LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=n.data();ListView_InsertItem(list,&item);
                ListView_SetItemText(list,row,1,title.data());ListView_SetItemText(list,row,2,artist.data());++row;
            }
            Status(dialog,L"保存将更新所示歌曲信息，服务未返回的字段保留原值。");EnableWindow(GetDlgItem(dialog,IDOK),TRUE);
        } else if(s.operation==3)EndDialog(dialog,IDOK);
        return TRUE;
    }
    if(message==WM_COMMAND) {
        if(LOWORD(wparam)==IDCANCEL){Close(dialog,s);return TRUE;}
        if(s.worker.valid())return TRUE;
        if(LOWORD(wparam)==kCandidates && HIWORD(wparam)==CBN_SELCHANGE){Select(dialog,s);return TRUE;}
        if(LOWORD(wparam)==IDOK && s.selected) {
            Start(dialog,s,3,[&s]{audio::SaveDiscRelease(s.source,*s.selected);s.saved=true;});return TRUE;
        }
    }
    if(message==WM_CLOSE){Close(dialog,s);return TRUE;}
    return FALSE;
}
}
std::optional<audio::DiscRelease> ShowDiscLookupDialog(HWND owner,const std::filesystem::path& path,
    const plugins::PluginManager* manager,HMODULE comm,const settings::NetworkSettings& network,bool automatic) {
    State state;state.path=path;state.manager=manager;state.comm=comm;state.network=network;state.automatic=automatic;
    ShowWtlModalDialog(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(kDialog),owner,Proc,reinterpret_cast<LPARAM>(&state));
    state.cancel=true;if(state.worker.valid())state.worker.wait();
    return state.saved?state.selected:std::nullopt;
}
}
