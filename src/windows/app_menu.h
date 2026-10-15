#pragma once
#include "platform/com_ptr.h"
#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>

namespace minima {
inline std::wstring menu_label(std::wstring value) {
    std::wstring result;for(const auto c:value){if(c==L'&')result+=L'&';result+=(c==L'\n'||c==L'\r'||c==L'\t')?L' ':c;}
    if(result.size()>100)result=result.substr(0,97)+L"…";return result;
}
// Hosts the item's actual Shell extension commands and forwards their menu events.
class ShellItemMenu {
public:
    static constexpr UINT first=10000,last=29999;
    ComPtr<IContextMenu> menu;
    ComPtr<IContextMenu2> menu2;
    ComPtr<IContextMenu3> menu3;
    HMENU popup=nullptr;
    ShellItemMenu()=default;
    ShellItemMenu(const ShellItemMenu&)=delete;
    ShellItemMenu& operator=(const ShellItemMenu&)=delete;
    ~ShellItemMenu(){if(popup)DestroyMenu(popup);}
    // Builds the item's native menu (Open, Run as administrator, Open file location, ...).
    bool load(HWND owner,const std::wstring& path,bool extended=false) {
        if(popup)return true;
        PIDLIST_ABSOLUTE item=nullptr;
        if(path.empty()||FAILED(SHParseDisplayName(path.c_str(),nullptr,&item,0,nullptr)))return false;
        ComPtr<IShellFolder> folder;PCUITEMID_CHILD child=nullptr;
        HRESULT hr=SHBindToParent(item,IID_PPV_ARGS(folder.put()),&child);
        if(SUCCEEDED(hr))hr=folder->GetUIObjectOf(owner,1,&child,IID_IContextMenu,nullptr,reinterpret_cast<void**>(menu.put()));
        CoTaskMemFree(item);if(FAILED(hr))return false;
        popup=CreatePopupMenu();if(!popup){menu.reset();return false;}
        hr=menu->QueryContextMenu(popup,0,first,last,CMF_NORMAL|(extended?CMF_EXTENDEDVERBS:0));
        if(FAILED(hr)||GetMenuItemCount(popup)<=0){DestroyMenu(popup);popup=nullptr;menu.reset();return false;}
        menu->QueryInterface(IID_PPV_ARGS(menu3.put()));menu->QueryInterface(IID_PPV_ARGS(menu2.put()));
        return true;
    }
    bool message(UINT msg,WPARAM w,LPARAM l,LRESULT& result) {
        if(msg!=WM_INITMENUPOPUP&&msg!=WM_DRAWITEM&&msg!=WM_MEASUREITEM&&msg!=WM_MENUCHAR)return false;
        if(menu3)return SUCCEEDED(menu3->HandleMenuMsg2(msg,w,l,&result));
        if(menu2&&msg!=WM_MENUCHAR){result=0;return SUCCEEDED(menu2->HandleMenuMsg(msg,w,l));}return false;
    }
    HRESULT invoke(HWND owner,UINT selected,POINT point) {
        if(!menu||selected<first||selected>last)return E_INVALIDARG;
        CMINVOKECOMMANDINFOEX command{};command.cbSize=sizeof(command);command.hwnd=owner;
        command.fMask=CMIC_MASK_UNICODE|CMIC_MASK_PTINVOKE;
        command.lpVerb=MAKEINTRESOURCEA(selected-first);command.lpVerbW=MAKEINTRESOURCEW(selected-first);
        command.nShow=SW_SHOWNORMAL;command.ptInvoke=point;
        return menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command));
    }
};
struct RecentItem {std::wstring title;ComPtr<IShellItem> item;};
// Windows' own jump-list history for the app: Recent, or Frequent for apps that only track that.
inline std::vector<RecentItem> recent_items(const std::wstring& app_id,bool* frequent=nullptr) {
    std::vector<RecentItem> items;if(frequent)*frequent=false;if(app_id.empty())return items;
    ComPtr<IApplicationDocumentLists> documents;
    if(FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(documents.put())))||FAILED(documents->SetAppID(app_id.c_str())))return items;
    for(const auto type:{ADLT_RECENT,ADLT_FREQUENT}) {
        ComPtr<IObjectArray> array;if(FAILED(documents->GetList(type,10,IID_PPV_ARGS(array.put()))))continue;
        UINT count=0;array->GetCount(&count);
        for(UINT i=0;i<std::min(count,10u);++i){ComPtr<IShellItem> item;if(FAILED(array->GetAt(i,IID_PPV_ARGS(item.put()))))continue;PWSTR title=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY,&title))){items.push_back({title,std::move(item)});CoTaskMemFree(title);}}
        if(!items.empty()){if(frequent)*frequent=type==ADLT_FREQUENT;break;}
    }
    return items;
}
inline bool open_recent_item(HWND owner,IShellItem* item) {
    PIDLIST_ABSOLUTE id=nullptr;if(FAILED(SHGetIDListFromObject(item,&id)))return false;
    SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_IDLIST;info.hwnd=owner;info.lpVerb=L"open";info.lpIDList=id;info.nShow=SW_SHOWNORMAL;
    const bool opened=ShellExecuteExW(&info)!=FALSE;CoTaskMemFree(id);return opened;
}
// "Remove from this list" for a jump-list history item.
inline bool remove_recent_item(const std::wstring& app_id,IShellItem* item) {
    ComPtr<IApplicationDestinations> destinations;
    if(app_id.empty()||!item||FAILED(CoCreateInstance(CLSID_ApplicationDestinations,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(destinations.put())))||
       FAILED(destinations->SetAppID(app_id.c_str())))return false;
    return SUCCEEDED(destinations->RemoveDestination(item));
}
// Menu item images: the Shell's own icon for an item, else the app's window icon.
inline HBITMAP menu_bitmap(IShellItem* item,int size) {
    ComPtr<IShellItemImageFactory> factory;HBITMAP bitmap=nullptr;
    if(item&&SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(factory.put()))))factory->GetImage(SIZE{size,size},SIIGBF_ICONONLY,&bitmap);
    return bitmap;
}
inline HBITMAP menu_bitmap(HICON icon,int size) {
    if(!icon)return nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;const auto dc=CreateCompatibleDC(nullptr);if(!dc)return nullptr;
    const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(bitmap){const auto old=SelectObject(dc,bitmap);DrawIconEx(dc,0,0,icon,size,size,0,nullptr,DI_NORMAL);SelectObject(dc,old);}
    DeleteDC(dc);return bitmap;
}
// Mirrors the Windows 11 taskbar: right-click shows the jump list (Recent, app,
// pin, close); Shift+right-click shows the window menu. Right-clicking the app
// entry opens its native Shell menu (Run as administrator, Open file location).
class AppContextMenu {
public:
    enum : UINT {launch_id=1,pin_id=2,close_id=3,restore_all_id=4,minimize_all_id=5,window_first=100,recent_first=3000};
    struct WindowCommand {UINT id;HWND window;UINT command;};
    HMENU menu=CreatePopupMenu();
    ShellItemMenu shell;
    std::vector<WindowCommand> commands;
    std::vector<RecentItem> recent;
    std::vector<HBITMAP> bitmaps;
    int launch_position=-1; // Menu index of the app entry, for its right-click Shell menu.
    bool window_menu=false;
    AppContextMenu(HWND owner,const std::wstring& name,const std::wstring& path,const std::wstring& app_id,
                   HICON icon,const std::vector<HWND>& windows,bool pinned,bool window_commands=false) {
        if(!menu)return;
        std::vector<HWND> open;for(const auto window:windows)if(IsWindow(window))open.push_back(window);
        const UINT dpi=owner?GetDpiForWindow(owner):96;
        const int size=GetSystemMetricsForDpi(SM_CXSMICON,dpi?dpi:96);
        if(window_commands&&!open.empty()){window_menu=true;build_window_menu(open);return;}
        bool frequent=false;recent=recent_items(app_id,&frequent);
        if(!recent.empty()) {
            AppendMenuW(menu,MF_STRING|MF_GRAYED,0,frequent?L"Frequent":L"Recent");
            for(size_t i=0;i<recent.size();++i) {
                const auto title=menu_label(recent[i].title);
                AppendMenuW(menu,MF_STRING,recent_first+static_cast<UINT>(i),title.c_str());
                image(recent_first+static_cast<UINT>(i),menu_bitmap(recent[i].item.get(),size));
            }
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        }
        launch_position=GetMenuItemCount(menu);
        const auto label=menu_label(name);
        AppendMenuW(menu,MF_STRING|(path.empty()?MF_GRAYED:0),launch_id,label.c_str());
        HBITMAP app=nullptr;
        if(!path.empty()){ComPtr<IShellItem> item;if(SUCCEEDED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(item.put()))))app=menu_bitmap(item.get(),size);}
        image(launch_id,app?app:menu_bitmap(icon,size));
        AppendMenuW(menu,MF_STRING|(path.empty()?MF_GRAYED:0),pin_id,pinned?L"Unpin from taskbar":L"Pin to taskbar");
        if(!open.empty()){AppendMenuW(menu,MF_STRING,close_id,open.size()>1?L"Close all windows":L"Close window");}
    }
    ~AppContextMenu(){if(menu)DestroyMenu(menu);for(const auto bitmap:bitmaps)DeleteObject(bitmap);}
    AppContextMenu(const AppContextMenu&)=delete;
    AppContextMenu& operator=(const AppContextMenu&)=delete;
