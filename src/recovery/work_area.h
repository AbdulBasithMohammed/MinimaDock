#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <stdexcept>
#include "windows/window_state.h"

namespace minima {
inline bool same_rect(const RECT& a,const RECT& b) {
    return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;
}
inline bool contained_rect(const RECT& outer,const RECT& inner) {
    return inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom&&inner.left<inner.right&&inner.top<inner.bottom;
}
struct WorkArea {
    std::wstring device;
    RECT bounds{},original{},applied{};
};
inline std::vector<WorkArea> monitor_work_areas() {
    std::vector<WorkArea> result;
    if(!EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM context)->BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(!GetMonitorInfoW(monitor,&info))return FALSE;
        reinterpret_cast<std::vector<WorkArea>*>(context)->push_back({info.szDevice,info.rcMonitor,info.rcWork,info.rcWork});return TRUE;
    },reinterpret_cast<LPARAM>(&result)))throw std::runtime_error("Cannot capture monitor work areas.");
    if(result.empty()||result.size()>32)throw std::runtime_error("Unsupported monitor configuration.");
    return result;
}
inline bool current_work_area(const WorkArea& area,MONITORINFOEXW& info) {
    RECT bounds=area.bounds;
    const auto monitor=MonitorFromRect(&bounds,MONITOR_DEFAULTTONULL);
    info={};info.cbSize=sizeof(info);
    return monitor&&GetMonitorInfoW(monitor,&info)&&area.device==info.szDevice&&same_rect(area.bounds,info.rcMonitor);
}
// Remove only the captured bottom reservation covered by this Windows taskbar.
// Keep space reserved by other desktop toolbars on the remaining edges.
inline RECT without_bottom_taskbar(const RECT& bounds,const RECT& work,const RECT& bar) {
    auto desired=work;
    if(contained_rect(bounds,work)&&work.bottom<bounds.bottom&&bar.bottom>=bounds.bottom&&
       bar.top<=work.bottom&&bar.top>bounds.top&&bar.left<=work.left&&bar.right>=work.right)
        desired.bottom=bounds.bottom;
    return desired;
}
inline bool reflow_frame(const RECT& before,const RECT& after,const RECT& frame,bool maximized,bool arranged,RECT& desired) {
    const auto close=[](LONG a,LONG b){return a>=b-2&&a<=b+2;};
    if(same_rect(before,after)||!close(frame.top,before.top)||!close(frame.bottom,before.bottom))return false;
    desired=frame;
    if(maximized) {
        if(!close(frame.left,before.left)||!close(frame.right,before.right))return false;
        desired=after;
    } else {
        // Extend only full-height native snapped panes; retain their exact widths.
        // Quarter layouts, ordinary windows and application-limited sizes stay put.
        if(!arranged||frame.left<before.left-2||frame.right>before.right+2||frame.left>=frame.right||
           frame.right-frame.left>=before.right-before.left-2)return false;
        desired.top=after.top;desired.bottom=after.bottom;
    }
    return !same_rect(frame,desired);
}
inline void reflow_work_area_windows(const RECT& before,const RECT& after) {
    if(same_rect(before,after))return;
    struct Context {RECT before,after;HMONITOR monitor;};
    RECT bounds=after;Context context{before,after,MonitorFromRect(&bounds,MONITOR_DEFAULTTONULL)};
    if(!context.monitor)return;
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        const auto& c=*reinterpret_cast<Context*>(data);
        if(!IsWindowVisible(window)||IsIconic(window)||MonitorFromWindow(window,MONITOR_DEFAULTTONULL)!=c.monitor)return TRUE;
        const bool maximized=IsZoomed(window)!=FALSE,arranged=window_arranged(window);
        if(!maximized&&!arranged)return TRUE;
        RECT outer{},frame{};DWORD cloaked=0;
        if(!GetWindowRect(window,&outer)||FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame)))||
           FAILED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))||cloaked)return TRUE;
        RECT desired{};
        if(!reflow_frame(c.before,c.after,frame,maximized,arranged,desired))return TRUE;
        const auto left=desired.left+outer.left-frame.left,top=desired.top+outer.top-frame.top;
        const auto right=desired.right+outer.right-frame.right,bottom=desired.bottom+outer.bottom-frame.bottom;
        SetWindowPos(window,nullptr,left,top,right-left,bottom-top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_ASYNCWINDOWPOS);
        return TRUE;
    },reinterpret_cast<LPARAM>(&context));
}
inline bool set_work_area(const RECT& previous,const RECT& desired) {
    auto rect=desired;
    // Session-only. Explorer reclaims the strip on a broadcast notification.
    // Refresh maximized/snapped apps without focus changes or blocking on them.
    if(!SystemParametersInfoW(SPI_SETWORKAREA,0,&rect,0))return false;
    reflow_work_area_windows(previous,desired);
    return true;
}
}
