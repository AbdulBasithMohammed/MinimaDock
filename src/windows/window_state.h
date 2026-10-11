#pragma once
#include <windows.h>

namespace minima {
// Windows exposes snap as a separate arranged state, including quarter layouts.
// Resolve once because some Windows SDKs omit its header/import-library entry.
inline bool window_arranged(HWND window) {
    using Query=BOOL (WINAPI*)(HWND);
    static const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"IsWindowArranged"));
    return query&&query(window)!=FALSE;
}
}
