#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>

namespace {
std::filesystem::path root;
HWND visible_bar=nullptr,hidden_bar=nullptr,unrelated=nullptr;
unsigned checks=0;
void expect(bool result,const char* label){++checks;if(!result)throw std::runtime_error(label);}
void pump(unsigned milliseconds) {
    const auto end=GetTickCount64()+milliseconds;
    do {MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);}while(GetTickCount64()<end);
}
template<class F> bool until(F predicate,unsigned timeout=6000){const auto end=GetTickCount64()+timeout;do{pump(20);if(predicate())return true;}while(GetTickCount64()<end);return false;}
struct Process {
    HANDLE handle=nullptr;DWORD pid=0;
    Process(const std::wstring& exe,const std::wstring& args) {
        auto command=L"\""+(root/exe).wstring()+L"\" "+args;STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
        if(!CreateProcessW((root/exe).c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,root.c_str(),&startup,&info))throw std::runtime_error("Process launch failed.");
        handle=info.hProcess;pid=info.dwProcessId;CloseHandle(info.hThread);
    }
    ~Process(){if(handle){if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT){TerminateProcess(handle,99);WaitForSingleObject(handle,1000);}CloseHandle(handle);}}
    Process(const Process&)=delete;
    bool done() const{return WaitForSingleObject(handle,0)==WAIT_OBJECT_0;}
    DWORD code() const{DWORD value=999;GetExitCodeProcess(handle,&value);return value;}
};
HWND control(){return FindWindowW(L"MinimaDock.Trial.TestControl",nullptr);}
HWND ui(){return FindWindowW(L"MinimaDock.DesktopDock",nullptr);}
DWORD command(const std::wstring& action){Process p(L"MinimaDockTrialTest.exe",action);expect(until([&]{return p.done();},9000),"command completes");return p.code();}
void deployed(){expect(until([]{return !IsWindowVisible(visible_bar)&&control()&&ui();},18000),"ready dock hides mock visible taskbar");expect(!IsWindowVisible(hidden_bar),"previously hidden taskbar remains hidden");expect(IsWindowVisible(unrelated),"unrelated window is untouched");}
void reverted(Process& guard){expect(until([&]{return guard.done();},16000),"guard shuts down");expect(until([]{return IsWindowVisible(visible_bar)!=FALSE;}),"visible taskbar restored");expect(!IsWindowVisible(hidden_bar),"original hidden state restored");expect(IsWindowVisible(unrelated),"unrelated window survives recovery");expect(!ui(),"dock removed");}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}
}
int wmain(int argc,wchar_t** argv){
    try {
        wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);root=std::filesystem::path(path).parent_path();
        WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.MockTaskbar";RegisterClassW(&klass);
        visible_bar=CreateWindowExW(WS_EX_TOOLWINDOW,klass.lpszClassName,L"Mock taskbar",WS_POPUP,0,0,1,1,nullptr,nullptr,klass.hInstance,nullptr);
        hidden_bar=CreateWindowExW(WS_EX_TOOLWINDOW,klass.lpszClassName,L"Hidden mock taskbar",WS_POPUP,0,0,1,1,nullptr,nullptr,klass.hInstance,nullptr);
        unrelated=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Unrelated test window",WS_POPUP,0,0,1,1,nullptr,nullptr,klass.hInstance,nullptr);
        ShowWindow(visible_bar,SW_SHOWNOACTIVATE);ShowWindow(unrelated,SW_SHOWNOACTIVATE);
        const std::wstring scenario=argc==2?argv[1]:L"revert";
        Process guard(L"MinimaDockTrialTest.exe",L"--trial 10");deployed();
        if(scenario==L"revert")expect(command(L"--revert")==0,"explicit revert accepted");
        else if(scenario==L"crash") {
            DWORD pid=0;GetWindowThreadProcessId(ui(),&pid);HANDLE dock=OpenProcess(PROCESS_TERMINATE,FALSE,pid);expect(dock!=nullptr,"can terminate own fixture dock");TerminateProcess(dock,88);CloseHandle(dock);
        } else if(scenario==L"keep") {
            expect(command(L"--keep")==0,"keep accepted");pump(11000);expect(!guard.done()&&!IsWindowVisible(visible_bar),"keep cancels timeout while supervision remains");expect(command(L"--status")==2,"status reports kept trial");expect(command(L"--revert")==0,"kept trial remains reversible");
        } else if(scenario==L"fallback") {
            TerminateProcess(guard.handle,77);expect(until([&]{return guard.done();}),"guard crash simulated");expect(command(L"--revert")==0,"standalone journal recovery succeeds");
        } else if(scenario==L"hang") {
            // Suspend only this test dock's UI thread. Guard must detect bounded WM_NULL failure.
            DWORD pid=0;const auto tid=GetWindowThreadProcessId(ui(),&pid);HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME,FALSE,tid);expect(thread!=nullptr,"can suspend own test UI thread");SuspendThread(thread);CloseHandle(thread);
        } else if(scenario!=L"timeout")throw std::runtime_error("Unknown scenario.");
        reverted(guard);expect(!std::filesystem::exists(root/L"taskbar-trial-test.state"),"restored journal cleaned");
        DestroyWindow(visible_bar);DestroyWindow(hidden_bar);DestroyWindow(unrelated);
        std::wcout<<scenario<<L": "<<checks<<L" checks passed; real Windows taskbar untouched.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
