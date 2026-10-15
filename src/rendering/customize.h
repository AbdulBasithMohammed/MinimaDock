#pragma once
#include "rendering/dock_settings.h"
#include "windows/prayer_times.h"
#include "windows/app_update.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace minima {
// What the settings window reads and changes in the running dock. Every
// change is applied at once through these callbacks (no Save button).
struct CustomizeHooks {
    std::function<void(const DockSettings&)> dock_changed;
    std::function<void(const PrayerSettings&,bool website_changed)> prayer_changed;
    std::function<PrayerSnapshot()> prayer_status;
    std::function<void()> check_prayers;
    std::function<bool()> windows_key;           // Empty when not a live dock.
    std::function<void(bool)> set_windows_key;
    std::function<void()> clear_history;
    std::function<void()> return_to_windows;     // Empty outside the live dock.
    std::function<bool()> start_with_windows;    // Empty outside the live dock.
    std::function<void(bool)> set_start_with_windows;
    std::function<void()> reset_everything;
    std::function<AppUpdateStatus()> update_status;  // Empty outside the live dock.
    std::function<void()> check_updates;
    std::function<void()> install_update;
    std::function<bool()> auto_update;
    std::function<void(bool)> set_auto_update;
    std::wstring version;
};
// MinimaDock's settings window ("MinimaDock settings" on the dock's
// right-click menu): Appearance, Dock, Prayer times, Launcher, About and updates.
// Drawn with the dock's own glass and palette; follows changes live.
class CustomizeWindow {
public:
    CustomizeWindow();
    ~CustomizeWindow();
    CustomizeWindow(const CustomizeWindow&)=delete;
    CustomizeWindow& operator=(const CustomizeWindow&)=delete;
    void open(HWND owner,const DockSettings& dock,const PrayerSettings& prayer,CustomizeHooks hooks,bool reduced_motion);
    void close();
    bool visible() const;
    // The dock calls these when prayer times arrive or settings change elsewhere.
    void prayer_updated();
    void refresh(const DockSettings& dock,const PrayerSettings& prayer,bool reduced_motion);
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
// --snapshot-customize <path> [page]: the window rendered to a PNG with sample data.
int save_customize_snapshot(const std::wstring& path,int page);
}
