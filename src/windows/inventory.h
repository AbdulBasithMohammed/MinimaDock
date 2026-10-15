#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
namespace minima {
using NativeIcon = std::shared_ptr<void>;
struct AppWindow { HWND hwnd; std::wstring title; std::wstring executable; HMONITOR monitor; bool minimized; NativeIcon icon; std::wstring app_id,icon_source; };
struct Shortcut { std::wstring title; std::wstring path; bool folder; std::wstring target; NativeIcon icon; std::wstring app_id; };
struct Inventory { std::vector<AppWindow> windows; std::vector<Shortcut> desktop; std::vector<Shortcut> launcher; std::vector<Shortcut> pinned; };
Inventory collect_inventory(DWORD own_pid);
std::vector<Shortcut> windows_pins();       // Explorer's taskbar pins, before dock-only overrides.
std::wstring taskbar_pins_folder();
std::wstring shortcut_target(const std::wstring& path); // A .lnk file's target.
std::vector<AppWindow> collect_windows(DWORD own_pid);
bool activate_window(HWND hwnd);
}
