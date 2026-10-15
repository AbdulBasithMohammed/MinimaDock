#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <span>
#include <vector>
#include "windows/shell_desktop.h"
#include <future>
#include <chrono>

namespace minima {
inline bool desktop_application_window(HWND window,DWORD excluded_process) {
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    const auto style=GetWindowLongPtrW(window,GWL_STYLE),extended=GetWindowLongPtrW(window,GWL_EXSTYLE);
    if(pid==excluded_process||!IsWindowVisible(window)||(style&WS_CHILD)||
        (extended&(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))||
        (GetWindow(window,GW_OWNER)&&!(extended&WS_EX_APPWINDOW)))return false;
    wchar_t type[128]{};GetClassNameW(window,type,128);
    if(wcscmp(type,L"Progman")==0||wcscmp(type,L"WorkerW")==0||wcsstr(type,L"TrayWnd"))return false;
    // Borderless games commonly have WS_POPUP | WS_EX_APPWINDOW with no
    // WS_MINIMIZEBOX. A caption/button is not required to minimize an app.
    if((style&(WS_MINIMIZEBOX|WS_CAPTION))||(extended&WS_EX_APPWINDOW))return true;
    if(!(style&WS_POPUP))return false;
    RECT bounds{};MONITORINFO monitor{sizeof(monitor),{},{},0};
    return GetWindowRect(window,&bounds)&&GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)&&
        bounds.left<=monitor.rcMonitor.left&&bounds.top<=monitor.rcMonitor.top&&
        bounds.right>=monitor.rcMonitor.right&&bounds.bottom>=monitor.rcMonitor.bottom;
}
// Native fallback for an Explorer-free shell; Explorer-backed trials dispatch
// Windows' own Show Desktop command on a worker, checked by the dock timer.
class DesktopToggle {
    struct Window {
        HWND handle=nullptr;
        DWORD process=0,thread=0;
        FILETIME created{};
        ATOM type=0;
    };
    std::vector<Window> hidden_,pending_;
    HWND foreground_=nullptr;
    ULONGLONG deadline_=0;
    bool restoring_=false,queue_failed_=false;
    std::future<HRESULT> shell_request_;
    static bool identify(HWND window,Window& result) {
        result.handle=window;result.thread=GetWindowThreadProcessId(window,&result.process);
        const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,result.process);
        if(!process)return false;
        FILETIME exited{},kernel{},user{};
        const auto ok=GetProcessTimes(process,&result.created,&exited,&kernel,&user);
        CloseHandle(process);result.type=static_cast<ATOM>(GetClassLongPtrW(window,GCW_ATOM));
        return ok&&result.thread&&result.type;
    }
    static bool valid(const Window& window) {
        Window current;
        return IsWindow(window.handle)&&identify(window.handle,current)&&
            current.process==window.process&&current.thread==window.thread&&current.type==window.type&&
            CompareFileTime(&current.created,&window.created)==0;
    }
    static bool visible(HWND window) {
        DWORD cloaked=0;
        return IsWindowVisible(window)&&!IsIconic(window)&&
            (FAILED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))||!cloaked);
    }
public:
    bool busy() const { return deadline_!=0||shell_request_.valid(); }
    // Explicit targets also allow native fixture testing without moving user apps.
    HRESULT toggle_windows(std::span<const HWND> candidates,HWND foreground) {
        if(busy())return E_PENDING;
        const bool any_visible=std::any_of(candidates.begin(),candidates.end(),visible);
        restoring_=!hidden_.empty()&&!any_visible;
        pending_.clear();queue_failed_=false;
        if(restoring_) {
            for(const auto& window:hidden_)if(valid(window)&&IsIconic(window.handle))pending_.push_back(window);
        } else {
            hidden_.clear();foreground_=foreground;
            for(const auto handle:candidates)if(visible(handle)) {
                Window window;
                if(identify(handle,window))pending_.push_back(window);else queue_failed_=true;
            }
        }
        if(pending_.empty()){hidden_.clear();return queue_failed_?E_ACCESSDENIED:S_OK;}
        for(const auto& window:pending_)
            if(!ShowWindowAsync(window.handle,restoring_?SW_RESTORE:SW_MINIMIZE))queue_failed_=true;
        if(!restoring_)hidden_=pending_;
        deadline_=GetTickCount64()+1500;
        return E_PENDING;
    }
    HRESULT toggle(HWND dock,HWND foreground) {
        if(busy())return E_PENDING;
        if(const auto taskbar=explorer_taskbar()) {
            shell_request_=std::async(std::launch::async,[taskbar]{return invoke_shell_desktop(taskbar);});
            return E_PENDING;
        }
        std::vector<HWND> targets;
        struct Enumeration {DWORD excluded;std::vector<HWND>* targets;};
        DWORD pid=0;GetWindowThreadProcessId(dock,&pid);Enumeration state{pid,&targets};
        EnumWindows([](HWND window,LPARAM parameter)->BOOL {
            auto& state=*reinterpret_cast<Enumeration*>(parameter);
            if(desktop_application_window(window,state.excluded))state.targets->push_back(window);
            return TRUE;
        },reinterpret_cast<LPARAM>(&state));
        return toggle_windows(targets,foreground);
    }
    HRESULT tick() {
        if(shell_request_.valid()) {
            if(shell_request_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return E_PENDING;
            return shell_request_.get();
        }
        if(!busy())return S_OK;
        const auto done=[&](const Window& window){return !valid(window)||
            (restoring_?!IsIconic(window.handle):IsIconic(window.handle)||!IsWindowVisible(window.handle));};
        const bool complete=std::all_of(pending_.begin(),pending_.end(),done);
        if(!complete&&GetTickCount64()<deadline_)return E_PENDING;
        deadline_=0;
        if(restoring_) {
            std::erase_if(hidden_,[&](const Window& window){return !valid(window)||!IsIconic(window.handle);});
            if(complete&&IsWindow(foreground_)&&visible(foreground_))SetForegroundWindow(foreground_);
        } else std::erase_if(hidden_,[](const Window& window){return !valid(window)||!IsIconic(window.handle);});
        pending_.clear();
        return !complete?HRESULT_FROM_WIN32(ERROR_TIMEOUT):queue_failed_?E_ACCESSDENIED:S_OK;
    }
    void restore_on_exit() {
        for(const auto& window:hidden_)if(valid(window)&&IsIconic(window.handle))ShowWindowAsync(window.handle,SW_RESTORE);
        hidden_.clear();pending_.clear();deadline_=0;
    }
};
}
