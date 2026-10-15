#include "windows/launcher_index.h"
#include "platform/com_ptr.h"
#include "windows/file_search.h"
#include <shlobj.h>
#include <shellapi.h>
#include <powrprof.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace minima {
namespace {
constexpr wchar_t usage_key[]=L"Software\\MinimaDock\\Launcher\\Usage";
std::shared_ptr<void> own_bitmap(HBITMAP bitmap){return bitmap?std::shared_ptr<void>(bitmap,[](void* b){DeleteObject(static_cast<HBITMAP>(b));}):nullptr;}
std::vector<LauncherApp> read_apps() {
    std::vector<LauncherApp> apps;
    ComPtr<IShellItem> folder;ComPtr<IEnumShellItems> items;
    if(FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder",nullptr,IID_PPV_ARGS(folder.put())))||
       FAILED(folder->BindToHandler(nullptr,BHID_EnumItems,IID_PPV_ARGS(items.put()))))return apps;
    for(;;) {
        ComPtr<IShellItem> item;ULONG fetched=0;
        if(items->Next(1,item.put(),&fetched)!=S_OK||!fetched)break;
        PWSTR name=nullptr,id=nullptr;
        if(SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY,&name))&&SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING,&id))) {
            LauncherApp app;app.name=name;app.id=id;app.program=app_file_path(app.id);
            ComPtr<IShellItemImageFactory> images;HBITMAP image=nullptr;
            if(SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(images.put())))&&SUCCEEDED(images->GetImage(SIZE{64,64},SIIGBF_ICONONLY,&image)))app.image=own_bitmap(image);
            if(!app.name.empty()&&std::none_of(apps.begin(),apps.end(),[&](const LauncherApp& a){return a.name==app.name;}))apps.push_back(std::move(app));
        }
        CoTaskMemFree(name);CoTaskMemFree(id);
        if(apps.size()>=1500)break;
    }
    std::sort(apps.begin(),apps.end(),[](const LauncherApp& a,const LauncherApp& b){return _wcsicmp(a.name.c_str(),b.name.c_str())<0;});
    return apps;
}
bool enable_shutdown_privilege() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return false;
    TOKEN_PRIVILEGES privileges{};privileges.PrivilegeCount=1;privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    const bool ok=LookupPrivilegeValueW(nullptr,SE_SHUTDOWN_NAME,&privileges.Privileges[0].Luid)&&
        AdjustTokenPrivileges(token,FALSE,&privileges,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS;
    CloseHandle(token);return ok;
}
constexpr DWORD planned=SHTDN_REASON_MAJOR_OTHER|SHTDN_REASON_MINOR_OTHER|SHTDN_REASON_FLAG_PLANNED;
}

struct LauncherIndex::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<const std::vector<LauncherApp>> apps=std::make_shared<std::vector<LauncherApp>>();
    std::atomic<HWND> notify=nullptr;
    std::thread worker;
    bool requested=false,stopping=false;
    ULONGLONG loaded=0;
    void run() {
        const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        for(;;) {
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||requested;});if(stopping)break;requested=false;}
            auto list=std::make_shared<std::vector<LauncherApp>>(read_apps());
            {std::lock_guard lock(mutex);apps=std::move(list);loaded=GetTickCount64();}
            if(const auto window=notify.load())PostMessageW(window,launcher_index_message,0,0);
        }
        if(SUCCEEDED(apartment))CoUninitialize();
    }
};
LauncherIndex::LauncherIndex():impl_(std::make_unique<Impl>()){}
LauncherIndex::~LauncherIndex(){stop();}
void LauncherIndex::start(HWND notify) {
    stop();impl_->notify=notify;impl_->stopping=false;
    impl_->worker=std::thread([i=impl_.get()]{i->run();});
    {std::lock_guard lock(impl_->mutex);impl_->requested=true;}
    impl_->wake.notify_all();
}
void LauncherIndex::stop() {
    auto& i=*impl_;i.notify=nullptr;if(!i.worker.joinable())return;
    {std::lock_guard lock(i.mutex);i.stopping=true;}
    i.wake.notify_all();i.worker.join();
}
void LauncherIndex::refresh() {
    auto& i=*impl_;
    {std::lock_guard lock(i.mutex);if(i.loaded&&GetTickCount64()-i.loaded<10*60*1000)return;i.requested=true;}
    i.wake.notify_all();
}
std::shared_ptr<const std::vector<LauncherApp>> LauncherIndex::apps() const{std::lock_guard lock(impl_->mutex);return impl_->apps;}

