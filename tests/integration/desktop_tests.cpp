// Real native windows only; no user windows, shell changes or key injection.
#include "windows/desktop.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace {
void expect(bool value,const char* error){if(!value)throw std::runtime_error(error);}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM p,LPARAM l){return DefWindowProcW(w,m,p,l);}
void pump(){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
void finish(minima::DesktopToggle& desktop){HRESULT result=E_PENDING;const auto deadline=GetTickCount64()+2000;while(result==E_PENDING&&GetTickCount64()<deadline){pump();result=desktop.tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}expect(result==S_OK,"Native window state did not complete successfully");}
RECT bounds(HWND w){RECT r{};GetWindowRect(w,&r);return r;}
}
int main(int argc,char** argv){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);std::array<HWND,4> windows{};HWND tool=nullptr,popup=nullptr;minima::DesktopToggle desktop;int result=0;
try {
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MinimaDock.DesktopToggleFixture";
    expect(RegisterClassW(&cls)!=0,"Fixture registration failed");
    for(size_t i=0;i<3;++i){windows[i]=CreateWindowExW(0,cls.lpszClassName,L"Owned desktop toggle fixture",WS_OVERLAPPEDWINDOW,80+static_cast<int>(i)*40,80,300,160,nullptr,nullptr,cls.hInstance,nullptr);expect(windows[i]!=nullptr,"Fixture creation failed");ShowWindow(windows[i],i==1?SW_SHOWMAXIMIZED:i==2?SW_SHOWMINNOACTIVE:SW_SHOWNOACTIVATE);}
    MONITORINFO monitor{sizeof(monitor),{},{},0};expect(GetMonitorInfoW(MonitorFromWindow(windows[0],MONITOR_DEFAULTTOPRIMARY),&monitor),"Fixture monitor unavailable");const auto area=monitor.rcMonitor;
    windows[3]=CreateWindowExW(WS_EX_APPWINDOW,cls.lpszClassName,L"Owned borderless game fixture",WS_POPUP|WS_CLIPSIBLINGS,area.left,area.top,area.right-area.left,area.bottom-area.top,nullptr,nullptr,cls.hInstance,nullptr);
    expect(windows[3]!=nullptr,"Borderless fixture creation failed");ShowWindow(windows[3],SW_SHOWNOACTIVATE);
    expect(!(GetWindowLongPtrW(windows[3],GWL_STYLE)&WS_MINIMIZEBOX),"Game fixture accidentally has a minimize button");
    expect(minima::desktop_application_window(windows[3],0),"Borderless APPWINDOW game was excluded");
    SetWindowLongPtrW(windows[3],GWL_EXSTYLE,0);
    expect(minima::desktop_application_window(windows[3],0),"Fullscreen popup without APPWINDOW was excluded");
    SetWindowLongPtrW(windows[3],GWL_EXSTYLE,WS_EX_APPWINDOW);
    expect(!minima::desktop_application_window(windows[3],GetCurrentProcessId()),"Dock process exclusion failed");
    tool=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Owned tool fixture",WS_OVERLAPPEDWINDOW,80,80,200,100,nullptr,nullptr,cls.hInstance,nullptr);
    popup=CreateWindowExW(0,cls.lpszClassName,L"Owned small popup fixture",WS_POPUP,80,80,100,100,nullptr,nullptr,cls.hInstance,nullptr);
    expect(tool&&popup,"Exclusion fixtures failed");ShowWindow(tool,SW_SHOWNOACTIVATE);ShowWindow(popup,SW_SHOWNOACTIVATE);
    expect(!minima::desktop_application_window(tool,0)&&!minima::desktop_application_window(popup,0),"Tool or anonymous popup was included");
    if(argc==2&&std::string_view(argv[1])=="--shell") {
        expect(minima::explorer_taskbar()!=nullptr,"Explorer taskbar is unavailable");
        expect(desktop.toggle(windows[0],windows[0])==E_PENDING,"Native shell action was not queued");finish(desktop);pump();
        const auto deadline=GetTickCount64()+1500;wchar_t foreground[128]{};
        do {pump();GetClassNameW(GetForegroundWindow(),foreground,128);if(IsIconic(windows[0])&&(wcscmp(foreground,L"Progman")==0||wcscmp(foreground,L"WorkerW")==0))break;Sleep(10);}while(GetTickCount64()<deadline);
        const bool reached=IsIconic(windows[0])&&(wcscmp(foreground,L"Progman")==0||wcscmp(foreground,L"WorkerW")==0);
        expect(desktop.toggle(windows[0],windows[0])==E_PENDING,"Native shell restore was not queued");finish(desktop);pump();
        expect(!IsIconic(windows[0]),"Native shell button did not restore the fixture");
        expect(reached,"Native shell button did not reach the desktop");
        std::cout<<"Native shell delegation passed: real Windows Show Desktop command, desktop foreground reached, second-click restore.\n";
        for(auto window:windows)if(IsWindow(window))DestroyWindow(window);DestroyWindow(tool);DestroyWindow(popup);return 0;
    }
    pump();const auto normal=bounds(windows[0]);
    expect(desktop.toggle_windows(windows,windows[0])==E_PENDING,"First toggle did not queue native work");finish(desktop);
    for(auto window:windows)expect(IsIconic(window),"Visible fixture did not minimize");
    expect(desktop.toggle_windows(windows,windows[0])==E_PENDING,"Second toggle did not queue restore");finish(desktop);
    expect(!IsIconic(windows[0])&&IsZoomed(windows[1]),"Original normal/maximized states were not restored");
    expect(IsIconic(windows[2]),"Originally minimized window was incorrectly restored");const auto restored=bounds(windows[0]);expect(EqualRect(&normal,&restored),"Normal window position changed");
    const auto game_restored=bounds(windows[3]);expect(!IsIconic(windows[3])&&EqualRect(&area,&game_restored)&&!(GetWindowLongPtrW(windows[3],GWL_STYLE)&WS_MINIMIZEBOX),"Borderless game geometry/style was not restored");
    desktop.toggle_windows(windows,windows[0]);finish(desktop);
    ShowWindow(windows[0],SW_RESTORE);pump();
    desktop.toggle_windows(windows,windows[0]);finish(desktop);
    expect(IsIconic(windows[0])&&IsIconic(windows[1]),"Manual app interaction did not start a new desktop cycle");
    desktop.toggle_windows(windows,windows[0]);finish(desktop);
    expect(!IsIconic(windows[0])&&IsIconic(windows[1]),"New cycle restored windows outside that cycle");
    ShowWindow(windows[1],SW_RESTORE);pump();desktop.toggle_windows(windows,windows[0]);finish(desktop);
    DestroyWindow(windows[1]);windows[1]=nullptr;
    desktop.toggle_windows(windows,windows[0]);finish(desktop);
    expect(!IsIconic(windows[0]),"Closing a minimized window prevented remaining restore");
    desktop.toggle_windows(windows,windows[0]);finish(desktop);desktop.restore_on_exit();pump();
    expect(!IsIconic(windows[0])&&IsIconic(windows[2]),"Exit cleanup failed or restored a pre-minimized window");
    expect(desktop.toggle_windows({},nullptr)==S_OK&&!desktop.busy(),"Empty desktop should settle immediately");
    std::cout<<"Desktop toggle passed: borderless fullscreen games without minimize buttons, candidate exclusions, native minimize/restore, normal placement, maximized state, pre-minimized exclusion, manual interaction, closed-window exclusion, exit restoration and empty desktop.\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}
desktop.restore_on_exit();pump();for(auto window:windows)if(IsWindow(window))DestroyWindow(window);if(tool)DestroyWindow(tool);if(popup)DestroyWindow(popup);return result;}
