#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <iomanip>
#include <array>
#include <atomic>
#include "recovery/work_area.h"

// Temporary Explorer-backed trial. Persist visibility and monitor work areas
// BEFORE changing either. Never changes startup, Explorer lifetime or wallpaper.
namespace {
constexpr UINT revert_message=WM_APP+20,keep_message=WM_APP+21,status_message=WM_APP+22;
#ifndef MINIMA_TRIAL_TEST
constexpr UINT snap_changed_message=WM_APP+23,snap_foreground_message=WM_APP+24;
// The dock nudges the supervisor when its persistent bar starts or stops
// reserving space, and answers bar_reserve_query with 1 while it should.
constexpr UINT bar_reserve_changed=WM_APP+25,bar_reserve_query=WM_APP+120;
HWND snap_control=nullptr;
std::atomic_bool snap_queued=false;
void CALLBACK snap_event(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG child,DWORD,DWORD) {
    // Location hooks follow only the foreground process; re-target on activation.
    if(event==EVENT_SYSTEM_FOREGROUND){if(snap_control&&window)PostMessageW(snap_control,snap_foreground_message,reinterpret_cast<WPARAM>(window),0);return;}
    if(snap_control&&window&&object==OBJID_WINDOW&&child==CHILDID_SELF&&GetAncestor(window,GA_ROOT)==window&&
       minima::window_arranged(window)&&!snap_queued.exchange(true))PostMessageW(snap_control,snap_changed_message,0,0);
}
#endif
#ifdef MINIMA_TRIAL_TEST
constexpr auto control_class=L"MinimaDock.Trial.TestControl";
constexpr auto trial_image=L"MinimaDockTrialTest.exe";
constexpr auto state_name=L"taskbar-trial-test.state";
#else
constexpr auto control_class=L"MinimaDock.Trial.Control";
constexpr auto trial_image=L"MinimaDockTrial.exe";
constexpr auto state_name=L"taskbar-trial.state";
#endif
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::filesystem::path directory() {
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))throw std::runtime_error("Cannot locate deployment.");
    return std::filesystem::path(path).parent_path();
}
std::wstring sid(HANDLE process) {
    HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&raw))throw std::runtime_error("Cannot read process identity.");
    Handle token(raw);DWORD bytes=0;GetTokenInformation(raw,TokenUser,nullptr,0,&bytes);
    std::vector<BYTE> data(bytes);if(!GetTokenInformation(raw,TokenUser,data.data(),bytes,&bytes))throw std::runtime_error("Cannot validate process identity.");
    wchar_t* text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text))throw std::runtime_error("Cannot read SID.");
    std::wstring value(text);LocalFree(text);return value;
}
const std::wstring& own_sid() {static const auto value=sid(GetCurrentProcess());return value;}
bool same_session(HANDLE process,DWORD pid) {
    DWORD current=0,target=0;return ProcessIdToSessionId(GetCurrentProcessId(),&current)&&ProcessIdToSessionId(pid,&target)&&current==target&&sid(process)==own_sid();
}
std::wstring image(HANDLE process) {
    std::wstring path(32768,L'\0');DWORD count=32768;if(!QueryFullProcessImageNameW(process,0,path.data(),&count))return {};
    path.resize(count);return path;
}
unsigned long long created(HANDLE process) {
    FILETIME a{},b{},c{},d{};if(!GetProcessTimes(process,&a,&b,&c,&d))return 0;
    return (static_cast<unsigned long long>(a.dwHighDateTime)<<32)|a.dwLowDateTime;
}
bool same_image(const std::wstring& a,const std::filesystem::path& b){return _wcsicmp(a.c_str(),b.c_str())==0;}
bool bar_class(HWND window) {
    wchar_t klass[128]{};GetClassNameW(window,klass,128);
#ifdef MINIMA_TRIAL_TEST
    return wcscmp(klass,L"MinimaDock.MockTaskbar")==0;
#else
    return wcscmp(klass,L"Shell_TrayWnd")==0||wcscmp(klass,L"Shell_SecondaryTrayWnd")==0;
#endif
}
bool bar_owner(HANDLE process,DWORD pid) {
#ifdef MINIMA_TRIAL_TEST
    return same_session(process,pid)&&same_image(image(process),directory()/L"TrialTests.exe");
#else
    wchar_t windows[32768]{};if(!GetWindowsDirectoryW(windows,32768))return false;
    return same_session(process,pid)&&same_image(image(process),std::filesystem::path(windows)/L"explorer.exe");
#endif
}
struct Bar {unsigned long long hwnd=0,time=0;DWORD pid=0;bool visible=false;};
bool valid(const Bar& b) {
    const auto window=reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd));DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    if(pid!=b.pid||!bar_class(window))return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    return process.value&&created(process.value)==b.time&&bar_owner(process.value,pid);
}
struct Journal {
    DWORD dock_pid=0;unsigned long long dock_time=0;std::vector<Bar> bars;std::vector<minima::WorkArea> areas;
    void save() const {
        DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
        std::wostringstream data;data<<L"MinimaDockTaskbarTrial2 "<<own_sid()<<L' '<<session<<L' '<<dock_pid<<L' '<<dock_time<<L' '<<bars.size()<<L' '<<areas.size()<<L'\n';
        for(const auto& b:bars)data<<b.hwnd<<L' '<<b.time<<L' '<<b.pid<<L' '<<b.visible<<L'\n';
        for(const auto& a:areas) {
            data<<std::quoted(a.device);
            for(const auto& r:{a.bounds,a.original,a.applied})data<<L' '<<r.left<<L' '<<r.top<<L' '<<r.right<<L' '<<r.bottom;
            data<<L'\n';
        }
        const auto wide=data.str();const std::string text(wide.begin(),wide.end());
        const auto temp=directory()/(std::wstring(state_name)+L".tmp"),path=directory()/state_name;
        Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        DWORD bytes=0;if(file.value==INVALID_HANDLE_VALUE||!WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&bytes,nullptr)||bytes!=text.size()||!FlushFileBuffers(file.value))throw std::runtime_error("Cannot persist taskbar recovery journal.");
        CloseHandle(file.value);file.value=nullptr;
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit taskbar recovery journal.");
    }
    static Journal load() {
        Journal j;const auto path=directory()/state_name;if(!std::filesystem::exists(path))return j;
        if(std::filesystem::file_size(path)>16384)throw std::runtime_error("Invalid taskbar recovery journal.");
        std::wifstream file(path);std::wstring magic,user;DWORD session=0,current=0;
        if(!(file>>magic>>user>>session>>j.dock_pid>>j.dock_time)||(magic!=L"MinimaDockTaskbarTrial1"&&magic!=L"MinimaDockTaskbarTrial2")||user!=own_sid()||!ProcessIdToSessionId(GetCurrentProcessId(),&current))throw std::runtime_error("Taskbar journal belongs to another user or session.");
        if(session!=current) {
            // Left by an earlier sign-in (a shutdown can end the session before the
            // journal is removed). Windows recreates the taskbar and resets the work
            // area at every sign-in, so there is nothing to restore: set it aside.
            // A journal whose dock is still running belongs to another live session.
            Handle dock(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,j.dock_pid));
            if(dock.value&&created(dock.value)==j.dock_time)throw std::runtime_error("Taskbar journal belongs to another user or session.");
            file.close();std::error_code ignored;
            std::filesystem::rename(path,directory()/(std::wstring(state_name)+L".previous-session"),ignored);
            return Journal{};
        }
        if(magic==L"MinimaDockTaskbarTrial1") {
            Bar b;while(file>>b.hwnd>>b.time>>b.pid>>b.visible){if(j.bars.size()>=32)throw std::runtime_error("Too many taskbars in journal.");j.bars.push_back(b);}
        } else {
            size_t bar_count=0,area_count=0;
            if(!(file>>bar_count>>area_count)||bar_count>32||area_count>32)throw std::runtime_error("Invalid taskbar journal counts.");
            for(size_t i=0;i<bar_count;++i){Bar b;if(!(file>>b.hwnd>>b.time>>b.pid>>b.visible))throw std::runtime_error("Malformed taskbar record.");j.bars.push_back(b);}
            for(size_t i=0;i<area_count;++i) {
                minima::WorkArea a;
                if(!(file>>std::quoted(a.device))||a.device.empty()||a.device.size()>32)throw std::runtime_error("Malformed monitor identity.");
                for(auto* r:{&a.bounds,&a.original,&a.applied})if(!(file>>r->left>>r->top>>r->right>>r->bottom))throw std::runtime_error("Malformed work area.");
                if(!minima::contained_rect(a.bounds,a.original)||!minima::contained_rect(a.bounds,a.applied)||a.original.left!=a.applied.left||a.original.top!=a.applied.top||a.original.right!=a.applied.right||(a.applied.bottom!=a.bounds.bottom&&a.applied.bottom!=a.original.bottom))throw std::runtime_error("Invalid work area recovery record.");
                if(std::any_of(j.areas.begin(),j.areas.end(),[&](const auto& old){return old.device==a.device;}))throw std::runtime_error("Duplicate monitor recovery record.");
                j.areas.push_back(a);
            }
            file>>std::ws;
        }
        if(!file.eof())throw std::runtime_error("Malformed taskbar recovery journal.");return j;
    }
    void restore() const {
        for(const auto& b:bars)if(valid(b))ShowWindowAsync(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd)),b.visible?SW_SHOWNOACTIVATE:SW_HIDE);
        for(const auto& a:areas) {
            MONITORINFOEXW info{};
            // Never replay coordinates onto a moved, resized or replacement monitor.
            if(!minima::same_rect(a.original,a.applied)&&minima::current_work_area(a,info)) {
                if(minima::same_rect(info.rcWork,a.applied))minima::set_work_area(a.applied,a.original);
                else if(minima::same_rect(info.rcWork,a.original))minima::reflow_work_area_windows(a.applied,a.original);
            }
        }
    }
    bool restored() const {
        for(const auto& b:bars)if(valid(b)&&((IsWindowVisible(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd)))!=FALSE)!=b.visible))return false;
        for(const auto& a:areas){MONITORINFOEXW info{};if(!minima::same_rect(a.original,a.applied)&&minima::current_work_area(a,info)&&!minima::same_rect(info.rcWork,a.original))return false;}
        return true;
    }
    void remove() const {std::error_code error;std::filesystem::remove(directory()/state_name,error);}
};
HWND find_control() {
    auto window=FindWindowW(control_class,nullptr);if(!window)return nullptr;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    return process.value&&same_session(process.value,pid)&&same_image(image(process.value),directory()/trial_image)?window:nullptr;
}
void fallback_revert() {
    const auto journal=Journal::load();journal.restore();
    Handle dock(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,journal.dock_pid));
    if(dock.value&&same_session(dock.value,journal.dock_pid)&&created(dock.value)==journal.dock_time&&same_image(image(dock.value),directory()/L"MinimaDock.exe")) {
        // Only the exact recorded dock process; never Explorer or other apps.
        TerminateProcess(dock.value,0);WaitForSingleObject(dock.value,2000);
    }
    for(unsigned i=0;i<20&&!journal.restored();++i)Sleep(50);
    if(journal.restored())journal.remove();else throw std::runtime_error("Windows taskbar visibility or work area has not restored. Keep the journal and retry Revert-Taskbar.cmd.");
}
struct Supervisor {
    Journal journal;Handle dock;HWND ui=nullptr,control=nullptr;bool accepted=false,active=false,journal_written=false;unsigned missed=0;ULONGLONG deadline=0;
    // Installed (started at sign-in with --run): no time limit, and a crashed or
    // hung dock is restarted (at most 3 times in 10 minutes) instead of reverting.
    bool production=false;std::vector<ULONGLONG> restarts;
    std::array<HWINEVENTHOOK,3> snap_hooks{};
    DWORD snap_process=0;
#ifndef MINIMA_TRIAL_TEST
    // System-wide EVENT_OBJECT_LOCATIONCHANGE also fires for every cursor move.
    // Snapping changes the foreground window, so watch only its process.
    void watch_location(HWND foreground) {
        DWORD pid=0;if(foreground)GetWindowThreadProcessId(foreground,&pid);
        if(!pid||pid==GetCurrentProcessId()||(pid==snap_process&&snap_hooks[0]))return;
        if(snap_hooks[0])UnhookWinEvent(snap_hooks[0]);
        snap_hooks[0]=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,snap_event,pid,0,WINEVENT_OUTOFCONTEXT);
        snap_process=snap_hooks[0]?pid:0;
    }
