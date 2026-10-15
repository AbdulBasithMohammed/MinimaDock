#include "rendering/preview.h"
#include "rendering/calendar.h"
#include "rendering/glass.h"
#include "rendering/prayer_toast.h"
#include "windows/jump_lists.h"
#include "windows/connectivity.h"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
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
    else if (count == 3 && wcscmp(args[1], L"--snapshot-wifi") == 0) result = minima::save_preview_snapshot(args[2], true,14);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-bluetooth") == 0) result = minima::save_preview_snapshot(args[2], true,15);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-timer") == 0) result = minima::save_preview_snapshot(args[2], true,16);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-prayer-clock") == 0) result = minima::save_preview_snapshot(args[2], true,17);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-hover") == 0) result = minima::save_preview_snapshot(args[2], false,18);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-tray-hover") == 0) result = minima::save_preview_snapshot(args[2], true,8);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-wifi-hover") == 0) result = minima::save_preview_snapshot(args[2], true,9);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-sound-hover") == 0) result = minima::save_preview_snapshot(args[2], true,10);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-clock-hover") == 0) result = minima::save_preview_snapshot(args[2], true,11);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-media") == 0) result = minima::save_preview_snapshot(args[2], true,12);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-media-paused") == 0) result = minima::save_preview_snapshot(args[2], true,13);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-calendar") == 0) result = minima::save_calendar_snapshot(args[2]);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-calendar-prayers") == 0) {
        minima::PrayerView view;view.visible=true;view.status=L"Updated 9:02 AM";view.website=L"https://www.islamicsocietyoftoronto.com/";
        view.rows={{L"Fajr",L"6:25 AM",L"6:40 AM",0},{L"Sunrise",L"7:32 AM",L"\x2014",0},{L"Zuhr",L"1:30 PM",L"1:45 PM",0},{L"Asr",L"5:00 PM",L"5:10 PM",1},{L"Maghrib",L"6:37 PM",L"6:40 PM",2},{L"Isha",L"8:30 PM",L"8:45 PM",2}};
        result = minima::save_calendar_snapshot(args[2], false, &view);
    }
    else if (count == 3 && wcscmp(args[1], L"--snapshot-calendar-motion") == 0) result = minima::save_calendar_snapshot(args[2],true);
    else if ((count == 3 || count == 4) && wcscmp(args[1], L"--glass-probe") == 0) result = minima::run_glass_probe(instance, static_cast<unsigned>(std::clamp(_wtoi(args[2]),1,30)), count == 4 ? args[3] : nullptr);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher") == 0) result = minima::save_preview_snapshot(args[2], true,19);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-actions") == 0) result = minima::save_preview_snapshot(args[2], true,20);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-home") == 0) result = minima::save_preview_snapshot(args[2], true,21);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-emoji") == 0) result = minima::save_preview_snapshot(args[2], true,22);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-command") == 0) result = minima::save_preview_snapshot(args[2], true,23);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-launcher-windows") == 0) result = minima::save_preview_snapshot(args[2], true,24);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-prayer-toast") == 0) result = minima::save_prayer_toast_snapshot(args[2], false);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-prayer-toast-pill") == 0) result = minima::save_prayer_toast_snapshot(args[2], true);
    else if (count == 3 && wcscmp(args[1], L"--snapshot-prayer-toast-compare") == 0) result = minima::save_prayer_toast_compare(args[2]);
    else if (count == 2 && wcscmp(args[1], L"--prayer-toast-demo") == 0) result = minima::run_prayer_toast_demo();
    else if (count == 3 && wcscmp(args[1], L"--quick-settings-report") == 0) result = minima::run_quick_settings_report(args[2]);
    else if (count == 3 && wcscmp(args[1], L"--jump-list-report") == 0) result = minima::run_jump_list_report(args[2]);
    else if (count == 2 && wcscmp(args[1], L"--dock") == 0) result = minima::run_desktop_dock(instance);
    else if (count == 3 && wcscmp(args[1], L"--benchmark-render") == 0) result = minima::benchmark_dock_render(instance,args[2]);
    else if (count == 1 || (count == 2 && wcscmp(args[1], L"--preview") == 0)) result = minima::run_preview(instance, show);
    else {
        MessageBoxW(nullptr, L"This build contains a framed design preview. Shell activation is disabled until the recovery, compatibility, and performance gates pass.\n\nUse --preview.", L"MinimaDock", MB_OK | MB_ICONINFORMATION);
        result = 2;
    }
    LocalFree(args); CoUninitialize(); return result;
}
