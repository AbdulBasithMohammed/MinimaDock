#pragma once
#include <windows.h>

namespace minima {
inline HWND explorer_taskbar() {
    const auto shell=GetShellWindow();DWORD shell_process=0;GetWindowThreadProcessId(shell,&shell_process);
    if(!shell_process||shell_process==GetCurrentProcessId())return nullptr;
    struct Search {DWORD process;HWND window=nullptr;} search{shell_process};
    EnumWindows([](HWND window,LPARAM parameter)->BOOL{
        auto& search=*reinterpret_cast<Search*>(parameter);DWORD process=0;GetWindowThreadProcessId(window,&process);
        wchar_t type[128]{};GetClassNameW(window,type,128);
        if(process==search.process&&wcscmp(type,L"Shell_TrayWnd")==0){search.window=window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.window;
}
// Explorer compatibility adapter for the approved temporary trial. The stock
// Show Desktop command (407) works while its taskbar is hidden, unlike querying
// the hidden XAML button's Invoke pattern. This is an undocumented shell command;
// validate it on supported Windows builds before extending compatibility claims.
inline HRESULT invoke_shell_desktop(HWND taskbar) {
    if(taskbar!=explorer_taskbar())return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    DWORD_PTR result=0;SetLastError(ERROR_SUCCESS);
    if(SendMessageTimeoutW(taskbar,WM_COMMAND,MAKEWPARAM(407,0),0,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&result))return S_OK;
    const auto error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_TIMEOUT);
}
}