#endif
    ~Supervisor(){stop();}
    bool stop() {
        if(!active)return journal.restored();active=false;
#ifndef MINIMA_TRIAL_TEST
        snap_control=nullptr;
#endif
        for(auto& hook:snap_hooks){if(hook)UnhookWinEvent(hook);hook=nullptr;}
        snap_process=0;
        journal.restore();
        if(ui)PostMessageW(ui,WM_CLOSE,0,0);
        if(dock.value&&WaitForSingleObject(dock.value,1500)==WAIT_TIMEOUT)TerminateProcess(dock.value,0);
        // Keep the journal if any captured window still disagrees, enabling standalone retry.
        for(unsigned i=0;i<20&&!journal.restored();++i)Sleep(50);
        const auto restored=journal.restored();
        if(restored)journal.remove();
        return restored;
    }
    static BOOL CALLBACK enumerate(HWND window,LPARAM context) {
        auto& self=*reinterpret_cast<Supervisor*>(context);if(!bar_class(window))return TRUE;
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if(!process.value||!bar_owner(process.value,pid))return TRUE;
        const auto hwnd=static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(window)),time=created(process.value);
        if(!time)return TRUE;
        if(std::none_of(self.journal.bars.begin(),self.journal.bars.end(),[&](const Bar& b){return b.hwnd==hwnd&&b.pid==pid&&b.time==time;}))
            self.journal.bars.push_back({hwnd,time,pid,IsWindowVisible(window)!=FALSE});
        return TRUE;
    }
    void hide() {
        // Destroyed taskbars (Explorer restarts) can never be restored; drop
        // them so repeated restarts cannot exceed the journal's 32-record bound.
        const auto count=journal.bars.size();std::erase_if(journal.bars,[](const Bar& b){return !valid(b);});
        const auto kept=journal.bars.size();EnumWindows(enumerate,reinterpret_cast<LPARAM>(this));
        if(journal.bars.size()>32)throw std::runtime_error("Too many Windows taskbars to journal; trial reverted.");
        if(count!=kept||kept!=journal.bars.size()||!journal_written){journal.save();journal_written=true;}
        for(const auto& b:journal.bars)if(valid(b)&&IsWindowVisible(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd))))ShowWindowAsync(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd)),SW_HIDE);
    }
    // A persistent full-width bar keeps Windows' original reservation on the
    // dock's monitor, so the work area only ever moves between the two
    // journaled rectangles (and recovery restores `original` as before).
    bool bar_reserved(const minima::WorkArea& area) const {
#ifndef MINIMA_TRIAL_TEST
        DWORD_PTR want=0;
        if(!ui||!SendMessageTimeoutW(ui,bar_reserve_query,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,300,&want)||want!=1)return false;
        MONITORINFO info{};info.cbSize=sizeof(info);
        return GetMonitorInfoW(MonitorFromWindow(ui,MONITOR_DEFAULTTOPRIMARY),&info)&&minima::same_rect(info.rcMonitor,area.bounds);
#else
        (void)area;return false;
#endif
    }
    void expand_work_areas() {
#ifndef MINIMA_TRIAL_TEST
        const auto current=minima::monitor_work_areas();
        if(current.size()!=journal.areas.size())throw std::runtime_error("Display configuration changed; taskbar trial reverted.");
        for(const auto& a:journal.areas) {
            MONITORINFOEXW info{};
            if(!minima::current_work_area(a,info))throw std::runtime_error("Display configuration changed; taskbar trial reverted.");
            if(minima::same_rect(a.original,a.applied))continue;
            const bool reserve=bar_reserved(a);
            const auto& target=reserve?a.original:a.applied;const auto& other=reserve?a.applied:a.original;
            if(minima::same_rect(info.rcWork,target)) {
                // Explorer's snap layout can still use its cached old work area.
                minima::reflow_work_area_windows(other,target);continue;
            }
            if(!minima::same_rect(info.rcWork,other))throw std::runtime_error("Work area changed by another application; trial reverted without overwriting it.");
            if(!minima::set_work_area(other,target))throw std::runtime_error("Cannot update the taskbar work area.");
        }
#endif
    }
    // Starts MinimaDock.exe --dock and waits until it reports ready.
    void launch_dock(ULONGLONG wait) {
        std::wstring command=L"\""+(directory()/L"MinimaDock.exe").wstring()+L"\" --dock";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
        if(!CreateProcessW((directory()/L"MinimaDock.exe").c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory().c_str(),&startup,&info))throw std::runtime_error("Cannot start desktop dock.");
        CloseHandle(info.hThread);
        if(dock.value&&dock.value!=INVALID_HANDLE_VALUE)CloseHandle(dock.value);
        dock.value=info.hProcess;journal.dock_pid=info.dwProcessId;journal.dock_time=created(dock.value);active=true;
        const auto ready_deadline=GetTickCount64()+wait;
        do {
            ui=FindWindowW(L"MinimaDock.DesktopDock",nullptr);DWORD pid=0;if(ui)GetWindowThreadProcessId(ui,&pid);
            DWORD_PTR ready=0;
            if(pid==info.dwProcessId&&SendMessageTimeoutW(ui,WM_APP+40,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&ready)&&ready==1)break;
            ui=nullptr;if(WaitForSingleObject(dock.value,100)==WAIT_OBJECT_0)throw std::runtime_error("Dock exited before readiness. Windows taskbar was not hidden.");
        }while(GetTickCount64()<ready_deadline);
        if(!ui)throw std::runtime_error("Dock did not become ready. Windows taskbar was not hidden.");
    }
    // Installed mode: replace a crashed or hung dock while the taskbar stays hidden.
    bool restart_dock() {
        const auto now=GetTickCount64();
        std::erase_if(restarts,[&](ULONGLONG at){return now-at>600000;});
        if(restarts.size()>=3)return false;
        restarts.push_back(now);
        if(WaitForSingleObject(dock.value,0)!=WAIT_OBJECT_0){TerminateProcess(dock.value,1);WaitForSingleObject(dock.value,2000);}
        launch_dock(30000);journal.save();journal_written=true;missed=0;
        expand_work_areas();
        return true;
    }
    void start(unsigned seconds) {
        TOKEN_ELEVATION elevation{};DWORD bytes=0;HANDLE raw=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error("Cannot check elevation.");
        Handle token(raw);if(!GetTokenInformation(raw,TokenElevation,&elevation,sizeof(elevation),&bytes)||elevation.TokenIsElevated)throw std::runtime_error("Run the taskbar test as your normal unelevated desktop user.");
        fallback_revert();
#ifndef MINIMA_TRIAL_TEST
        // At sign-in Explorer may still be creating its taskbar.
        if(production)for(unsigned i=0;i<180&&!FindWindowW(L"Shell_TrayWnd",nullptr);++i)Sleep(500);
#endif
        launch_dock(production?60000:15000);
        accepted=accepted||production;
        EnumWindows(enumerate,reinterpret_cast<LPARAM>(this));
        if(journal.bars.empty())throw std::runtime_error("No matching taskbar found; deployment cancelled.");
#ifndef MINIMA_TRIAL_TEST
        journal.areas=minima::monitor_work_areas();
        for(auto& a:journal.areas)for(const auto& b:journal.bars) {
            RECT bar{};
            if(valid(b)&&GetWindowRect(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd)),&bar))
                a.applied=minima::without_bottom_taskbar(a.bounds,a.applied,bar);
        }