bool launch_app_id(HWND owner,const std::wstring& id,bool elevated) {
    const auto target=L"shell:AppsFolder\\"+id;
    const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(owner,elevated?L"runas":L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
    return result>32||(elevated&&result==SE_ERR_ACCESSDENIED); // A declined elevation prompt is not an error.
}

std::map<std::wstring,Usage> load_usage() {
    std::map<std::wstring,Usage> result;HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,usage_key,0,KEY_READ,&key)!=ERROR_SUCCESS)return result;
    FILETIME now_file{};GetSystemTimeAsFileTime(&now_file);
    const auto now=(static_cast<ULONGLONG>(now_file.dwHighDateTime)<<32)|now_file.dwLowDateTime;
    for(DWORD index=0;;++index) {
        wchar_t name[1024];DWORD name_size=1024,type=0;struct {std::uint32_t count;ULONGLONG last;} data{};DWORD data_size=sizeof(data);
        const auto status=RegEnumValueW(key,index,name,&name_size,nullptr,&type,reinterpret_cast<BYTE*>(&data),&data_size);
        if(status==ERROR_NO_MORE_ITEMS)break;
        if(status!=ERROR_SUCCESS||type!=REG_BINARY||data_size!=sizeof(data))continue;
        result[name]={data.count,now>data.last?static_cast<double>(now-data.last)/(864000000000.0):0};
    }
    RegCloseKey(key);return result;
}
void record_use(const std::wstring& key) {
    HKEY handle=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,usage_key,0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&handle,nullptr)!=ERROR_SUCCESS)return;
    struct {std::uint32_t count;ULONGLONG last;} data{};DWORD size=sizeof(data),type=0;
    if(RegQueryValueExW(handle,key.c_str(),nullptr,&type,reinterpret_cast<BYTE*>(&data),&size)!=ERROR_SUCCESS||type!=REG_BINARY||size!=sizeof(data))data={};
    FILETIME now{};GetSystemTimeAsFileTime(&now);
    data.count=std::min<std::uint32_t>(data.count+1,100000);data.last=(static_cast<ULONGLONG>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
    RegSetValueExW(handle,key.c_str(),0,REG_BINARY,reinterpret_cast<const BYTE*>(&data),sizeof(data));
    RegCloseKey(handle);
}

