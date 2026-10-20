#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "rendering/theme.h"
#include "windows/inventory.h"

namespace minima {
// wParam: 0 = fully closed, 1 = a window was activated (lParam = HWND).
inline constexpr UINT previews_event_message=WM_APP+17;
// Windows snapped together on one monitor (a Windows 11 snap group), shown as
// one extra tile with each window where it is snapped.
struct SnapGroup {
    std::vector<HWND> windows;
    std::vector<RECT> frames; // Each window's visible frame when snapped (screen pixels).
    RECT area{};              // The work area they share.
    std::wstring label;       // "Chrome + Discord".
};
// Glass card of live DWM thumbnails for one app's windows, shown above its
// dock icon, then one tile per snap group they belong to. Non-activating;
// clicking a tile switches to that window (a group tile restores the whole
// group), the close button (or middle click) asks a window to close.
class WindowPreviews {
public:
    WindowPreviews();
    ~WindowPreviews();
    WindowPreviews(const WindowPreviews&)=delete;
    WindowPreviews& operator=(const WindowPreviews&)=delete;
    // Opens (or retargets) for `key`. `anchor` is the icon's screen rectangle
    // and `dock_top` the pill's top edge in screen pixels.
    void open(HWND owner,const std::wstring& key,const std::wstring& name,const std::vector<HWND>& windows,const std::vector<SnapGroup>& groups,NativeIcon icon,
              RECT anchor,LONG dock_top,UINT dpi,const theme::Palette& palette,bool live,bool reduced);
    void close(bool animated=true);
    bool visible() const;      // open or animating closed
    bool open_for(const std::wstring& key) const;
    bool contains(POINT screen) const;
    HWND window() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
