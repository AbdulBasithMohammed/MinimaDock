#include "windows/inventory.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commoncontrols.h>
#include <propsys.h>
#include <propkey.h>
#include <appmodel.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include "platform/com_ptr.h"
#include <unordered_map>
#include "windows/pin_stream.h"
#include "windows/pin_preferences.h"

namespace minima {
namespace {
std::wstring property_string(IPropertyStore* properties,REFPROPERTYKEY key) {
    PROPVARIANT value{};std::wstring text;
    if(SUCCEEDED(properties->GetValue(key,&value))&&value.vt==VT_LPWSTR&&value.pwszVal)text=value.pwszVal;
    PropVariantClear(&value);return text;
}
// Paths may be long, but per-window/per-shortcut work must not zero 64 KB
// stack arrays. Size environment expansion exactly; other Shell calls use an
// uninitialized heap buffer of the maximum length.
constexpr DWORD long_path=32768;
std::unique_ptr<wchar_t[]> path_buffer(){auto buffer=std::make_unique_for_overwrite<wchar_t[]>(long_path);buffer[0]=0;return buffer;}
std::wstring expand_environment(const wchar_t* value) {
    const auto needed=ExpandEnvironmentStringsW(value,nullptr,0);
    if(!needed||needed>long_path)return {};
    std::wstring result(needed,wchar_t{});
    const auto written=ExpandEnvironmentStringsW(value,result.data(),needed);
    if(!written||written>needed)return {};
    result.resize(written-1);return result;
}
bool has_extension(const std::filesystem::path& path,const wchar_t* extension){return _wcsicmp(path.extension().c_str(),extension)==0;}
NativeIcon own_icon(HICON icon) {return icon?NativeIcon(icon,[](void* h){DestroyIcon(static_cast<HICON>(h));}):NativeIcon{};}
NativeIcon resource_icon(const std::wstring& resource) {
    static std::unordered_map<std::wstring,NativeIcon> cache;
    if(resource.empty())return {};
    if(const auto found=cache.find(resource);found!=cache.end())return found->second;
    std::wstring path=expand_environment(resource.c_str());int index=0;
    if(path.empty())return {};
    if(const auto comma=path.rfind(L',');comma!=std::wstring::npos) {
        wchar_t* end=nullptr;const auto parsed=wcstol(path.c_str()+comma+1,&end,10);
        if(!end||*end!=0)return {};
        index=static_cast<int>(parsed);path.resize(comma);
    }
    if(path.size()>1&&path.front()==L'"'&&path.back()==L'"')path=path.substr(1,path.size()-2);
    HICON raw=nullptr;SHDefExtractIconW(path.c_str(),index,0,&raw,nullptr,MAKELONG(48,0));
    auto icon=own_icon(raw);if(cache.size()>=128)cache.erase(cache.begin());cache.emplace(resource,icon);return icon;
}
NativeIcon shell_app_icon(const std::wstring& app_id,UINT pixels) {
    static std::unordered_map<std::wstring,NativeIcon> cache;
    if(app_id.empty())return {};
    const auto key=app_id+L"/"+std::to_wstring(pixels);
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    ComPtr<IShellItemImageFactory> factory;HBITMAP color=nullptr;
    const auto path=L"shell:AppsFolder\\"+app_id;
    if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(factory.put())))||
       FAILED(factory->GetImage({static_cast<LONG>(pixels),static_cast<LONG>(pixels)},static_cast<SIIGBF>(SIIGBF_ICONONLY|SIIGBF_BIGGERSIZEOK),&color)))return {};
    BITMAP bitmap{};GetObjectW(color,sizeof(bitmap),&bitmap);
    HICON raw=nullptr;
    if(bitmap.bmWidth>0&&bitmap.bmWidth<=512&&bitmap.bmHeight>0&&bitmap.bmHeight<=512) {
        // Shell images carry alpha. Preserve it through CreateIconIndirect;
        // no icon-background flag, theme recolor, or executable substitution.
        std::vector<BYTE> mask_bits(static_cast<size_t>((bitmap.bmWidth+15)/16)*2*bitmap.bmHeight,0);
        HBITMAP mask=CreateBitmap(bitmap.bmWidth,bitmap.bmHeight,1,1,mask_bits.data());
        if(mask){ICONINFO info{TRUE,0,0,mask,color};raw=CreateIconIndirect(&info);DeleteObject(mask);}
    }
    DeleteObject(color);auto icon=own_icon(raw);
    if(cache.size()>=128)cache.erase(cache.begin());cache.emplace(key,icon);return icon;
}
NativeIcon read_icon(const std::wstring& path) {
    if(path.empty()) return {};
    static std::unordered_map<std::wstring,NativeIcon> cache;
    if(const auto it=cache.find(path);it!=cache.end()) return it->second;
    SHFILEINFOW info{};
    HICON raw=nullptr;
    // Virtual shell pins (notably File Explorer) can have no filesystem target.
    // Honor the shortcut's actual icon resource before asking the file image list.
    if(has_extension(path,L".lnk")) {
        ComPtr<IShellLinkW> link;ComPtr<IPersistFile> file;
        if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put())))&&
           SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put())))&&SUCCEEDED(file->Load(path.c_str(),STGM_READ))) {
            const auto location=path_buffer();int index=0;
            if(SUCCEEDED(link->GetIconLocation(location.get(),long_path,&index))&&location[0])
                if(const auto expanded=expand_environment(location.get());!expanded.empty())SHDefExtractIconW(expanded.c_str(),index,0,&raw,nullptr,MAKELONG(48,0));
        }
    }
    if(!raw&&SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_SYSICONINDEX)) {
        ComPtr<IImageList> images;
        if(SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE,IID_PPV_ARGS(images.put()))))images->GetIcon(info.iIcon,ILD_TRANSPARENT,&raw);
    }
    if(!raw&&SHGetFileInfoW(path.c_str(),0,&info,sizeof(info),SHGFI_ICON|SHGFI_LARGEICON))raw=info.hIcon;
    if(!raw)return {};
    NativeIcon icon(raw,[](void* h){DestroyIcon(static_cast<HICON>(h));});
    if(cache.size()>=64) cache.erase(cache.begin());
    cache.emplace(path,icon); return icon;
}
std::wstring link_target(const std::wstring& path) {
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> file;
    if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put()))) ||
       FAILED(link->QueryInterface(IID_PPV_ARGS(file.put()))) || FAILED(file->Load(path.c_str(),STGM_READ))) return {};
    const auto buffer=path_buffer();wchar_t* target=buffer.get();
    if(FAILED(link->GetPath(target,long_path,nullptr,SLGP_RAWPATH))) return {};
    if(!target[0]) {
        ComPtr<IPropertyStore> properties;PROPVARIANT identity{};
        if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(properties.put())))&&SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID,&identity))) {
            const bool explorer=identity.vt==VT_LPWSTR&&identity.pwszVal&&wcscmp(identity.pwszVal,L"Microsoft.Windows.Explorer")==0;
            PropVariantClear(&identity);
            if(explorer) {wchar_t windows[MAX_PATH]{};if(GetWindowsDirectoryW(windows,MAX_PATH))return (std::filesystem::path(windows)/L"explorer.exe").wstring();}
        }
    }
    if(auto expanded=expand_environment(target);!expanded.empty()) return expanded;
    return target;
}
// Per-window icon copies. Entries not seen by the latest enumeration belong to
// closed windows and are pruned, so the cache never needs a wholesale reset
// (which would recopy every icon and force the GUI to rebuild its bitmaps).
struct WindowIconEntry {DWORD pid;DWORD_PTR source;NativeIcon icon;unsigned generation;};
std::unordered_map<HWND,WindowIconEntry> window_icons;
unsigned window_icon_generation=0;
NativeIcon window_icon(HWND window,DWORD pid) {
    auto& cache=window_icons;
    DWORD_PTR value=0;
    SendMessageTimeoutW(window,WM_GETICON,ICON_BIG,GetDpiForWindow(window),SMTO_ABORTIFHUNG|SMTO_BLOCK,20,&value);
    if(!value)value=static_cast<DWORD_PTR>(GetClassLongPtrW(window,GCLP_HICON));
    if(!value)SendMessageTimeoutW(window,WM_GETICON,ICON_SMALL2,GetDpiForWindow(window),SMTO_ABORTIFHUNG|SMTO_BLOCK,20,&value);
    if(!value)return {};
    if(const auto found=cache.find(window);found!=cache.end()&&found->second.pid==pid&&found->second.source==value){found->second.generation=window_icon_generation;return found->second.icon;}
    const auto copy=CopyIcon(reinterpret_cast<HICON>(value));if(!copy)return {};
    NativeIcon icon(copy,[](void* h){DestroyIcon(static_cast<HICON>(h));});
    cache.insert_or_assign(window,WindowIconEntry{pid,value,icon,window_icon_generation});return icon;
}
struct Context { DWORD own; std::vector<AppWindow>* output; };
std::wstring process_app_id(HANDLE process) {
    wchar_t id[512]{};UINT32 size=512;
    return GetApplicationUserModelId(process,&size,id)==ERROR_SUCCESS?std::wstring(id):std::wstring{};
}
struct HostedIdentity {DWORD root_pid;unsigned examined=0;std::wstring app_id;};
BOOL CALLBACK hosted_identity(HWND child,LPARAM parameter) {
    auto& context=*reinterpret_cast<HostedIdentity*>(parameter);
    if(++context.examined>64)return FALSE;
    DWORD pid=0;GetWindowThreadProcessId(child,&pid);
    if(pid==context.root_pid)return TRUE;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process){context.app_id=process_app_id(process);CloseHandle(process);}
    return context.app_id.empty()?TRUE:FALSE;
}
BOOL CALLBACK enum_window(HWND hwnd, LPARAM param) {
    const auto& c = *reinterpret_cast<Context*>(param);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == c.own || !IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0) return TRUE;
    const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) && !(style & WS_EX_APPWINDOW)) return TRUE;
    DWORD cloaked = 0; DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    wchar_t class_name[128]{}; GetClassNameW(hwnd, class_name, 128);
    if (wcscmp(class_name, L"Progman") == 0 || wcscmp(class_name, L"WorkerW") == 0 || wcsstr(class_name, L"TrayWnd")) return TRUE;
    wchar_t title[512]{}; GetWindowTextW(hwnd, title, 512);
    std::wstring executable,app_id,resource;
    ComPtr<IPropertyStore> properties;
    if(SUCCEEDED(SHGetPropertyStoreForWindow(hwnd,IID_PPV_ARGS(properties.put())))) {
        app_id=property_string(properties.get(),PKEY_AppUserModel_ID);
        resource=property_string(properties.get(),PKEY_AppUserModel_RelaunchIconResource);
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        const auto path=path_buffer(); DWORD count = long_path;
        if (QueryFullProcessImageNameW(process, 0, path.get(), &count)) executable.assign(path.get(), count);
        if(app_id.empty())app_id=process_app_id(process);
        CloseHandle(process);
    }
    if(app_id.empty()&&wcscmp(class_name,L"ApplicationFrameWindow")==0) {
        HostedIdentity hosted{pid,0,{}};
        EnumChildWindows(hwnd,hosted_identity,reinterpret_cast<LPARAM>(&hosted));app_id=std::move(hosted.app_id);
    }
    auto icon=resource_icon(resource);std::wstring source=L"relaunch resource";
    if(!icon&&!app_id.empty()){icon=shell_app_icon(app_id,MulDiv(24,GetDpiForWindow(hwnd),96));source=L"registered Shell app";}
    if(!icon){icon=window_icon(hwnd,pid);source=L"window icon";}
    if(!icon){icon=read_icon(executable);source=L"executable fallback";}
    c.output->push_back({ hwnd, title, executable, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), IsIconic(hwnd) != FALSE, std::move(icon),std::move(app_id),std::move(source) });
    return TRUE;
}
void shortcuts(REFKNOWNFOLDERID id, bool recursive, std::vector<Shortcut>& output) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) return;
    const std::filesystem::path root(path); CoTaskMemFree(path);
    std::error_code error;
    auto add = [&](const std::filesystem::directory_entry& entry) {
        if (output.size() >= 512) return;
        const auto name = entry.path().filename().wstring();
        if (name == L"desktop.ini" || name.empty() || name[0] == L'.') return;
        const bool link = has_extension(entry.path(), L".lnk") || has_extension(entry.path(), L".url");
        if (recursive && !link) return;
        output.push_back({ link ? entry.path().stem().wstring() : name, entry.path().wstring(), entry.is_directory(error), {}, {}, {} });
    };
    if (recursive) {
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
        for (; it != end && !error; it.increment(error)) { if (it.depth() >= 4) it.disable_recursion_pending(); add(*it); }
    } else {
        std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
        for (; it != end && !error; it.increment(error)) add(*it);
    }
}
void shell_pins(std::vector<Shortcut>& output) {
    constexpr auto key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    DWORD version=0,version_size=sizeof(version);
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"FavoritesVersion",RRF_RT_REG_DWORD,nullptr,&version,&version_size)!=ERROR_SUCCESS||version!=3)return;
    DWORD bytes=0;
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"Favorites",RRF_RT_REG_BINARY,nullptr,nullptr,&bytes)!=ERROR_SUCCESS||bytes>1024*1024)return;
    std::vector<std::uint8_t> stream(bytes);
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"Favorites",RRF_RT_REG_BINARY,nullptr,stream.data(),&bytes)!=ERROR_SUCCESS)return;
    stream.resize(bytes);
    for(const auto& record:parse_pin_stream(stream)) {
        ComPtr<IShellItem2> item;
        if(FAILED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(stream.data()+record.offset),IID_PPV_ARGS(item.put()))))continue;
        PWSTR title=nullptr,path=nullptr,id=nullptr;
        if(FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY,&title)))continue;
        item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&path);
        item->GetString(PKEY_AppUserModel_ID,&id);
        Shortcut pin{title,path?path:L"",false,{}, {},id?id:L""};
        CoTaskMemFree(title);CoTaskMemFree(path);CoTaskMemFree(id);
        if(pin.path.empty()||pin.title.empty())continue;
        const bool link=has_extension(pin.path,L".lnk");
        if(link) {pin.target=link_target(pin.path);pin.icon=read_icon(pin.path);}
        if(!pin.app_id.empty()) {
            const auto icon=shell_app_icon(pin.app_id,MulDiv(24,GetDpiForSystem(),96));
            if(icon)pin.icon=icon;
        }
        output.push_back(std::move(pin));
    }
}
}
std::vector<AppWindow> collect_windows(DWORD own_pid) {
    std::vector<AppWindow> windows;
    ++window_icon_generation;
    Context context{own_pid, &windows}; EnumWindows(enum_window, reinterpret_cast<LPARAM>(&context));
    std::erase_if(window_icons,[](const auto& entry){return entry.second.generation!=window_icon_generation;});
    return windows;
}
Inventory collect_inventory(DWORD own_pid) {
    Inventory result;
    result.windows = collect_windows(own_pid);
    shortcuts(FOLDERID_Desktop, false, result.desktop); shortcuts(FOLDERID_PublicDesktop, false, result.desktop);
    shortcuts(FOLDERID_StartMenu, true, result.launcher); shortcuts(FOLDERID_CommonStartMenu, true, result.launcher);
    for(size_t i=0;i<std::min<size_t>(8,result.desktop.size());++i) result.desktop[i].icon=read_icon(result.desktop[i].path);
    shell_pins(result.pinned);
    PWSTR roaming=nullptr;
    if(result.pinned.empty()&&SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,KF_FLAG_DEFAULT,nullptr,&roaming))) {
        const auto folder=std::filesystem::path(roaming)/L"Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"; CoTaskMemFree(roaming);
        std::error_code error;
        for(std::filesystem::directory_iterator it(folder,error),end;it!=end&&!error;it.increment(error)) {
            if(result.pinned.size()>=16) break;
            if(has_extension(it->path(),L".lnk")) {
                const auto path=it->path().wstring();
                result.pinned.push_back({it->path().stem().wstring(),path,false,link_target(path),read_icon(path),{}});
            }
        }
    }
    apply_pin_preferences(result);
    for(auto& pin:result.pinned)if(!pin.icon) {
        if(!pin.app_id.empty())pin.icon=shell_app_icon(pin.app_id,MulDiv(24,GetDpiForSystem(),96));
        if(!pin.icon)pin.icon=read_icon(pin.path);
    }
    std::sort(result.launcher.begin(), result.launcher.end(), [](const auto& a, const auto& b) { return a.title < b.title; });
    return result;
}
bool activate_window(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
    return SetForegroundWindow(hwnd) != FALSE;
}
}
