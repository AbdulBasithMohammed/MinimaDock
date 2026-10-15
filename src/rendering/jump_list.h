#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "platform/com_ptr.h"
#include "rendering/theme.h"
#include "windows/inventory.h"

namespace minima {
// wParam: chosen entry id (0 = dismissed); lParam: the session returned by open().
// Posted exactly once per session, as soon as the list starts closing.
inline constexpr UINT jump_list_event_message=WM_APP+18;
struct JumpEntry {
    UINT id=0;
    std::wstring text;
    ComPtr<IShellItem> item; // Icon source: the Shell's own icon for the item.
    NativeIcon icon;         // Fallback icon.
    wchar_t glyph=0;         // Segoe Fluent Icons glyph instead of an icon.
    bool enabled=true;
    bool separator=false;    // A task separator line instead of an entry.
};
// Titled sections stack directly (Pinned, Recent, Tasks); an untitled section
// gets a full-width line above it. `fixed` sections are never shortened to fit.
struct JumpSection {std::wstring title;std::vector<JumpEntry> entries;bool fixed=false;};
// The Windows 11 taskbar jump list, drawn with the system flyout's material,
// metrics and theme: the app's own sections, then the app, pin and close
// entries. Takes foreground so Escape, the arrow keys and Enter work; losing
// activation dismisses it. Opened with `activate` false it is a card that never
// takes focus (the timer alarm over a game) and closes only when clicked.
class JumpList {
public:
    // Right-click or context key on an entry, at a screen point. Runs inside the
    // list's window procedure; `window` owns any menu shown. Return true to close.
    using ContextHandler=std::function<bool(UINT id,POINT point,HWND window)>;
    // Lets nested native Shell menus owned by the list receive their messages.
    using MenuMessages=std::function<bool(UINT message,WPARAM w,LPARAM l,LRESULT& result)>;
    // `message` receives the events (each instance needs its own).
    explicit JumpList(UINT message=jump_list_event_message);
    ~JumpList();
    JumpList(const JumpList&)=delete;
    JumpList& operator=(const JumpList&)=delete;
    // `anchor` is the icon's screen rectangle and `dock_top` the pill's top edge.
    unsigned open(HWND owner,std::vector<JumpSection> sections,RECT anchor,LONG dock_top,UINT dpi,const theme::Palette& palette,
                  bool live,bool reduced,bool keyboard,ContextHandler context,MenuMessages messages,bool activate=true);
    void close(bool animated=true);
    bool visible() const;
    HWND window() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
