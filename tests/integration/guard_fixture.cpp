#include <windows.h>
#include <fstream>
#include <filesystem>
#include <string>
namespace {
std::wstring mode;
LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if(message==WM_TIMER) {
        KillTimer(hwnd,1);
        if(mode==L"crash") ExitProcess(23);
        if(mode==L"hang") Sleep(INFINITE);
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    mode=argv[1];
    WNDCLASSW klass{}; klass.lpfnWndProc=proc; klass.hInstance=GetModuleHandleW(nullptr); klass.lpszClassName=L"MinimaDock.GuardFixture";
    RegisterClassW(&klass);
    const auto hwnd=CreateWindowW(klass.lpszClassName,L"Guard fixture",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,klass.hInstance,nullptr);
    FILETIME c{},e{},k{},u{}; GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);
    ULARGE_INTEGER ticks{}; ticks.LowPart=c.dwLowDateTime; ticks.HighPart=c.dwHighDateTime;
    std::ofstream info{std::filesystem::path(argv[2])};
    info << reinterpret_cast<ULONG_PTR>(hwnd) << ' ' << ticks.QuadPart << '\n'; info.close();
    if(mode==L"crash"||mode==L"hang") SetTimer(hwnd,1,2000,nullptr);
    MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
