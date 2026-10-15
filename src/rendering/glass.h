#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <memory>
#include <vector>
#include "platform/com_ptr.h"

namespace minima {
// One rounded shape whose area shows the live, blurred desktop behind the
// window. `rect`/`radius` are in DIPs under `transform` (uniform scale plus
// translation, relative to the drawing origin); `smooth` marks continuous
// corners; `opacity` fades the backdrop with a fading flyout.
struct GlassShape {
    D2D1_MATRIX_3X2_F transform;
    D2D1_RECT_F rect;
    float radius=0;
    bool smooth=false;
    float opacity=1;
};
// Real glass for an owned popup: Windows.UI.Composition draws a host backdrop
// (the system's blurred view of whatever is behind the window) clipped by an
// anti-aliased mask, with the app's Direct2D content composited above it. The
// window must be created with WS_EX_NOREDIRECTIONBITMAP. Opacity and vertical
// offset are applied by the compositor, so fades cost no per-pixel CPU work.
class GlassHost {
public:
    GlassHost();
    ~GlassHost();
    GlassHost(const GlassHost&)=delete;
    GlassHost& operator=(const GlassHost&)=delete;
    // Creates the compositor/device chain once per GUI thread; false when this
    // Windows build or GPU cannot provide composition glass.
    static bool available();
    // above_content=false places the visuals below the window content, so DWM
    // thumbnails registered on the window draw on top of the glass.
    void attach(HWND window,ID2D1Factory* factory,bool above_content=true);
    void detach();
    bool attached() const;
    // Content pass. The device context is drawing-ready (no BeginDraw), has
    // the window DPI and `origin` already applied; draw in window DIPs.
    ID2D1DeviceContext* begin(UINT width,UINT height,float dpi,D2D1_MATRIX_3X2_F& origin);
    void end();
    // Place one clipped backdrop pane per shape, then commit visual properties.
    void backdrops(const std::vector<GlassShape>& shapes,float dpi);
    void present(float opacity,float offset_pixels,bool backdrop);
    // Increments whenever drawing surfaces or the device are (re)created.
    unsigned allocations() const;
    // Name of the step that last failed (for error reports), or "".
    static const char* failure();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int run_glass_probe(HINSTANCE instance,unsigned seconds,const wchar_t* report=nullptr);
}