const std::vector<SettingsPage>& settings_pages() {
    static const std::vector<SettingsPage> pages={
        {L"Bluetooth & devices",L"bluetooth devices pair",L"ms-settings:bluetooth"},{L"Wi-Fi",L"wifi wireless network",L"ms-settings:network-wifi"},
        {L"Network & internet",L"network internet ethernet",L"ms-settings:network"},{L"VPN",L"vpn",L"ms-settings:network-vpn"},
        {L"Mobile hotspot",L"hotspot tethering",L"ms-settings:network-mobilehotspot"},{L"Display",L"display screen resolution scale brightness monitor",L"ms-settings:display"},
        {L"Night light",L"night light blue",L"ms-settings:nightlight"},{L"Sound",L"sound audio output input speakers",L"ms-settings:sound"},
        {L"Notifications",L"notifications focus do not disturb",L"ms-settings:notifications"},{L"Power & battery",L"power battery sleep energy",L"ms-settings:powersleep"},
        {L"Storage",L"storage disk space cleanup",L"ms-settings:storagesense"},{L"Multitasking",L"multitasking snap alt tab",L"ms-settings:multitasking"},
        {L"Clipboard",L"clipboard history",L"ms-settings:clipboard"},{L"About this PC",L"about pc specs name rename",L"ms-settings:about"},
        {L"Personalization",L"personalization theme colours colors",L"ms-settings:personalization"},{L"Background",L"background wallpaper",L"ms-settings:personalization-background"},
        {L"Colours",L"colours colors accent dark light mode",L"ms-settings:colors"},{L"Lock screen",L"lock screen",L"ms-settings:lockscreen"},
        {L"Fonts",L"fonts",L"ms-settings:fonts"},{L"Installed apps",L"installed apps uninstall programs",L"ms-settings:appsfeatures"},
        {L"Default apps",L"default apps browser",L"ms-settings:defaultapps"},{L"Startup apps",L"startup apps boot",L"ms-settings:startupapps"},
        {L"Optional features",L"optional features",L"ms-settings:optionalfeatures"},{L"Accounts",L"accounts user",L"ms-settings:accounts"},
        {L"Sign-in options",L"sign in pin password windows hello",L"ms-settings:signinoptions"},{L"Date & time",L"date time clock timezone",L"ms-settings:dateandtime"},
        {L"Language & region",L"language region keyboard locale",L"ms-settings:regionlanguage"},{L"Typing",L"typing keyboard autocorrect",L"ms-settings:typing"},
        {L"Gaming",L"gaming game mode xbox game bar",L"ms-settings:gaming-gamemode"},{L"Captures",L"captures recording screenshots",L"ms-settings:gaming-gamedvr"},
        {L"Accessibility",L"accessibility ease of access",L"ms-settings:easeofaccess"},{L"Mouse",L"mouse pointer speed cursor",L"ms-settings:mousetouchpad"},
        {L"Privacy & security",L"privacy security",L"ms-settings:privacy"},{L"Location",L"location gps",L"ms-settings:privacy-location"},
        {L"Microphone permissions",L"microphone mic privacy",L"ms-settings:privacy-microphone"},{L"Camera permissions",L"camera webcam privacy",L"ms-settings:privacy-webcam"},
        {L"Windows Security",L"windows security defender antivirus",L"windowsdefender:"},{L"Windows Update",L"windows update",L"ms-settings:windowsupdate"},
        {L"Recovery",L"recovery reset",L"ms-settings:recovery"},{L"Printers & scanners",L"printers scanners",L"ms-settings:printers"},
        {L"Taskbar",L"taskbar",L"ms-settings:taskbar"},{L"Graphics",L"graphics gpu",L"ms-settings:display-advancedgraphics"},
    };
    return pages;
}

bool power_lock(){return LockWorkStation()!=FALSE;}
bool power_sleep(){return SetSuspendState(FALSE,FALSE,FALSE)!=FALSE;}
bool power_restart(){enable_shutdown_privilege();return ExitWindowsEx(EWX_REBOOT,planned)!=FALSE;}
bool power_shutdown(){enable_shutdown_privilege();return ExitWindowsEx(EWX_SHUTDOWN|EWX_POWEROFF|EWX_HYBRID_SHUTDOWN,planned)!=FALSE;}
bool power_sign_out(){return ExitWindowsEx(EWX_LOGOFF,planned)!=FALSE;}

