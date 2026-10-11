#pragma once
#include <windows.h>
#include <string>
namespace minima {
int run_preview(HINSTANCE instance, int show);
int run_desktop_dock(HINSTANCE instance);
int benchmark_dock_render(HINSTANCE instance,const std::wstring& path);
int save_preview_snapshot(const std::wstring& path, bool expanded, int panel=0);
}