#endif
        deadline=GetTickCount64()+seconds*1000ULL;hide();
        // Explorer processes ShowWindowAsync on its own UI thread. Let that
        // operation finish before releasing the reservation it may refresh.
        const auto hidden=[&]{return std::none_of(journal.bars.begin(),journal.bars.end(),[](const Bar& b){return valid(b)&&IsWindowVisible(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(b.hwnd)));});};
        for(unsigned i=0;i<40&&!hidden();++i)Sleep(25);
        if(!hidden())throw std::runtime_error("Windows taskbar did not hide; deployment cancelled.");
        Sleep(100);expand_work_areas();
#ifndef MINIMA_TRIAL_TEST
        snap_queued=false;snap_control=control;
        snap_hooks[1]=SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND,EVENT_SYSTEM_MOVESIZEEND,nullptr,snap_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        snap_hooks[2]=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,snap_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        watch_location(GetForegroundWindow());
#endif
        SetTimer(control,1,2000,nullptr);
    }
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Supervisor*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Supervisor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,w,l);
        try {
            if(message==status_message)return self->active?(self->accepted?2:1):0;
            if(message==keep_message||(message==WM_HOTKEY&&w==2)){self->accepted=true;return 1;}
#ifndef MINIMA_TRIAL_TEST
            if(message==snap_changed_message){snap_queued=false;if(self->active)self->expand_work_areas();return 0;}
            if(message==bar_reserve_changed){if(self->active)self->expand_work_areas();return 0;}
            if(message==snap_foreground_message){if(self->active)self->watch_location(reinterpret_cast<HWND>(w));return 0;}
#endif
            if(message==revert_message||message==WM_CLOSE||(message==WM_HOTKEY&&w==1)){const auto restored=self->stop();DestroyWindow(window);return restored?1:0;}
            if(message==WM_TIMER) {
                const bool exited=WaitForSingleObject(self->dock.value,0)==WAIT_OBJECT_0;
                bool stop=exited||(!self->accepted&&GetTickCount64()>=self->deadline);
                DWORD_PTR reply=0;if(!IsWindow(self->ui)||!SendMessageTimeoutW(self->ui,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,700,&reply))++self->missed;else self->missed=0;
                if(self->production&&(exited||self->missed>=2)) {
                    // Exit code 0 is a deliberate close; anything else is a crash.
                    DWORD code=1;if(exited)GetExitCodeProcess(self->dock.value,&code);
                    if((!exited||code!=0)&&self->restart_dock()){self->hide();return 0;}
                    if(exited&&code!=0)MessageBoxW(nullptr,L"MinimaDock stopped several times, so the Windows taskbar is back. It will start again the next time you sign in.",L"MinimaDock",MB_OK|MB_ICONWARNING|MB_SETFOREGROUND);
                }
                if(stop||self->missed>=2){self->stop();DestroyWindow(window);}else {self->hide();self->expand_work_areas();}return 0;
            }
            if(message==WM_QUERYENDSESSION){self->stop();return TRUE;}
            if(message==WM_DESTROY){KillTimer(window,1);UnregisterHotKey(window,1);UnregisterHotKey(window,2);PostQuitMessage(0);return 0;}
        }catch(const std::exception& error){std::ofstream(directory()/L"guard-error.txt")<<error.what()<<'\n';self->stop();DestroyWindow(window);return 0;}
        catch(...){self->stop();DestroyWindow(window);return 0;}
        return DefWindowProcW(window,message,w,l);
    }
};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 1;
        std::wstring action=count>=2?args[1]:L"--trial";unsigned seconds=600;
        const bool production=action==L"--run";if(production)action=L"--trial";
        if(count==3&&action==L"--trial"){wchar_t* end=nullptr;const auto value=wcstoul(args[2],&end,10);if(!end||*end||value<10||value>3600){LocalFree(args);return 2;}seconds=static_cast<unsigned>(value);}
        LocalFree(args);
        if(action==L"--revert"||action==L"--keep"||action==L"--status") {
            const auto control=find_control();if(!control){if(action==L"--revert"){fallback_revert();return 0;}return 3;}
            DWORD_PTR reply=0;const auto message=action==L"--revert"?revert_message:(action==L"--keep"?keep_message:status_message);
            if(!SendMessageTimeoutW(control,message,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,6000,&reply)){
                if(action==L"--revert"){
                    DWORD pid=0;GetWindowThreadProcessId(control,&pid);Handle guard(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE,FALSE,pid));
                    if(!guard.value||!same_session(guard.value,pid)||!same_image(image(guard.value),directory()/trial_image))throw std::runtime_error("Cannot authenticate unresponsive guard.");
                    TerminateProcess(guard.value,0);WaitForSingleObject(guard.value,2000);fallback_revert();return 0;
                }return 4;
            }
            return action==L"--status"?static_cast<int>(reply):(reply==1?0:6);
        }
        if(action!=L"--trial")return 2;
        DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
        const auto mutex_name=std::wstring(L"Local.")+control_class+L"."+own_sid()+L"."+std::to_wstring(session);
        Handle lock(CreateMutexW(nullptr,FALSE,mutex_name.c_str()));if(!lock.value)throw std::runtime_error("Cannot create trial lock.");
        if(GetLastError()==ERROR_ALREADY_EXISTS){const auto control=find_control();return control?0:5;}
        Supervisor supervisor;WNDCLASSW klass{};klass.lpfnWndProc=Supervisor::proc;klass.hInstance=instance;klass.lpszClassName=control_class;
        if(!RegisterClassW(&klass))return 1;
        supervisor.control=CreateWindowExW(WS_EX_TOOLWINDOW,control_class,L"MinimaDock taskbar recovery guard",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,&supervisor);
        if(!supervisor.control)throw std::runtime_error("Cannot create recovery control.");
        if(!RegisterHotKey(supervisor.control,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_BACK)||!RegisterHotKey(supervisor.control,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'K'))throw std::runtime_error("Recovery shortcut is already in use. Nothing was hidden.");
        supervisor.production=production;supervisor.start(seconds);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        return 0;
    }catch(const std::exception& error){
#ifdef MINIMA_TRIAL_TEST
        std::ofstream(directory()/L"TrialTestFailure.log")<<error.what()<<'\n';
#endif
#ifndef MINIMA_TRIAL_TEST
        MessageBoxA(nullptr,error.what(),"MinimaDock",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
#endif
        return 1;
    }
}
