#include "windows/tray.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace minima {
namespace {
// Windows 11 serializes notification data with 32-bit USER handles even for
// x64 senders. Parse the wire fields, never dereference sender-owned pointers.
struct WireIcon {
    DWORD size,window,id,flags,message,icon;
    WCHAR tip[128];DWORD state,mask;WCHAR info[256];UINT version;
    WCHAR info_title[64];DWORD info_flags;GUID guid;DWORD balloon;
};
static_assert(sizeof(WireIcon)==956);
constexpr DWORD notify_signature=0x34753423;
unsigned long long created(HANDLE p){FILETIME a{},b{},c{},d{};if(!GetProcessTimes(p,&a,&b,&c,&d))return 0;return (static_cast<unsigned long long>(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
bool identity(HWND window,DWORD& pid,unsigned long long& time) {
    DWORD session=0,ours=0;GetWindowThreadProcessId(window,&pid);
    if(!pid||!ProcessIdToSessionId(pid,&session)||!ProcessIdToSessionId(GetCurrentProcessId(),&ours)||session!=ours)return false;
    auto p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!p)return false;time=created(p);CloseHandle(p);return time!=0;
}
HWND explorer_bar() {
    HWND result=nullptr;EnumWindows([](HWND w,LPARAM l)->BOOL{wchar_t name[128]{};GetClassNameW(w,name,128);if(wcscmp(name,L"Shell_TrayWnd"))return TRUE;
        DWORD pid=0;unsigned long long time=0;if(!identity(w,pid,time)||pid==GetCurrentProcessId())return TRUE;
        auto p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);wchar_t image[MAX_PATH]{},windows[MAX_PATH]{};DWORD size=MAX_PATH;
        // explorer.exe is in the Windows directory, always shorter than MAX_PATH;
        // a longer image path fails the query and cannot be Explorer.
        const bool ok=p&&QueryFullProcessImageNameW(p,0,image,&size)&&GetWindowsDirectoryW(windows,MAX_PATH)&&_wcsicmp(image,(std::wstring(windows)+L"\\explorer.exe").c_str())==0;if(p)CloseHandle(p);
        if(ok){*reinterpret_cast<HWND*>(l)=w;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&result));return result;
}
HWND handle(DWORD value){return reinterpret_cast<HWND>(static_cast<LONG_PTR>(static_cast<LONG>(value)));}
}
struct TrayPopup::Impl {
    struct Item {TrayIcon view;HWND owner=nullptr;DWORD pid=0,source_icon=0;unsigned long long time=0;UINT id=0,message=0,version=0;GUID guid{};RECT bounds{};bool by_guid=false,hidden=false;};
    std::jthread worker;mutable std::mutex mutex;std::vector<Item> items;
    std::atomic<HWND> host=nullptr;HWND notify=nullptr,explorer=nullptr;DWORD explorer_pid=0;unsigned long long explorer_time=0;
    std::atomic_bool prepared=false,usable=false,opened=false;std::atomic<HRESULT> result=E_PENDING;
    std::atomic<DWORD> context_thread=0,context_pid=0;std::atomic<ULONGLONG> context_until=0;
    std::vector<HWND> context_before; // The app's windows before its menu opened (guarded by mutex).
    unsigned long long next_key=1;UINT taskbar_created=0;
    ~Impl(){stop();}
    bool original_valid() const {DWORD pid=0;unsigned long long time=0;return identity(explorer,pid,time)&&pid==explorer_pid&&time==explorer_time;}
    void changed(){PostMessageW(notify,tray_ready_message,0,0);}
    static bool valid(const Item& i){DWORD pid=0;unsigned long long time=0;return identity(i.owner,pid,time)&&pid==i.pid&&time==i.time;}
    bool receive(const COPYDATASTRUCT& data) {
        if(data.dwData!=1||!data.lpData||data.cbData<8+offsetof(WireIcon,tip)||data.cbData>4096)return false;
        DWORD header[2]{};std::memcpy(header,data.lpData,8);if(header[0]!=notify_signature||header[1]>NIM_SETVERSION)return false;
        WireIcon wire{};std::memcpy(&wire,static_cast<const BYTE*>(data.lpData)+8,std::min<size_t>(data.cbData-8,sizeof(wire)));
        if((wire.size!=152&&wire.size!=936&&wire.size!=952&&wire.size!=956)||data.cbData<8+wire.size)return false;
        if((wire.flags&NIF_GUID)&&wire.size<offsetof(WireIcon,balloon))return false;
        std::lock_guard lock(mutex);const bool guid=(wire.flags&NIF_GUID)!=0;
        auto found=std::find_if(items.begin(),items.end(),[&](const Item& i){return guid?i.by_guid&&IsEqualGUID(i.guid,wire.guid):i.owner==handle(wire.window)&&i.id==wire.id;});
        const auto owner=wire.window?handle(wire.window):(guid&&found!=items.end()?found->owner:nullptr);DWORD pid=0;unsigned long long time=0;if(!identity(owner,pid,time))return false;
        if(found!=items.end()&&(found->pid!=pid||found->time!=time))return false;
        if(header[1]==NIM_DELETE){if(found==items.end())return false;items.erase(found);changed();return true;}
        if(header[1]==NIM_SETFOCUS)return found!=items.end();
        if(header[1]==NIM_SETVERSION){if(found==items.end()||wire.version>4)return false;found->version=wire.version;return true;}
        if(found==items.end()){if(header[1]!=NIM_ADD||items.size()>=256)return false;Item item;item.view.key=next_key++;item.owner=owner;item.pid=pid;item.time=time;item.id=wire.id;item.by_guid=guid;item.guid=wire.guid;items.push_back(std::move(item));found=items.end()-1;}
        if(header[1]==NIM_ADD){found->version=0;found->owner=owner;found->id=wire.id;}
        if(wire.flags&NIF_MESSAGE){if(wire.message>=WM_USER&&wire.message<=0xffff)found->message=wire.message;}
        if((wire.flags&NIF_ICON)&&(header[1]==NIM_ADD||wire.icon!=found->source_icon)){const auto copied=CopyIcon(reinterpret_cast<HICON>(handle(wire.icon)));if(copied){found->source_icon=wire.icon;found->view.icon=NativeIcon(copied,[](void* p){DestroyIcon(static_cast<HICON>(p));});}}
        if(wire.flags&NIF_TIP){wire.tip[127]=0;found->view.title=wire.tip;}
        if((wire.flags&NIF_STATE)&&(wire.mask&NIS_HIDDEN))found->hidden=(wire.state&NIS_HIDDEN)!=0;
        changed();return true;
    }
    LRESULT message(HWND w,UINT m,WPARAM p,LPARAM l) {
        if(m==WM_COPYDATA&&l){const auto data=reinterpret_cast<const COPYDATASTRUCT*>(l);const bool accepted=receive(*data);DWORD_PTR forwarded=0;
            if(data->dwData==3&&data->lpData&&data->cbData==40){DWORD fields[6]{};GUID guid{};std::memcpy(fields,data->lpData,24);std::memcpy(&guid,static_cast<const BYTE*>(data->lpData)+24,16);
                if(fields[0]==notify_signature&&(fields[1]==1||fields[1]==2)&&fields[2]==32){std::lock_guard lock(mutex);const bool by_guid=!IsEqualGUID(guid,GUID{});const auto found=std::find_if(items.begin(),items.end(),[&](const Item& i){return by_guid?i.by_guid&&IsEqualGUID(i.guid,guid):i.owner==handle(fields[4])&&i.id==fields[5];});
                    if(found!=items.end()&&found->bounds.right>found->bounds.left){const auto r=found->bounds;return fields[1]==1?MAKELONG(r.left,r.top):MAKELONG(r.right-r.left,r.bottom-r.top);}}
            }
            if(original_valid())SendMessageTimeoutW(explorer,m,p,l,SMTO_ABORTIFHUNG|SMTO_BLOCK,200,&forwarded);
            return accepted?TRUE:static_cast<LRESULT>(forwarded);
        }
        if(m==WM_TIMER){
            if(FindWindowW(L"Shell_TrayWnd",nullptr)!=w)SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            {std::lock_guard lock(mutex);const auto count=items.size();std::erase_if(items,[](const Item& i){return !valid(i);});if(count!=items.size())changed();}
            return 0;
        }
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
        return DefWindowProcW(w,m,p,l);
    }
    static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM p,LPARAM l){auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,p,l);try{return self->message(w,m,p,l);}catch(...){return 0;}}
    void run(std::stop_token stop) {
        if(stop.stop_requested())return;
        explorer=explorer_bar();if(!explorer||!identity(explorer,explorer_pid,explorer_time)){result=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);prepared=true;changed();return;}
        WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"Shell_TrayWnd";
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){result=HRESULT_FROM_WIN32(GetLastError());prepared=true;changed();return;}
        const auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,klass.lpszClassName,L"MinimaDock notification receiver",WS_POPUP,0,0,0,0,nullptr,nullptr,klass.hInstance,this);
        if(!window){result=HRESULT_FROM_WIN32(GetLastError());prepared=true;changed();return;}
        host=window;SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");usable=true;result=S_OK;prepared=true;changed();SetTimer(window,1,2000,nullptr);
        // Apps re-register with our receiver. Forwarding preserves Explorer's
        // copy throughout the trial, including a dock crash or timeout.
        if(taskbar_created&&!stop.stop_requested())SendNotifyMessageW(HWND_BROADCAST,taskbar_created,0,0);
        if(stop.stop_requested())DestroyWindow(window);
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
        KillTimer(window,1);host=nullptr;usable=false;opened=false;
        if(taskbar_created&&original_valid())SendNotifyMessageW(HWND_BROADCAST,taskbar_created,0,0);
    }
    void stop(){if(worker.joinable())worker.request_stop();if(const auto w=host.load())PostMessageW(w,WM_CLOSE,0,0);if(worker.joinable())worker.join();opened=false;std::lock_guard lock(mutex);items.clear();}
};
TrayPopup::TrayPopup():impl_(std::make_unique<Impl>()){}
TrayPopup::~TrayPopup()=default;
void TrayPopup::prepare(HWND notify){impl_->stop();impl_->notify=notify;impl_->prepared=false;impl_->usable=false;impl_->result=E_PENDING;auto impl=impl_.get();impl_->worker=std::jthread([impl](std::stop_token stop){try{impl->run(stop);}catch(...){if(const auto w=impl->host.exchange(nullptr))DestroyWindow(w);impl->result=E_FAIL;impl->usable=false;impl->prepared=true;impl->changed();}});}
void TrayPopup::stop(){impl_->stop();}
bool TrayPopup::ready() const{return impl_->prepared;}
bool TrayPopup::available() const{return impl_->usable;}
HRESULT TrayPopup::error() const{return impl_->result;}
bool TrayPopup::open(RECT,UINT){if(!available())return false;impl_->opened=true;return true;}
void TrayPopup::close(){impl_->opened=false;impl_->context_thread=0;impl_->context_pid=0;impl_->context_until=0;}
bool TrayPopup::tick(){return impl_->opened&&available();}
bool TrayPopup::owns(HWND window) const{return window&&window==impl_->notify;}
HWND TrayPopup::window() const{return impl_->opened?impl_->notify:nullptr;}
HWND TrayPopup::receiver() const{return impl_->host;}
namespace {
std::vector<HWND> process_windows(DWORD pid) {
    struct Search {DWORD pid;std::vector<HWND> found;} search{pid,{}};
    EnumWindows([](HWND window,LPARAM value)->BOOL{
        auto& s=*reinterpret_cast<Search*>(value);DWORD owner=0;GetWindowThreadProcessId(window,&owner);
        if(owner==s.pid&&IsWindowVisible(window))s.found.push_back(window);return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.found;
}
// A tray menu the app draws itself (Steam, Electron/Chromium, NVIDIA, ...): a new,
// visible, captionless popup of the app's process, menu-sized, or a standard
// menu window (#32768) on any of its threads.
bool app_menu_open(DWORD pid,const std::vector<HWND>& before) {
    for(const auto window:process_windows(pid)) {
        wchar_t name[16]{};GetClassNameW(window,name,16);
        if(wcscmp(name,L"#32768")==0)return true;
        if(std::find(before.begin(),before.end(),window)!=before.end())continue;
        const auto style=static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE));
        if(!(style&WS_POPUP)||(style&WS_CAPTION)==WS_CAPTION)continue;
        RECT r{};MONITORINFO info{sizeof(info),{},{},0};
        if(!GetWindowRect(window,&r)||!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info))continue;
        const auto width=r.right-r.left,height=r.bottom-r.top;
        if(width>0&&height>0&&width*2<=info.rcMonitor.right-info.rcMonitor.left&&height*5<=(info.rcMonitor.bottom-info.rcMonitor.top)*4)return true;
    }
    return false;
}
}
bool TrayPopup::holds_menu() const {
    const auto thread=impl_->context_thread.load();if(!thread)return false;
    if(GetTickCount64()<impl_->context_until)return true;
    GUITHREADINFO info{};info.cbSize=sizeof(info);
    if(GetGUIThreadInfo(thread,&info)&&(info.flags&(GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE)))return true;
    const auto pid=impl_->context_pid.load();if(!pid)return false;
    std::vector<HWND> before;{std::lock_guard lock(impl_->mutex);before=impl_->context_before;}
    return app_menu_open(pid,before);
}
void TrayPopup::set_bounds(unsigned long long key,RECT bounds){std::lock_guard lock(impl_->mutex);const auto found=std::find_if(impl_->items.begin(),impl_->items.end(),[&](const auto& i){return i.view.key==key;});if(found!=impl_->items.end())found->bounds=bounds;}
std::vector<TrayIcon> TrayPopup::snapshot() const {std::lock_guard lock(impl_->mutex);std::vector<TrayIcon> result;for(const auto& i:impl_->items)if(!i.hidden&&i.view.icon)result.push_back(i.view);return result;}
bool TrayPopup::activate(unsigned long long key,bool context,POINT point,bool double_click,bool keyboard) {
    Impl::Item item;{std::lock_guard lock(impl_->mutex);const auto found=std::find_if(impl_->items.begin(),impl_->items.end(),[&](const auto& i){return i.view.key==key;});if(found==impl_->items.end())return false;item=*found;}
    if(!item.message||!Impl::valid(item))return false;AllowSetForegroundWindow(item.pid);
    const auto send=[&](UINT event){const auto modern=item.version==NOTIFYICON_VERSION_4;const WPARAM w=modern?static_cast<WPARAM>(MAKELONG(point.x,point.y)):item.id;const LPARAM l=modern?MAKELONG(event,item.id):event;return PostMessageW(item.owner,item.message,w,l)!=FALSE;};
    if(context){
        DWORD pid=0;impl_->context_thread=GetWindowThreadProcessId(item.owner,&pid);impl_->context_pid=pid;
        {auto before=process_windows(pid);std::lock_guard lock(impl_->mutex);impl_->context_before=std::move(before);}
        // Slow apps (Steam, Electron) take a moment to show their menu.
        impl_->context_until=GetTickCount64()+1200;
        // Match Explorer's mouse sequence: version 3/4 icons still receive the
        // button messages before WM_CONTEXTMENU; many open menus on WM_RBUTTONUP.
        if(keyboard&&item.version>=NOTIFYICON_VERSION)return send(WM_CONTEXTMENU);
        send(WM_RBUTTONDOWN);const bool posted=send(WM_RBUTTONUP);if(item.version>=NOTIFYICON_VERSION)return send(WM_CONTEXTMENU);return posted;}
    if(double_click)return send(WM_LBUTTONDBLCLK);
    send(WM_LBUTTONDOWN);const bool posted=send(WM_LBUTTONUP);if(item.version>=NOTIFYICON_VERSION)send(NIN_SELECT);return posted;
}
}