private:
    void image(UINT id,HBITMAP bitmap) {
        if(!bitmap)return;bitmaps.push_back(bitmap);
        MENUITEMINFOW info{};info.cbSize=sizeof(info);info.fMask=MIIM_BITMAP;info.hbmpItem=bitmap;SetMenuItemInfoW(menu,id,FALSE,&info);
    }
    // One window: its own system menu commands with their live enabled state.
    // Several: the group commands Windows offered for stacked buttons.
    void build_window_menu(const std::vector<HWND>& open) {
        if(open.size()>1) {
            AppendMenuW(menu,MF_STRING,restore_all_id,L"Restore all windows");
            AppendMenuW(menu,MF_STRING,minimize_all_id,L"Minimize all windows");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,close_id,L"Close all windows");
            return;
        }
        const auto window=open.front();const auto system=GetSystemMenu(window,FALSE);UINT id=window_first;
        constexpr UINT actions[]={SC_RESTORE,SC_MOVE,SC_SIZE,SC_MINIMIZE,SC_MAXIMIZE,SC_CLOSE};
        for(const auto action:actions) {
            if(!system)break;const auto state=GetMenuState(system,action,MF_BYCOMMAND);
            if(state==static_cast<UINT>(-1))continue;
            wchar_t text[128]{};if(!GetMenuStringW(system,action,text,128,MF_BYCOMMAND))continue;
            if(action==SC_CLOSE)AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING|(state&(MF_DISABLED|MF_GRAYED)),id,text);commands.push_back({id++,window,action});
        }
        if(commands.empty()){AppendMenuW(menu,MF_STRING,close_id,L"Close window");}
    }
};
}
