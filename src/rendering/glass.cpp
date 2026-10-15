#include "rendering/glass.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>

// CoreMessaging.dll (Windows 10 1709+). Declared here because the toolchain's
// dispatcherqueue.h is limited to the app partition.
struct DispatcherQueueOptionsAbi {DWORD size;int thread_type;int apartment;};
extern "C" HRESULT WINAPI CreateDispatcherQueueController(DispatcherQueueOptionsAbi,IUnknown**);
namespace minima {
namespace {
// Minimal Windows.UI.Composition ABI. The toolchain's generated WinRT header
// does not compile as C++ (boolean/BYTE template clash), so, like media_abi.h,
// declare only what is used. Identifiers and method order come from
// Windows.UI.winmd; unused slots only keep the vtable layout.
namespace wuc {
struct Vector2 {FLOAT X,Y;};
struct Vector3 {FLOAT X,Y,Z;};
struct Size {FLOAT Width,Height;};
#define SLOT(name) virtual HRESULT STDMETHODCALLTYPE name(void*)=0;
struct ICompositionClip : IInspectable {};
struct IVisual : IInspectable {
    SLOT(get_AnchorPoint) SLOT(put_AnchorPoint) SLOT(get_BackfaceVisibility) SLOT(put_BackfaceVisibility)
    SLOT(get_BorderMode) SLOT(put_BorderMode) SLOT(get_CenterPoint) SLOT(put_CenterPoint)
    SLOT(get_Clip)
    virtual HRESULT STDMETHODCALLTYPE put_Clip(ICompositionClip*)=0;
    SLOT(get_CompositeMode) SLOT(put_CompositeMode) SLOT(get_IsVisible)
    virtual HRESULT STDMETHODCALLTYPE put_IsVisible(boolean)=0;
    SLOT(get_Offset)
    virtual HRESULT STDMETHODCALLTYPE put_Offset(Vector3)=0;
    SLOT(get_Opacity)
    virtual HRESULT STDMETHODCALLTYPE put_Opacity(FLOAT)=0;
    SLOT(get_Orientation) SLOT(put_Orientation) SLOT(get_Parent) SLOT(get_RotationAngle) SLOT(put_RotationAngle)
    SLOT(get_RotationAngleInDegrees) SLOT(put_RotationAngleInDegrees) SLOT(get_RotationAxis) SLOT(put_RotationAxis)
    SLOT(get_Scale) SLOT(put_Scale) SLOT(get_Size)
    virtual HRESULT STDMETHODCALLTYPE put_Size(Vector2)=0;
    SLOT(get_TransformMatrix) SLOT(put_TransformMatrix)
};
struct IVisualCollection : IInspectable {
    SLOT(get_Count) SLOT(InsertAbove) SLOT(InsertAtBottom)
    virtual HRESULT STDMETHODCALLTYPE InsertAtTop(IVisual*)=0;
    SLOT(InsertBelow) SLOT(Remove) SLOT(RemoveAll)
};
struct IContainerVisual : IInspectable {virtual HRESULT STDMETHODCALLTYPE get_Children(IVisualCollection**)=0;};
struct ICompositionBrush : IInspectable {};
struct ICompositionSurface : IInspectable {};
struct ISpriteVisual : IInspectable {
    SLOT(get_Brush)
    virtual HRESULT STDMETHODCALLTYPE put_Brush(ICompositionBrush*)=0;
};
struct ICompositionSurfaceBrush : IInspectable {
    SLOT(get_BitmapInterpolationMode) SLOT(put_BitmapInterpolationMode) SLOT(get_HorizontalAlignmentRatio)
    virtual HRESULT STDMETHODCALLTYPE put_HorizontalAlignmentRatio(FLOAT)=0;
    SLOT(get_Stretch)
    virtual HRESULT STDMETHODCALLTYPE put_Stretch(INT32)=0;
    SLOT(get_Surface) SLOT(put_Surface) SLOT(get_VerticalAlignmentRatio)
    virtual HRESULT STDMETHODCALLTYPE put_VerticalAlignmentRatio(FLOAT)=0;
};
struct ICompositionDrawingSurface : IInspectable {SLOT(get_AlphaMode) SLOT(get_PixelFormat) SLOT(get_Size)};
struct ICompositionGraphicsDevice : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateDrawingSurface(Size,INT32,INT32,ICompositionDrawingSurface**)=0;
    SLOT(add_RenderingDeviceReplaced) SLOT(remove_RenderingDeviceReplaced)
};
struct ICompositionTarget : IInspectable {
    SLOT(get_Root)
    virtual HRESULT STDMETHODCALLTYPE put_Root(IVisual*)=0;
};
struct ICompositor : IInspectable {
    SLOT(CreateColorKeyFrameAnimation) SLOT(CreateColorBrush) SLOT(CreateColorBrushWithColor)
    virtual HRESULT STDMETHODCALLTYPE CreateContainerVisual(IContainerVisual**)=0;
    SLOT(CreateCubicBezierEasingFunction) SLOT(CreateEffectFactory) SLOT(CreateEffectFactoryWithProperties)
    SLOT(CreateExpressionAnimation) SLOT(CreateExpressionAnimationWithExpression) SLOT(CreateInsetClip)
    SLOT(CreateInsetClipWithInsets) SLOT(CreateLinearEasingFunction) SLOT(CreatePropertySet)
    SLOT(CreateQuaternionKeyFrameAnimation) SLOT(CreateScalarKeyFrameAnimation) SLOT(CreateScopedBatch)
    virtual HRESULT STDMETHODCALLTYPE CreateSpriteVisual(ISpriteVisual**)=0;
    SLOT(CreateSurfaceBrush)
    virtual HRESULT STDMETHODCALLTYPE CreateSurfaceBrushWithSurface(ICompositionSurface*,ICompositionSurfaceBrush**)=0;
    SLOT(CreateTargetForCurrentView) SLOT(CreateVector2KeyFrameAnimation) SLOT(CreateVector3KeyFrameAnimation)
    SLOT(CreateVector4KeyFrameAnimation) SLOT(GetCommitBatch)
};
struct ICompositorInterop : IUnknown {
    SLOT(CreateCompositionSurfaceForHandle) SLOT(CreateCompositionSurfaceForSwapChain)
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsDevice(IUnknown*,ICompositionGraphicsDevice**)=0;
};
#undef SLOT
}
// Interface identifiers and layouts from Windows.UI.winmd (method order is
// metadata order). The toolchain header declares only the version 1 types.
constexpr GUID iid_compositor{0xb403ca50,0x7f8c,0x4e83,{0x98,0x5f,0xcc,0x45,0x06,0x00,0x36,0xd8}};
constexpr GUID iid_compositor3{0xc9dd8ef0,0x6eb1,0x4e3c,{0xa6,0x58,0x67,0x5d,0x9c,0x64,0xd4,0xab}};
constexpr GUID iid_compositor7{0xd3483fad,0x9a12,0x53ba,{0xbf,0xc8,0x88,0xb7,0xff,0x79,0x77,0xc6}};
constexpr GUID iid_compositor_interop{0x25297d5c,0x3ad4,0x4c9c,{0xb5,0xcf,0xe3,0x6a,0x38,0x51,0x23,0x30}};
constexpr GUID iid_desktop_interop{0x29e691fa,0x4567,0x4dca,{0xb3,0x19,0xd0,0xf2,0x07,0xeb,0x68,0x07}};
constexpr GUID iid_target{0xa1bea8ba,0xd726,0x4663,{0x81,0x29,0x6b,0x5e,0x79,0x27,0xff,0xa6}};
constexpr GUID iid_visual{0x117e202d,0xa859,0x4c89,{0x87,0x3b,0xc2,0xaa,0x56,0x67,0x88,0xe3}};
constexpr GUID iid_brush{0xab0d7608,0x30c0,0x40e9,{0xb5,0x68,0xb6,0x0a,0x6b,0xd1,0xfb,0x46}};
constexpr GUID iid_clip{0x1ccd2a52,0xcfc7,0x4ace,{0x99,0x83,0x14,0x6b,0xb8,0xeb,0x6a,0x3c}};
constexpr GUID iid_surface{0x1527540d,0x42c7,0x47a6,{0xa4,0x08,0x66,0x8f,0x79,0xa9,0x0d,0xfb}};
constexpr GUID iid_surface_interop{0xfd04e6e3,0xfe0c,0x4c3c,{0xab,0x19,0xa0,0x76,0x01,0xa5,0x76,0xee}};
struct Compositor3 : IInspectable {virtual HRESULT STDMETHODCALLTYPE CreateHostBackdropBrush(IInspectable**)=0;};
// Windows 11: rectangle clips with per-corner radii (anti-aliased).
struct Compositor7 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateAnimationPropertyInfo(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateRectangleClip(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateRectangleClipWithSides(FLOAT,FLOAT,FLOAT,FLOAT,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateRectangleClipWithSidesAndRadius(FLOAT,FLOAT,FLOAT,FLOAT,wuc::Vector2,wuc::Vector2,wuc::Vector2,wuc::Vector2,IInspectable**)=0;
};
struct DesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND,BOOL,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD)=0;
};
struct SurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(const RECT*,REFIID,void**,POINT*)=0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw()=0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE)=0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(const RECT*,const RECT*,int,int)=0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw()=0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw()=0;
};
template<class T> HRESULT query(IUnknown* from,const GUID& iid,ComPtr<T>& to){return from?from->QueryInterface(iid,reinterpret_cast<void**>(to.put())):E_POINTER;}
// Records the failing step for diagnostics (dock-error.txt, --glass-probe).
const char* failed_step="";
void step(const char* name,HRESULT hr){if(FAILED(hr)){failed_step=name;throw hr;}}
// The compositor is thread-affine and needs a DispatcherQueue on that thread.
// Both live for the process; every popup on the GUI thread shares them.
struct Shared {
    ComPtr<IUnknown> queue;
    ComPtr<wuc::ICompositor> compositor;
    ComPtr<Compositor3> compositor3;
    ComPtr<Compositor7> compositor7;
    ComPtr<DesktopInterop> desktop;
    ComPtr<wuc::ICompositorInterop> interop;
    DWORD thread=0;
    HRESULT status=E_PENDING;
};
Shared& shared() {
    // Intentionally never destroyed: static destructors run after wWinMain's
    // CoUninitialize, when releasing WinRT objects could call unloaded code.
    static Shared& value=*new Shared;
    if(value.status!=E_PENDING)return value;
    value.thread=GetCurrentThreadId();
    try {
        const DispatcherQueueOptionsAbi options{sizeof(DispatcherQueueOptionsAbi),2/*DQTYPE_THREAD_CURRENT*/,2/*DQTAT_COM_STA*/};
        step("dispatcher queue",CreateDispatcherQueueController(options,value.queue.put()));
        HSTRING name=nullptr;constexpr wchar_t runtime[]=L"Windows.UI.Composition.Compositor";
        step("compositor class name",WindowsCreateString(runtime,static_cast<UINT32>(std::size(runtime)-1),&name));
        ComPtr<IInspectable> instance;const auto hr=RoActivateInstance(name,instance.put());WindowsDeleteString(name);step("compositor activation",hr);
        step("ICompositor",query(instance.get(),iid_compositor,value.compositor));
        step("ICompositor3",query(instance.get(),iid_compositor3,value.compositor3));
        step("ICompositor7",query(instance.get(),iid_compositor7,value.compositor7));
        step("desktop interop",query(instance.get(),iid_desktop_interop,value.desktop));
        step("compositor interop",query(instance.get(),iid_compositor_interop,value.interop));
        value.status=S_OK;
    } catch(HRESULT error) {value.status=error;}
    return value;
}
ComPtr<ID3D11Device> create_d3d() {
    ComPtr<ID3D11Device> device;
    for(const auto type:{D3D_DRIVER_TYPE_HARDWARE,D3D_DRIVER_TYPE_WARP})
        if(SUCCEEDED(D3D11CreateDevice(nullptr,type,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,nullptr)))return device;
    step("Direct3D device",E_FAIL);return device;
}
}
struct GlassHost::Impl {
    HWND window=nullptr;
    ComPtr<ID2D1Device> d2d;
    ComPtr<wuc::ICompositionGraphicsDevice> graphics;
    ComPtr<IInspectable> target,host_brush;
    ComPtr<wuc::IContainerVisual> root,backdrops;
    ComPtr<wuc::ISpriteVisual> content;
    ComPtr<wuc::ICompositionDrawingSurface> surface;
    ComPtr<SurfaceInterop> draw;
    ComPtr<wuc::ICompositionSurfaceBrush> content_brush;
    // One clipped backdrop sprite per glass shape, reused between frames.
    struct Pane {ComPtr<wuc::ISpriteVisual> sprite;D2D1_RECT_F bounds{};float radius=-1,opacity=-1;bool visible=false;};
    std::vector<Pane> panes;
    UINT width=0,height=0,allocations=0;
    float last_opacity=-1,last_offset=-1;int last_backdrop=-1;
    bool drawing=false;
    ComPtr<ID2D1DeviceContext> session;
    static ComPtr<wuc::IVisual> visual(IUnknown* sprite){ComPtr<wuc::IVisual> result;step("IVisual",query(sprite,iid_visual,result));return result;}
    static ComPtr<wuc::ICompositionBrush> brush(IUnknown* value){ComPtr<wuc::ICompositionBrush> result;step("ICompositionBrush",query(value,iid_brush,result));return result;}
    void surfaces(UINT w,UINT h) {
        if(surface&&w<=width&&h<=height)return;
        width=std::max(w,width);height=std::max(h,height);++allocations;
        constexpr INT32 format=87; // DirectXPixelFormat B8G8R8A8UIntNormalized
        constexpr INT32 alpha=1;   // DirectXAlphaMode Premultiplied
        step("drawing surface",graphics->CreateDrawingSurface(wuc::Size{static_cast<FLOAT>(width),static_cast<FLOAT>(height)},format,alpha,surface.put()));
        step("drawing surface interop",query(surface.get(),iid_surface_interop,draw));
        auto& s=shared();ComPtr<wuc::ICompositionSurface> view;step("ICompositionSurface",query(surface.get(),iid_surface,view));
        step("surface brush",s.compositor->CreateSurfaceBrushWithSurface(view.get(),content_brush.put()));
        // Unscaled, anchored top-left: the surface can be larger than the window.
        step("surface brush stretch",content_brush->put_Stretch(0/*CompositionStretch None*/));
        step("surface brush alignment",content_brush->put_HorizontalAlignmentRatio(0));
        step("surface brush alignment",content_brush->put_VerticalAlignmentRatio(0));
        step("content brush",content->put_Brush(brush(content_brush.get()).get()));
    }
    Pane& pane(size_t index) {
        while(panes.size()<=index) {
            auto& s=shared();Pane value;
            step("backdrop sprite",s.compositor->CreateSpriteVisual(value.sprite.put()));
            step("backdrop brush",value.sprite->put_Brush(brush(host_brush.get()).get()));
            ComPtr<wuc::IVisualCollection> children;step("backdrop children",backdrops->get_Children(children.put()));
            step("backdrop insert",children->InsertAtTop(visual(value.sprite.get()).get()));
            panes.push_back(std::move(value));
        }
        return panes[index];
    }
};
GlassHost::GlassHost():impl_(std::make_unique<Impl>()){}
GlassHost::~GlassHost(){detach();}
const char* GlassHost::failure(){return failed_step;}
bool GlassHost::available() {
    // Explicit opt-out for comparison and fallback testing.
    wchar_t mode[16]{};
    if(GetEnvironmentVariableW(L"MINIMADOCK_RENDERER",mode,16)&&_wcsicmp(mode,L"layered")==0)return false;
    auto& s=shared();
    return SUCCEEDED(s.status)&&s.thread==GetCurrentThreadId();
}
void GlassHost::attach(HWND window,ID2D1Factory* factory,bool above_content) {
    detach();
    if(!available())step("composition unavailable",E_NOTIMPL);
    auto& s=shared();auto& i=*impl_;
    // Lets a Win32 window sample the system's blurred host backdrop.
    const BOOL use_backdrop=TRUE;DwmSetWindowAttribute(window,17/*DWMWA_USE_HOSTBACKDROPBRUSH*/,&use_backdrop,sizeof(use_backdrop));
    ComPtr<ID2D1Factory1> factory1;step("ID2D1Factory1",factory->QueryInterface(__uuidof(ID2D1Factory1),reinterpret_cast<void**>(factory1.put())));
    const auto d3d=create_d3d();ComPtr<IDXGIDevice> dxgi;step("IDXGIDevice",d3d->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(dxgi.put())));
    step("Direct2D device",factory1->CreateDevice(dxgi.get(),i.d2d.put()));
    step("composition graphics device",s.interop->CreateGraphicsDevice(i.d2d.get(),i.graphics.put()));
    step("desktop window target",s.desktop->CreateDesktopWindowTarget(window,above_content?TRUE:FALSE,i.target.put()));
    step("root visual",s.compositor->CreateContainerVisual(i.root.put()));
    step("backdrop container",s.compositor->CreateContainerVisual(i.backdrops.put()));
    step("content sprite",s.compositor->CreateSpriteVisual(i.content.put()));
    step("host backdrop brush",s.compositor3->CreateHostBackdropBrush(i.host_brush.put()));
    ComPtr<wuc::IVisualCollection> children;step("root children",i.root->get_Children(children.put()));
    step("insert backdrops",children->InsertAtTop(Impl::visual(i.backdrops.get()).get()));
    step("insert content",children->InsertAtTop(Impl::visual(i.content.get()).get()));
    ComPtr<wuc::ICompositionTarget> target;step("ICompositionTarget",query(i.target.get(),iid_target,target));
    step("target root",target->put_Root(Impl::visual(i.root.get()).get()));
    i.window=window;
}
void GlassHost::detach() {
    auto& i=*impl_;
    if(i.drawing){i.session.reset();if(i.draw)i.draw->EndDraw();i.drawing=false;}
    if(i.target){ComPtr<wuc::ICompositionTarget> target;if(SUCCEEDED(query(i.target.get(),iid_target,target)))target->put_Root(nullptr);}
    i.panes.clear();i.draw.reset();i.content_brush.reset();i.surface.reset();i.host_brush.reset();
    i.content.reset();i.backdrops.reset();i.root.reset();i.target.reset();i.graphics.reset();i.d2d.reset();
    i.window=nullptr;i.width=i.height=0;i.last_opacity=i.last_offset=-1;i.last_backdrop=-1;
}
bool GlassHost::attached() const{return impl_->window!=nullptr;}
unsigned GlassHost::allocations() const{return impl_->allocations;}
ID2D1DeviceContext* GlassHost::begin(UINT width,UINT height,float dpi,D2D1_MATRIX_3X2_F& origin) {
    auto& i=*impl_;if(!i.window)step("draw without window",E_UNEXPECTED);
    i.surfaces(width,height);
    step("content size",Impl::visual(i.content.get())->put_Size(wuc::Vector2{static_cast<FLOAT>(width),static_cast<FLOAT>(height)}));
    POINT offset{};
    step("begin draw",i.draw->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),reinterpret_cast<void**>(i.session.put()),&offset));
    i.drawing=true;
    // The drawing session owns BeginDraw; the context arrives ready. Surfaces
    // keep old pixels, so clear the whole update area first.
    i.session->SetDpi(dpi,dpi);
    origin=D2D1::Matrix3x2F::Translation(offset.x*96.0f/dpi,offset.y*96.0f/dpi);
    i.session->SetTransform(origin);
    i.session->Clear(D2D1::ColorF(0,0,0,0));
    return i.session.get();
}
void GlassHost::end() {
    auto& i=*impl_;if(!i.drawing)return;
    i.drawing=false;i.session.reset();step("end draw",i.draw->EndDraw());
}
void GlassHost::backdrops(const std::vector<GlassShape>& shapes,float dpi) {
    auto& i=*impl_;if(!i.window)return;
    auto& s=shared();const float scale=dpi/96;
    size_t used=0;
    for(const auto& shape:shapes) {
        if(shape.opacity<=0)continue;
        // Shapes use uniform scale + translation, so map the rectangle directly.
        const auto& m=shape.transform;const float zoom=std::sqrt(std::abs(m._11*m._22-m._12*m._21));
        const auto bounds=D2D1::RectF((shape.rect.left*m._11+shape.rect.top*m._21+m._31)*scale,(shape.rect.left*m._12+shape.rect.top*m._22+m._32)*scale,
            (shape.rect.right*m._11+shape.rect.bottom*m._21+m._31)*scale,(shape.rect.right*m._12+shape.rect.bottom*m._22+m._32)*scale);
        // A circular clip slightly larger than a continuous corner stays inside it.
        const float radius=std::min(shape.radius*(shape.smooth?1.15f:1.0f)*zoom*scale,std::min(bounds.right-bounds.left,bounds.bottom-bounds.top)/2);
        auto& pane=i.pane(used++);
        const auto sprite=Impl::visual(pane.sprite.get());
        const bool moved=bounds.left!=pane.bounds.left||bounds.top!=pane.bounds.top||bounds.right!=pane.bounds.right||bounds.bottom!=pane.bounds.bottom;
        if(moved) {
            step("backdrop offset",sprite->put_Offset(wuc::Vector3{bounds.left,bounds.top,0}));
            step("backdrop size",sprite->put_Size(wuc::Vector2{bounds.right-bounds.left,bounds.bottom-bounds.top}));
        }
        if(moved||radius!=pane.radius) {
            const wuc::Vector2 corner{radius,radius};ComPtr<IInspectable> clip;
            step("backdrop clip",s.compositor7->CreateRectangleClipWithSidesAndRadius(0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,corner,corner,corner,corner,clip.put()));
            ComPtr<wuc::ICompositionClip> view;step("ICompositionClip",query(clip.get(),iid_clip,view));
            step("backdrop set clip",sprite->put_Clip(view.get()));
        }
        if(shape.opacity!=pane.opacity)step("backdrop opacity",sprite->put_Opacity(std::clamp(shape.opacity,0.0f,1.0f)));
        if(!pane.visible)step("backdrop show",sprite->put_IsVisible(true));
        pane.bounds=bounds;pane.radius=radius;pane.opacity=shape.opacity;pane.visible=true;
    }
    for(size_t index=used;index<i.panes.size();++index)if(i.panes[index].visible) {
        step("backdrop hide",Impl::visual(i.panes[index].sprite.get())->put_IsVisible(false));i.panes[index].visible=false;
    }
}
void GlassHost::present(float opacity,float offset_pixels,bool backdrop) {
    auto& i=*impl_;if(!i.window)return;
    opacity=std::clamp(opacity,0.0f,1.0f);
    if(opacity!=i.last_opacity||offset_pixels!=i.last_offset) {
        const auto root=Impl::visual(i.root.get());
        step("root opacity",root->put_Opacity(opacity));
        step("root offset",root->put_Offset(wuc::Vector3{0,std::round(offset_pixels),0}));
        i.last_opacity=opacity;i.last_offset=offset_pixels;
    }
    if(static_cast<int>(backdrop)!=i.last_backdrop) {
        step("backdrop visibility",Impl::visual(i.backdrops.get())->put_IsVisible(backdrop));
        i.last_backdrop=backdrop;
    }
}
// Diagnostic: a non-activating popup with a glass capsule (left) and a
// content-only tint (right), to verify backdrop sampling on this system.
// Writes "ok" or the failing step and HRESULT to `report` when given.
int run_glass_probe(HINSTANCE instance,unsigned seconds,const wchar_t* report) {
    HRESULT result=S_OK;
    try {
        if(!GlassHost::available()){failed_step=failed_step[0]?failed_step:"composition unavailable";throw E_NOTIMPL;}
        ComPtr<ID2D1Factory> factory;step("Direct2D factory",D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
        WNDCLASSW klass{};klass.lpfnWndProc=DefWindowProcW;klass.hInstance=instance;klass.lpszClassName=L"MinimaDock.GlassProbe";
        RegisterClassW(&klass);
        MONITORINFO info{sizeof(info),{},{},0};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&info);
        const UINT w=640,h=220;const int x=(info.rcWork.left+info.rcWork.right-static_cast<int>(w))/2,y=info.rcWork.bottom-static_cast<int>(h)-40;
        const auto window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,klass.lpszClassName,L"MinimaDock glass probe",WS_POPUP,x,y,w,h,nullptr,nullptr,instance,nullptr);
        if(!window)step("probe window",HRESULT_FROM_WIN32(GetLastError()));
        GlassHost glass;glass.attach(window,factory.get());
        const float dpi=96;D2D1_MATRIX_3X2_F origin{};
        auto context=glass.begin(w,h,dpi,origin);
        ComPtr<ID2D1SolidColorBrush> brush;step("probe brush",context->CreateSolidColorBrush(D2D1::ColorF(0x101014,.42f),brush.put()));
        const auto left=D2D1::RectF(20,70,300,150),right=D2D1::RectF(340,70,620,150);
        context->FillRoundedRectangle(D2D1::RoundedRect(left,40,40),brush.get());context->FillRoundedRectangle(D2D1::RoundedRect(right,40,40),brush.get());
        brush->SetColor(D2D1::ColorF(1,1,1,.5f));context->DrawRoundedRectangle(D2D1::RoundedRect(left,40,40),brush.get(),1);context->DrawRoundedRectangle(D2D1::RoundedRect(right,40,40),brush.get(),1);
        brush.reset();glass.end();
        glass.backdrops({GlassShape{D2D1::Matrix3x2F::Identity(),left,40,false,1}},dpi);glass.present(1,0,true);
        ShowWindow(window,SW_SHOWNOACTIVATE);
        SetTimer(window,1,seconds*1000,nullptr);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(message.message==WM_TIMER)break;DispatchMessageW(&message);}
        glass.detach();DestroyWindow(window);
    } catch(HRESULT error){result=error;}
    if(report&&report[0]) {
        if(const auto file=_wfopen(report,L"w")) {
            if(SUCCEEDED(result))fputs("ok",file);else fprintf(file,"%s 0x%08lX",failed_step,static_cast<unsigned long>(result));
            fclose(file);
        }
    }
    return SUCCEEDED(result)?0:static_cast<int>(result);
}
}