bool set_dark_mode(bool dark) {
    const DWORD light=dark?0:1;bool ok=true;
    for(const auto name:{L"AppsUseLightTheme",L"SystemUsesLightTheme"})
        ok&=RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",name,REG_DWORD,&light,sizeof(light))==ERROR_SUCCESS;
    DWORD_PTR result=0;
    SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),SMTO_ABORTIFHUNG,200,&result);
    return ok;
}
bool copy_text(HWND owner,const std::wstring& text) {
    if(!OpenClipboard(owner))return false;
    EmptyClipboard();bool ok=false;
    if(const auto memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t))) {
        if(auto* data=static_cast<wchar_t*>(GlobalLock(memory))){std::copy(text.begin(),text.end(),data);data[text.size()]=0;GlobalUnlock(memory);ok=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;}
        if(!ok)GlobalFree(memory);
    }
    CloseClipboard();return ok;
}
}

namespace minima {
namespace {
std::wstring base64(const std::uint8_t* data,size_t size) {
    static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    for(size_t i=0;i<size;i+=3) {
        const std::uint32_t n=(static_cast<std::uint32_t>(data[i])<<16)|(i+1<size?static_cast<std::uint32_t>(data[i+1])<<8:0)|(i+2<size?data[i+2]:0);
        out+=static_cast<wchar_t>(table[(n>>18)&63]);out+=static_cast<wchar_t>(table[(n>>12)&63]);
        out+=i+1<size?static_cast<wchar_t>(table[(n>>6)&63]):L'=';out+=i+2<size?static_cast<wchar_t>(table[n&63]):L'=';
    }
    return out;
}
bool has_terminal() {
    wchar_t path[MAX_PATH]{};
    return SearchPathW(nullptr,L"wt.exe",nullptr,MAX_PATH,path,nullptr)>0;
}
}
bool run_command(const std::wstring& command,bool elevated) {
    // -EncodedCommand (UTF-16LE, base64) keeps quotes and symbols exactly as typed.
    std::wstring arguments=L"-NoLogo -NoExit";
    if(!command.empty())arguments+=L" -EncodedCommand "+base64(reinterpret_cast<const std::uint8_t*>(command.data()),command.size()*sizeof(wchar_t));
    wchar_t home[MAX_PATH]{};GetEnvironmentVariableW(L"USERPROFILE",home,MAX_PATH);
    const bool terminal=has_terminal();
    const auto parameters=terminal?L"-d \""+std::wstring(home)+L"\" powershell.exe "+arguments:arguments;
    const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,elevated?L"runas":L"open",terminal?L"wt.exe":L"powershell.exe",parameters.c_str(),home,SW_SHOWNORMAL));
    return result>32||(elevated&&result==SE_ERR_ACCESSDENIED);
}
bool open_url(const std::wstring& url){return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;}
bool arrange_window(HWND window,Arrange how) {
    if(!IsWindow(window))return false;
    switch(how) {
    case Arrange::Close:return PostMessageW(window,WM_SYSCOMMAND,SC_CLOSE,0)!=FALSE;
    case Arrange::Minimize:return ShowWindowAsync(window,SW_MINIMIZE)!=FALSE;
    case Arrange::Maximize:ShowWindow(window,SW_MAXIMIZE);return SetForegroundWindow(window)!=FALSE;
    default:break;
    }
    // Half of the work area, measured on the visible frame (without the invisible resize borders).
    if(IsIconic(window)||IsZoomed(window))ShowWindow(window,SW_RESTORE);
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return false;
    RECT outer{},frame{};GetWindowRect(window,&outer);
    if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame))))frame=outer;
    const RECT& area=monitor.rcWork;const LONG half=(area.right-area.left)/2;
    RECT target{how==Arrange::Left?area.left:area.left+half,area.top,how==Arrange::Left?area.left+half:area.right,area.bottom};
    target.left-=frame.left-outer.left;target.top-=frame.top-outer.top;target.right+=outer.right-frame.right;target.bottom+=outer.bottom-frame.bottom;
    const bool ok=SetWindowPos(window,nullptr,target.left,target.top,target.right-target.left,target.bottom-target.top,SWP_NOZORDER|SWP_NOOWNERZORDER)!=FALSE;
    SetForegroundWindow(window);
    return ok;
}
}
