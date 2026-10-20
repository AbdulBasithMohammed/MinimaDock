#include "rendering/previews.h"
#include "rendering/glass.h"
#include "rendering/glass_style.h"
#include "taskbar/state_machine.h"
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace minima {
namespace {
constexpr UINT_PTR motion_timer=1;
constexpr size_t max_tiles=6;
// Layout in DIPs before the fit scale; margins leave room for the shadow.
constexpr float tile_width=212,title_height=30,thumb_height=122,pad=12,gap=8,side=26,top_margin=20,bottom_margin=34;
void check(HRESULT hr){if(FAILED(hr))throw hr;}
bool inside(D2D1_RECT_F r,float x,float y){return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;}
}
struct WindowPreviews::Impl {
    struct Member {HWND window=nullptr;RECT frame{};HTHUMBNAIL thumbnail=nullptr;SIZE source{};bool iconic=false;std::wstring title;};
    // A window tile, or a snap group tile (`members` set; `window` is this app's member).
    struct Item {HWND window=nullptr;std::wstring title;HTHUMBNAIL thumbnail=nullptr;SIZE source{};bool iconic=false;std::vector<Member> members;RECT area{};};
    HWND hwnd=nullptr,owner=nullptr;
    GlassHost host;
    GlassPainter painter;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> title_font,glyph_font,note_font;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> small_icon,large_icon;
    NativeIcon icon;
    std::vector<Item> items;
    std::wstring key,name;
    theme::Palette palette=theme::palette(false);
    PanelReveal reveal;
    float dpi=96,fit=1;
    UINT width=0,height=0;
    POINT position{};
    int hover=-1;
    bool hover_close=false,live=false,reduced=false,bound=false,closing=false,timer=false,tracking=false;

    static std::int64_t now(){return static_cast<std::int64_t>(GetTickCount64());}
    void initialize() {
        if(factory)return;
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IDWriteFontCollection> collection;check(write->GetSystemFontCollection(collection.put()));
        const auto family=theme::text_family(collection.get());
        const auto format=[&](const wchar_t* face,float size,ComPtr<IDWriteTextFormat>& out) {
            check(write->CreateTextFormat(face,nullptr,DWRITE_FONT_WEIGHT_LIGHT,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"",out.put()));
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};ComPtr<IDWriteInlineObject> ellipsis;
            write->CreateEllipsisTrimmingSign(out.get(),ellipsis.put());out->SetTrimming(&trimming,ellipsis.get());
        };
        format(family,12.5f,title_font);format(family,12,note_font);format(L"Segoe Fluent Icons",10,glyph_font);
        note_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);glyph_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Card geometry (DIPs, window space).
    float card_width() const{const float n=static_cast<float>(std::max<size_t>(1,items.size()));return (n*tile_width+(n-1)*gap+2*pad)*fit;}
    float card_height() const{return (pad*2+title_height+thumb_height)*fit;}
    D2D1_RECT_F card() const{return D2D1::RectF(side,top_margin,side+card_width(),top_margin+card_height());}
    D2D1_RECT_F tile(size_t i) const{const auto c=card();const float x=c.left+(pad+static_cast<float>(i)*(tile_width+gap))*fit,y=c.top+pad*fit;return D2D1::RectF(x,y,x+tile_width*fit,y+(title_height+thumb_height)*fit);}
    D2D1_RECT_F close_rect(size_t i) const{const auto t=tile(i);return D2D1::RectF(t.right-24*fit,t.top+4*fit,t.right-2*fit,t.top+26*fit);}
    D2D1_RECT_F thumb_area(size_t i) const{const auto t=tile(i);return D2D1::RectF(t.left,t.top+title_height*fit,t.right,t.bottom);}
    D2D1_POINT_2F pivot() const{const auto c=card();return D2D1::Point2F((c.left+c.right)/2,c.bottom);}
    // Maps a pointer position through the reveal scale (as the panels do).
    D2D1_POINT_2F content(LPARAM l) const {
        const auto p=pivot();const float s=reveal.scale();
        return D2D1::Point2F(p.x+(GET_X_LPARAM(l)*96.0f/dpi-p.x)/s,p.y+(GET_Y_LPARAM(l)*96.0f/dpi-reveal.offset()-p.y)/s);
    }
    ComPtr<ID2D1Bitmap> bitmap(ID2D1RenderTarget* target,float dip) {
        ComPtr<ID2D1Bitmap> result;if(!icon)return result;
        ComPtr<IWICBitmap> source;ComPtr<IWICBitmapScaler> scaled;ComPtr<IWICFormatConverter> converted;
        const auto pixels=static_cast<UINT>(std::max(1.0f,std::round(dip*dpi/96)));
        if(FAILED(wic->CreateBitmapFromHICON(static_cast<HICON>(icon.get()),source.put()))||FAILED(wic->CreateBitmapScaler(scaled.put()))||
           FAILED(scaled->Initialize(source.get(),pixels,pixels,WICBitmapInterpolationModeFant))||FAILED(wic->CreateFormatConverter(converted.put()))||
           FAILED(converted->Initialize(scaled.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return result;
        target->CreateBitmapFromWicBitmap(converted.get(),nullptr,result.put());return result;
    }
    void unregister() {
        for(auto& item:items) {
            if(item.thumbnail){DwmUnregisterThumbnail(item.thumbnail);item.thumbnail=nullptr;}
            for(auto& member:item.members)if(member.thumbnail){DwmUnregisterThumbnail(member.thumbnail);member.thumbnail=nullptr;}
        }
    }
    void rebuild(const std::vector<HWND>& windows,const std::vector<SnapGroup>& groups) {
        unregister();items.clear();
        const auto title_of=[&](HWND window){wchar_t title[256]{};GetWindowTextW(window,title,256);return std::wstring(title[0]?title:name.c_str());};
        for(const auto window:windows) {
            if(items.size()>=max_tiles)break;
            if(!IsWindow(window))continue;
            Item item;item.window=window;item.title=title_of(window);
            items.push_back(std::move(item));
        }
        for(const auto& group:groups) {
            if(items.size()>=max_tiles)break;
            Item item;item.title=group.label;item.area=group.area;
            for(size_t i=0;i<group.windows.size()&&i<group.frames.size();++i) {
                if(!IsWindow(group.windows[i]))continue;
                if(!item.window&&std::find(windows.begin(),windows.end(),group.windows[i])!=windows.end())item.window=group.windows[i];
                Member member;member.window=group.windows[i];member.frame=group.frames[i];member.title=title_of(group.windows[i]);
                item.members.push_back(std::move(member));
            }
            if(item.members.size()<2)continue;
            if(!item.window)item.window=item.members.front().window;
            items.push_back(std::move(item));
        }
    }
    void register_one(HWND window,HTHUMBNAIL& thumbnail,SIZE& source,bool& iconic) {
        iconic=IsIconic(window)!=FALSE;
        if(thumbnail||iconic)return;
        if(SUCCEEDED(DwmRegisterThumbnail(hwnd,window,&thumbnail))&&FAILED(DwmQueryThumbnailSourceSize(thumbnail,&source))){DwmUnregisterThumbnail(thumbnail);thumbnail=nullptr;}
    }
    void register_thumbnails() {
        for(auto& item:items) {
            if(item.members.empty()){register_one(item.window,item.thumbnail,item.source,item.iconic);continue;}
            for(auto& member:item.members)register_one(member.window,member.thumbnail,member.source,member.iconic);
        }
    }
    // Where each snapped window sits inside a group tile's frame: the shared
    // work area aspect-fitted into the frame, each window at its own place.
    D2D1_RECT_F member_rect(const Item& item,size_t m,D2D1_RECT_F area) const {
        const auto& a=item.area;const auto& f=item.members[m].frame;
        const float aw=static_cast<float>(std::max<LONG>(1,a.right-a.left)),ah=static_cast<float>(std::max<LONG>(1,a.bottom-a.top));
        const float zoom=std::min((area.right-area.left)/aw,(area.bottom-area.top)/ah);
        const float x0=(area.left+area.right)/2-aw*zoom/2,y0=(area.top+area.bottom)/2-ah*zoom/2;
        const float gap_dip=1.5f*fit;
        return D2D1::RectF(x0+(f.left-a.left)*zoom+gap_dip,y0+(f.top-a.top)*zoom+gap_dip,x0+(f.right-a.left)*zoom-gap_dip,y0+(f.bottom-a.top)*zoom-gap_dip);
    }
    void place(RECT anchor,LONG dock_top) {
        MONITORINFO info{sizeof(info),{},{},0};GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&info);
        const float scale=dpi/96,room=(info.rcMonitor.right-info.rcMonitor.left)/scale-2*side-24;
        fit=1;const float natural=card_width();if(natural>room)fit=std::max(.6f,room/natural);
        width=static_cast<UINT>(std::ceil((card_width()+2*side)*scale));height=static_cast<UINT>(std::ceil((top_margin+card_height()+bottom_margin)*scale));
        // Card bottom sits 12 DIP above the pill, centred on the icon.
        const LONG centre=(anchor.left+anchor.right)/2,card_bottom=dock_top-static_cast<LONG>(std::lround(12*scale));
        position.x=std::clamp<LONG>(centre-static_cast<LONG>(width)/2,info.rcMonitor.left+8,std::max(info.rcMonitor.left+8,info.rcMonitor.right-8-static_cast<LONG>(width)));
        position.y=card_bottom-static_cast<LONG>(std::lround((top_margin+card_height())*scale));
        SetWindowPos(hwnd,HWND_TOPMOST,position.x,position.y,static_cast<int>(width),static_cast<int>(height),SWP_NOACTIVATE);
        // Hit testing only on the card (plus a small shadow margin).
        const auto c=card();
        if(auto region=CreateRoundRectRgn(static_cast<int>((c.left-6)*scale),static_cast<int>((c.top-6)*scale),static_cast<int>((c.right+6)*scale)+1,static_cast<int>((c.bottom+10)*scale)+1,static_cast<int>(44*scale),static_cast<int>(44*scale)))
            if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
    }
    void schedule() {
        const bool moving=reveal.active();
        if(moving&&!timer)timer=SetTimer(hwnd,motion_timer,10,nullptr)!=0;
        else if(!moving&&timer){KillTimer(hwnd,motion_timer);timer=false;}
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void color(D2D1_COLOR_F c){brush->SetColor(c);}
    void render() {
        if(!hwnd)return;
        if(!host.attached()) {
            // Composition below the window content: DWM thumbnails stay on top.
            host.attach(hwnd,factory.get(),false);bound=false;
        }
        D2D1_MATRIX_3X2_F origin{};auto dc=host.begin(width,height,dpi,origin);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        painter.palette=palette;painter.live=live;
        if(!bound) {
            painter.bind(dc,factory.get());brush.reset();check(dc->CreateSolidColorBrush(palette.ink,brush.put()));
            small_icon=bitmap(dc,16);large_icon=bitmap(dc,40);bound=true;
        } else painter.retarget(dc);
        painter.origin=origin;painter.begin_frame();
        const auto p=pivot();const float s=reveal.scale();
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s,s,p)*(*D2D1::Matrix3x2F::ReinterpretBaseType(&origin)));
        painter.paint(card(),20,GlassKind::Panel);
        for(size_t i=0;i<items.size();++i) {
            const auto t=tile(i),thumb=thumb_area(i);
            if(static_cast<int>(i)==hover){color(palette.hover);dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left-4*fit,t.top-4*fit,t.right+4*fit,t.bottom+4*fit),12*fit,12*fit),brush.get());}
            if(!items[i].members.empty()){color(palette.ink);dc->DrawTextW(L"\xE8A9",1,glyph_font.get(),D2D1::RectF(t.left+4*fit,t.top+7*fit,t.left+20*fit,t.top+23*fit),brush.get());} // Tiles glyph: a group.
            else if(small_icon)dc->DrawBitmap(small_icon.get(),D2D1::RectF(t.left+4*fit,t.top+7*fit,t.left+20*fit,t.top+23*fit));
            const bool group=!items[i].members.empty();
            const float title_right=static_cast<int>(i)==hover&&!group?close_rect(i).left-2:t.right-4*fit;
            color(palette.ink);title_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            dc->DrawTextW(items[i].title.c_str(),static_cast<UINT32>(items[i].title.size()),title_font.get(),D2D1::RectF(t.left+26*fit,t.top,title_right,t.top+title_height*fit),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if(static_cast<int>(i)==hover&&!group) {
                const auto c=close_rect(i);
                if(hover_close){color(theme::rgb(0xc42b1c,.92f));dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((c.left+c.right)/2,(c.top+c.bottom)/2),10*fit,10*fit),brush.get());}
                color(hover_close?theme::rgb(0xffffff):palette.ink);
                dc->DrawTextW(L"\xE8BB",1,glyph_font.get(),c,brush.get());
            }
            // Thumbnail frame; DWM draws the live image inside it. Minimized
            // windows have no live image, so show the app icon instead.
            color(palette.field);dc->FillRoundedRectangle(D2D1::RoundedRect(thumb,8*fit,8*fit),brush.get());
            if(group) { // Each snapped window in its place; DWM draws the live ones on top.
                const auto inner=D2D1::RectF(thumb.left+4*fit,thumb.top+4*fit,thumb.right-4*fit,thumb.bottom-4*fit);
                for(size_t m=0;m<items[i].members.size();++m) {
                    const auto r=member_rect(items[i],m,inner);const auto& member=items[i].members[m];
                    color(palette.tile);dc->FillRoundedRectangle(D2D1::RoundedRect(r,4*fit,4*fit),brush.get());
                    if(member.iconic||!member.thumbnail) {
                        color(palette.ink_secondary);
                        dc->DrawTextW(member.title.c_str(),static_cast<UINT32>(member.title.size()),note_font.get(),D2D1::RectF(r.left+4*fit,r.top,r.right-4*fit,r.bottom),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                continue;
            }
            if(items[i].iconic||!items[i].thumbnail) {
                const float cx=(thumb.left+thumb.right)/2,cy=(thumb.top+thumb.bottom)/2-8*fit;
                if(large_icon)dc->DrawBitmap(large_icon.get(),D2D1::RectF(cx-20*fit,cy-20*fit,cx+20*fit,cy+20*fit));
                color(palette.ink_secondary);
                const wchar_t* note=items[i].iconic?L"Minimized":L"Preview unavailable";
                dc->DrawTextW(note,static_cast<UINT32>(wcslen(note)),note_font.get(),D2D1::RectF(thumb.left,cy+24*fit,thumb.right,cy+42*fit),brush.get());
            }
        }
        host.end();
        host.backdrops(painter.shapes,dpi);
        host.present(reveal.opacity(),reveal.offset()*dpi/96,live);
        update_thumbnails();
    }
    void update_thumbnails() {
        const float scale=dpi/96,s=reveal.scale();const auto p=pivot();
        const BYTE opacity=static_cast<BYTE>(std::lround(std::clamp(reveal.opacity(),0.0f,1.0f)*255));
        const auto map=[&](float x,float y){return D2D1::Point2F((p.x+(x-p.x)*s)*scale,(p.y+(y-p.y)*s+reveal.offset())*scale);};
        // Aspect-fits a source inside `area` and places its live image there.
        const auto show=[&](HTHUMBNAIL thumbnail,SIZE source,D2D1_RECT_F area) {
            const float sw=static_cast<float>(std::max<LONG>(1,source.cx)),sh=static_cast<float>(std::max<LONG>(1,source.cy));
            const float zoom=std::min((area.right-area.left)/sw,(area.bottom-area.top)/sh);
            const float w=sw*zoom,h=sh*zoom,cx=(area.left+area.right)/2,cy=(area.top+area.bottom)/2;
            const auto a=map(cx-w/2,cy-h/2),b=map(cx+w/2,cy+h/2);
            DWM_THUMBNAIL_PROPERTIES properties{};
            properties.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY|DWM_TNP_SOURCECLIENTAREAONLY;
            properties.rcDestination={static_cast<LONG>(std::lround(a.x)),static_cast<LONG>(std::lround(a.y)),static_cast<LONG>(std::lround(b.x)),static_cast<LONG>(std::lround(b.y))};
            properties.fVisible=TRUE;properties.opacity=opacity;properties.fSourceClientAreaOnly=FALSE;
            DwmUpdateThumbnailProperties(thumbnail,&properties);
        };
        for(size_t i=0;i<items.size();++i) {
            auto& item=items[i];auto area=thumb_area(i);
            if(!item.members.empty()) {
                const auto inner=D2D1::RectF(area.left+4*fit,area.top+4*fit,area.right-4*fit,area.bottom-4*fit);
                for(size_t m=0;m<item.members.size();++m)if(item.members[m].thumbnail)show(item.members[m].thumbnail,item.members[m].source,member_rect(item,m,inner));
                continue;
            }
            if(!item.thumbnail)continue;
            show(item.thumbnail,item.source,D2D1::RectF(area.left+3*fit,area.top+3*fit,area.right-3*fit,area.bottom-3*fit));
        }
    }
    void destroy() {
        unregister();items.clear();
        if(hwnd){const auto window=hwnd;host.detach();bound=false;hwnd=nullptr;DestroyWindow(window);}
        closing=false;timer=false;tracking=false;hover=-1;key.clear();
        if(IsWindow(owner))PostMessageW(owner,previews_event_message,0,0);
    }
    void hit(LPARAM l) {
        const auto point=content(l);int over=-1;bool close=false;
        for(size_t i=0;i<items.size();++i){const auto t=tile(i);if(inside(D2D1::RectF(t.left-4*fit,t.top-4*fit,t.right+4*fit,t.bottom+4*fit),point.x,point.y)){over=static_cast<int>(i);close=items[i].members.empty()&&inside(close_rect(i),point.x,point.y);break;}}
        if(over!=hover||close!=hover_close){hover=over;hover_close=close;schedule();}
    }
    void remove(size_t index) {
        if(index>=items.size())return;
        if(items[index].thumbnail)DwmUnregisterThumbnail(items[index].thumbnail);
        for(auto& member:items[index].members)if(member.thumbnail)DwmUnregisterThumbnail(member.thumbnail);
        items.erase(items.begin()+static_cast<std::ptrdiff_t>(index));hover=-1;
        if(items.empty()){close(true);return;}
        RECT current{};GetWindowRect(hwnd,&current);
        // Keep the card centred where it was while it shrinks.
        const float scale=dpi/96;const LONG centre=(current.left+current.right)/2,card_bottom=current.top+static_cast<LONG>(std::lround((top_margin+card_height())*scale));
        place(RECT{centre-1,card_bottom,centre+1,card_bottom+1},card_bottom+static_cast<LONG>(std::lround(12*scale)));
        schedule();
    }
    void close(bool animated) {
        if(!hwnd)return;
        if(animated&&!reduced){if(!closing){closing=true;reveal.close(now());schedule();}return;}
        destroy();
    }
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(message==WM_NCDESTROY){SetWindowLongPtrW(window,GWLP_USERDATA,0);return DefWindowProcW(window,message,w,l);}
        if(!self)return DefWindowProcW(window,message,w,l);
        try {return self->handle(window,message,w,l);}catch(...){PostMessageW(window,WM_CLOSE,0,0);return 0;}
    }
    LRESULT handle(HWND window,UINT message,WPARAM w,LPARAM l) {
        switch(message) {
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);try{if(window==hwnd)render();}catch(...){EndPaint(window,&ps);throw;}EndPaint(window,&ps);return 0;}
        case WM_TIMER:
            if(w==motion_timer){reveal.tick(now());if(closing&&reveal.hidden()){destroy();return 0;}schedule();}
            return 0;
        case WM_MOUSEMOVE:
            if(!tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window,0};TrackMouseEvent(&t);tracking=true;}
            if(!closing)hit(l);return 0;
        case WM_MOUSELEAVE:tracking=false;if(hover!=-1){hover=-1;hover_close=false;schedule();}return 0;
        case WM_LBUTTONUP:case WM_MBUTTONUP: {
            if(closing)return 0;hit(l);
            if(hover<0)return 0;
            const auto index=static_cast<size_t>(hover);const auto target=items[index].window;
            if(!items[index].members.empty()) {
                if(message==WM_MBUTTONUP)return 0; // A group has no single window to close.
                // Restore the whole group (snapped windows return to their places),
                // then bring each forward, ending with this app's window.
                for(const auto& member:items[index].members)if(IsWindow(member.window)&&IsIconic(member.window))ShowWindow(member.window,SW_RESTORE);
                for(const auto& member:items[index].members)if(member.window!=target&&IsWindow(member.window))activate_window(member.window);
            } else if(message==WM_MBUTTONUP||hover_close){PostMessageW(target,WM_CLOSE,0,0);remove(index);return 0;}
            activate_window(target);
            if(IsWindow(owner))PostMessageW(owner,previews_event_message,1,reinterpret_cast<LPARAM>(target));
            close(true);return 0;
        }
        case WM_CLOSE:close(true);return 0;
        case WM_DESTROY:KillTimer(window,motion_timer);timer=false;return 0;
        default:return DefWindowProcW(window,message,w,l);
        }
    }
};
WindowPreviews::WindowPreviews():impl_(std::make_unique<Impl>()){}
WindowPreviews::~WindowPreviews(){impl_->close(false);}
void WindowPreviews::open(HWND owner,const std::wstring& key,const std::wstring& name,const std::vector<HWND>& windows,const std::vector<SnapGroup>& groups,NativeIcon icon,
                          RECT anchor,LONG dock_top,UINT dpi,const theme::Palette& palette,bool live,bool reduced) {
    auto& i=*impl_;
    if(windows.empty()||!GlassHost::available()){close(false);return;}
    i.initialize();
    const bool same=i.hwnd&&i.key==key;
    i.owner=owner;i.name=name;i.palette=palette;i.live=live;i.reduced=reduced;i.dpi=static_cast<float>(dpi);
    if(i.icon.get()!=icon.get()){i.icon=std::move(icon);i.bound=false;i.host.detach();}
    if(!i.hwnd) {
        WNDCLASSW klass{};klass.lpfnWndProc=Impl::proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.WindowPreviews";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw E_FAIL;
        const auto created=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,klass.lpszClassName,L"MinimaDock window previews",WS_POPUP,0,0,1,1,owner,nullptr,klass.hInstance,&i);
        if(!created){i.hwnd=nullptr;throw HRESULT_FROM_WIN32(GetLastError());}
        i.hwnd=created;
        i.reveal.start(Impl::now(),reduced);
    } else if(i.closing||!same) {
        i.closing=false;i.reveal.open(Impl::now(),reduced);
    }
    i.key=key;i.hover=-1;i.hover_close=false;
    i.rebuild(windows,groups);
    i.place(anchor,dock_top);
    i.register_thumbnails();
    i.render();
    ShowWindow(i.hwnd,SW_SHOWNOACTIVATE);
    i.schedule();
}
void WindowPreviews::close(bool animated){impl_->close(animated);}
bool WindowPreviews::visible() const{return impl_->hwnd!=nullptr;}
bool WindowPreviews::open_for(const std::wstring& key) const{return impl_->hwnd&&!impl_->closing&&impl_->key==key;}
bool WindowPreviews::contains(POINT screen) const {
    if(!impl_->hwnd||impl_->closing)return false;
    RECT r{};GetWindowRect(impl_->hwnd,&r);const float scale=impl_->dpi/96;const auto c=impl_->card();
    const RECT card{r.left+static_cast<LONG>((c.left-8)*scale),r.top+static_cast<LONG>((c.top-8)*scale),r.left+static_cast<LONG>((c.right+8)*scale),r.top+static_cast<LONG>((c.bottom+14)*scale)};
    return PtInRect(&card,screen)!=FALSE;
}
HWND WindowPreviews::window() const{return impl_->hwnd;}
}
