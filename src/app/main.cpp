#include "rendering/preview.h"
#include "rendering/calendar.h"
#include <shellapi.h>
#include <objbase.h>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    int result = 0;
    if (count == 3 && wcscmp(args[1], L"--snapshot") == 0) result = minima::save_preview_snapshot(args[2], true);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-collapsed") == 0) result = minima::save_preview_snapshot(args[2], false);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-settings") == 0) result = minima::save_preview_snapshot(args[2], true,1);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-tray") == 0) result = minima::save_preview_snapshot(args[2], true,2);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-dock") == 0) result = minima::save_preview_snapshot(args[2], true,3);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-desktop-hover") == 0) result = minima::save_preview_snapshot(args[2], true,4);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-hover") == 0) result = minima::save_preview_snapshot(args[2], true,5);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-click") == 0) result = minima::save_preview_snapshot(args[2], true,6);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-panel-reveal") == 0) result = minima::save_preview_snapshot(args[2], true,7);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-tray-hover") == 0) result = minima::save_preview_snapshot(args[2], true,8);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-wifi-hover") == 0) result = minima::save_preview_snapshot(args[2], true,9);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-sound-hover") == 0) result = minima::save_preview_snapshot(args[2], true,10);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-clock-hover") == 0) result = minima::save_preview_snapshot(args[2], true,11);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-media") == 0) result = minima::save_preview_snapshot(args[2], true,12);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-media-paused") == 0) result = minima::save_preview_snapshot(args[2], true,13);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-calendar") == 0) result = minima::save_calendar_snapshot(args[2]);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-calendar-motion") == 0) result = minima::save_calendar_snapshot(args[2],true);
    else if (count == 2 && wcscmp(args[1], L"--dock") == 0) result = minima::run_desktop_dock(instance);
    else if (count == 3 && wcscmp(args[1], L"--benchmark-render") == 0) result = minima::benchmark_dock_render(instance,args[2]);
    else if (count == 1 || (count == 2 && wcscmp(args[1], L"--preview") == 0)) result = minima::run_preview(instance, show);
    else {
        MessageBoxW(nullptr, L"This build contains a framed design preview. Shell activation is disabled until the recovery, compatibility, and performance gates pass.\n\nUse --preview.", L"MinimaDock", MB_OK | MB_ICONINFORMATION);
        result = 2;
    }
    LocalFree(args); CoUninitialize(); return result;
}
