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
// Glass card of live DWM thumbnails for one app's windows, shown above its
// dock icon. Non-activating; clicking a tile switches to that window, the
// close button (or middle click) asks it to close.
class WindowPreviews {
public:
    WindowPreviews();
    ~WindowPreviews();
    WindowPreviews(const WindowPreviews&)=delete;
    WindowPreviews& operator=(const WindowPreviews&)=delete;
    // Opens (or retargets) for `key`. `anchor` is the icon's screen rectangle
    // and `dock_top` the pill's top edge in screen pixels.
    void open(HWND owner,const std::wstring& key,const std::wstring& name,const std::vector<HWND>& windows,NativeIcon icon,
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
