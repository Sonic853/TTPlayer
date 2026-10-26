#pragma once

#include "../app/resource_ids.h"
#include "ttplayer/i18n/i18n.h"
#include "ttplayer/settings/settings.h"
#include <algorithm>
#include <windows.h>
#include <commctrl.h>

namespace ttplayer::ui::detail {
// Coordinates use the original album section's space. FullscreenOptionsTabs
// moves these HWNDs into the visual tab along with the existing album controls.
class DreamFullscreenOptions {
public:
    static void EnableCustom(HWND page, bool custom) {
        for(const int id : {IDC_FULLSCREEN_DREAM_WIDTH,IDC_FULLSCREEN_DREAM_HEIGHT}) {
            const HWND control=GetDlgItem(page,id);
            EnableWindow(control,custom);
            RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
        }
    }

    static bool CommitNumber(HWND page, int id, settings::FullScreenSettings& value) {
        int* field=id==IDC_FULLSCREEN_DREAM_WIDTH?&value.dream_width:
            id==IDC_FULLSCREEN_DREAM_HEIGHT?&value.dream_height:
            id==IDC_FULLSCREEN_DREAM_FPS?&value.dream_frames_per_second:nullptr;
        if(!field || !GetDlgItem(page,id))return false;
        BOOL valid{};
        const int entered=static_cast<int>(GetDlgItemInt(page,id,&valid,TRUE));
        const int next=valid?std::clamp(entered,id==IDC_FULLSCREEN_DREAM_FPS?10:settings::FullScreenSettings::dream_min_size,
            id==IDC_FULLSCREEN_DREAM_FPS?120:settings::FullScreenSettings::dream_max_size):*field;
        const bool changed=*field!=next;
        *field=next;
        SetDlgItemInt(page,id,next,FALSE);
        return changed;
    }

    static void Commit(HWND page, settings::FullScreenSettings& value) {
        for(const int id : {IDC_FULLSCREEN_DREAM_WIDTH,IDC_FULLSCREEN_DREAM_HEIGHT,IDC_FULLSCREEN_DREAM_FPS})
            CommitNumber(page,id,value);
    }

    static void Populate(HWND page, HINSTANCE instance, const settings::FullScreenSettings& value) {
        if(!GetDlgItem(page,IDC_FULLSCREEN_DREAM_RESOLUTION)) {
            const auto add=[&](LPCWSTR klass, UINT label, int id, DWORD style, RECT bounds, DWORD ex=0) {
                MapDialogRect(page,&bounds);
                const auto text=label?i18n::ResourceText(GetModuleHandleW(nullptr),label):std::wstring{};
                const HWND control=CreateWindowExW(ex,klass,text.c_str(),WS_CHILD|WS_VISIBLE|style,
                    bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,page,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
                SendMessageW(control,WM_SETFONT,SendMessageW(page,WM_GETFONT,0,0),FALSE);
                return control;
            };
            const auto number=[&](int edit_id,int spin_id,RECT bounds,int low,int high) {
                const HWND edit=add(WC_EDITW,0,edit_id,WS_TABSTOP|WS_CLIPSIBLINGS|ES_NUMBER|ES_AUTOHSCROLL,bounds,WS_EX_CLIENTEDGE);
                SendMessageW(edit,EM_SETLIMITTEXT,4,0);
                if(!spin_id)return;
                const HWND spin=add(UPDOWN_CLASSW,0,spin_id,
                    UDS_ALIGNRIGHT|UDS_SETBUDDYINT|UDS_ARROWKEYS|UDS_NOTHOUSANDS,bounds);
                SendMessageW(spin,UDM_SETBUDDY,reinterpret_cast<WPARAM>(edit),0);
                SendMessageW(spin,UDM_SETRANGE32,low,high);
                // Give the spin the overlapping buddy-border pixels.
                SetWindowPos(spin,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            };
            add(WC_BUTTONW,IDS_FULLSCREEN_DREAM_GROUP,-1,BS_GROUPBOX,{7,218,273,282});
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_RESOLUTION,-1,0,{13,231,77,244});
            const HWND combo=add(WC_COMBOBOXW,0,IDC_FULLSCREEN_DREAM_RESOLUTION,
                WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,{78,228,168,295});
            for(const auto* text : {L"320x240",L"800x800"})
                SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
            const auto custom=i18n::ResourceText(GetModuleHandleW(nullptr),IDS_FULLSCREEN_DREAM_CUSTOM);
            SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(custom.c_str()));
            add(WC_BUTTONW,IDS_FULLSCREEN_DREAM_STRETCH,IDC_FULLSCREEN_DREAM_STRETCH,
                WS_TABSTOP|BS_AUTOCHECKBOX,{177,231,267,244});
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_WIDTH,-1,0,{13,248,30,261});
            number(IDC_FULLSCREEN_DREAM_WIDTH,0,{32,245,91,259},2,4096);
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_HEIGHT,-1,0,{100,248,117,261});
            number(IDC_FULLSCREEN_DREAM_HEIGHT,0,{119,245,178,259},2,4096);
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_SIZE_UNIT,-1,0,{185,248,267,261});
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_FPS,-1,0,{13,265,77,278});
            number(IDC_FULLSCREEN_DREAM_FPS,IDC_FULLSCREEN_DREAM_FPS_SPIN,{78,262,137,276},10,120);
            add(WC_STATICW,IDS_FULLSCREEN_DREAM_FPS_UNIT,-1,0,{144,265,267,278});
        }
        SendDlgItemMessageW(page,IDC_FULLSCREEN_DREAM_RESOLUTION,CB_SETCURSEL,std::clamp(value.dream_resolution,0,2),0);
        SendDlgItemMessageW(page,IDC_FULLSCREEN_DREAM_STRETCH,BM_SETCHECK,value.dream_stretch?BST_CHECKED:BST_UNCHECKED,0);
        SetDlgItemInt(page,IDC_FULLSCREEN_DREAM_WIDTH,value.dream_width,FALSE);
        SetDlgItemInt(page,IDC_FULLSCREEN_DREAM_HEIGHT,value.dream_height,FALSE);
        SetDlgItemInt(page,IDC_FULLSCREEN_DREAM_FPS,value.dream_frames_per_second,FALSE);
        EnableCustom(page,value.dream_resolution==2);
    }
};
}
