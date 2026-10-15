#include "rendering/jump_list.h"
#include "rendering/glass.h"
#include "rendering/glass_style.h"
#include "taskbar/state_machine.h"
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace minima {
namespace {
constexpr UINT_PTR motion_timer=1;
// Windows 11 jump-list metrics in DIPs; margins leave room for the shadow.
constexpr float card_width=294,pad_x=4,pad_top=2,pad_bottom=2,header_height=32,row_height=32,separator_height=5;
constexpr float side=26,top_margin=20,bottom_margin=34,radius=8;
constexpr float icon_size=16,icon_left=16,text_left=44,header_left=15;
void check(HRESULT hr){if(FAILED(hr))throw hr;}
bool inside(D2D1_RECT_F r,float x,float y){return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;}
}
struct JumpList::Impl {
    enum class Kind {Header,Entry,Separator};
    struct Row {Kind kind;size_t section=0,entry=0;float top=0,bottom=0;};
    HWND hwnd=nullptr,owner=nullptr;
    GlassHost host;
    GlassPainter painter;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> text_font,header_font,glyph_font;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<JumpSection> sections;
    std::vector<std::vector<ComPtr<ID2D1Bitmap>>> icons;
    std::vector<Row> rows;
    ContextHandler context;
    MenuMessages messages;
    theme::Palette palette=theme::palette(false);
    PanelReveal reveal;
    float dpi=96,content_height=0;
    UINT width=0,height=0;
    int hot=-1; // Row index of the highlighted entry.
    unsigned session=0;
    UINT message=jump_list_event_message;
    bool activate=true;
    bool live=false,reduced=false,bound=false,closing=false,timer=false,tracking=false,reported=false,in_context=false;

    static std::int64_t now(){return static_cast<std::int64_t>(GetTickCount64());}
    void initialize() {
        if(factory)return;
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IDWriteFontCollection> collection;check(write->GetSystemFontCollection(collection.put()));
        const auto family=theme::text_family(collection.get());
        const auto format=[&](const wchar_t* face,float size,DWRITE_FONT_WEIGHT weight,ComPtr<IDWriteTextFormat>& out) {
            check(write->CreateTextFormat(face,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"",out.put()));
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};ComPtr<IDWriteInlineObject> ellipsis;
            write->CreateEllipsisTrimmingSign(out.get(),ellipsis.put());out->SetTrimming(&trimming,ellipsis.get());
        };
        format(family,14,DWRITE_FONT_WEIGHT_NORMAL,text_font);format(family,12,DWRITE_FONT_WEIGHT_SEMI_BOLD,header_font);
        format(L"Segoe Fluent Icons",16,DWRITE_FONT_WEIGHT_NORMAL,glyph_font);glyph_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    D2D1_RECT_F card() const{return D2D1::RectF(side,top_margin,side+card_width,top_margin+content_height);}
    D2D1_POINT_2F pivot() const{const auto c=card();return D2D1::Point2F((c.left+c.right)/2,c.bottom);}
    D2D1_RECT_F row_rect(const Row& r) const{const auto c=card();return D2D1::RectF(c.left+pad_x,r.top,c.right-pad_x,r.bottom);}
    D2D1_POINT_2F content(LPARAM l) const {
        const auto p=pivot();const float s=reveal.scale();
        return D2D1::Point2F(p.x+(GET_X_LPARAM(l)*96.0f/dpi-p.x)/s,p.y+(GET_Y_LPARAM(l)*96.0f/dpi-reveal.offset()-p.y)/s);
    }
    const JumpEntry& entry(int row) const{const auto& r=rows[static_cast<size_t>(row)];return sections[r.section].entries[r.entry];}
    void layout() {
        rows.clear();float y=top_margin+pad_top;bool first=true;
        for(size_t s=0;s<sections.size();++s) {
            const auto& entries=sections[s].entries;
            if(std::none_of(entries.begin(),entries.end(),[](const JumpEntry& e){return !e.separator;}))continue;
            if(!first&&sections[s].title.empty()){rows.push_back({Kind::Separator,s,0,y,y+separator_height});y+=separator_height;}
            first=false;
            if(!sections[s].title.empty()){rows.push_back({Kind::Header,s,0,y,y+header_height});y+=header_height;}
            for(size_t e=0;e<entries.size();++e) {
                if(entries[e].separator) {
                    // Only between entries, never doubled.
                    if(rows.empty()||rows.back().kind!=Kind::Entry||e+1==entries.size())continue;
                    rows.push_back({Kind::Separator,s,e,y,y+separator_height});y+=separator_height;continue;
                }
                rows.push_back({Kind::Entry,s,e,y,y+row_height});y+=row_height;
            }
            if(!rows.empty()&&rows.back().kind==Kind::Separator){y-=separator_height;rows.pop_back();}
        }
        content_height=y+pad_bottom-top_margin;
    }
    // Drop the oldest history rows until the list fits above the dock.
    void fit(float room) {
        layout();
        while(content_height>room) {
            // Shorten the last trimmable section first, as Explorer drops the oldest history.
            auto found=std::find_if(sections.rbegin(),sections.rend(),[](const JumpSection& s){return !s.fixed&&!s.title.empty()&&!s.entries.empty();});
            if(found==sections.rend())break;
            found->entries.pop_back();
            while(!found->entries.empty()&&found->entries.back().separator)found->entries.pop_back();
            layout();
        }
    }
    ComPtr<ID2D1Bitmap> convert(ID2D1RenderTarget* target,IWICBitmapSource* source,UINT pixels) {
        ComPtr<ID2D1Bitmap> result;ComPtr<IWICBitmapScaler> scaled;ComPtr<IWICFormatConverter> converted;
        if(FAILED(wic->CreateBitmapScaler(scaled.put()))||FAILED(scaled->Initialize(source,pixels,pixels,WICBitmapInterpolationModeFant))||
           FAILED(wic->CreateFormatConverter(converted.put()))||
           FAILED(converted->Initialize(scaled.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return result;
        target->CreateBitmapFromWicBitmap(converted.get(),nullptr,result.put());return result;
    }
    ComPtr<ID2D1Bitmap> load_icon(ID2D1RenderTarget* target,const JumpEntry& e) {
        const auto pixels=static_cast<UINT>(std::max(1.0f,std::round(icon_size*dpi/96)));
        if(e.item) {
            ComPtr<IShellItemImageFactory> images;HBITMAP image=nullptr;
            if(SUCCEEDED(e.item->QueryInterface(IID_PPV_ARGS(images.put())))&&SUCCEEDED(images->GetImage(SIZE{static_cast<LONG>(pixels),static_cast<LONG>(pixels)},SIIGBF_ICONONLY,&image))) {
                ComPtr<IWICBitmap> source;const bool made=SUCCEEDED(wic->CreateBitmapFromHBITMAP(image,nullptr,WICBitmapUsePremultipliedAlpha,source.put()));
                DeleteObject(image);
                if(made)if(auto result=convert(target,source.get(),pixels))return result;
            }
        }
        if(e.icon){ComPtr<IWICBitmap> source;if(SUCCEEDED(wic->CreateBitmapFromHICON(static_cast<HICON>(e.icon.get()),source.put())))return convert(target,source.get(),pixels);}
        return {};
    }
    void place(RECT anchor,LONG dock_top) {
        MONITORINFO info{sizeof(info),{},{},0};GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&info);
        const float scale=dpi/96;
        const LONG card_bottom=dock_top-static_cast<LONG>(std::lround(12*scale));
        fit(std::max(row_height*3,(card_bottom-info.rcWork.top)/scale-16));
        width=static_cast<UINT>(std::ceil((card_width+2*side)*scale));height=static_cast<UINT>(std::ceil((top_margin+content_height+bottom_margin)*scale));
        // Centred on the icon, 12 DIP above the pill (like the previews card).
        const LONG centre=(anchor.left+anchor.right)/2;
        const LONG x=std::clamp<LONG>(centre-static_cast<LONG>(width)/2,info.rcMonitor.left,std::max(info.rcMonitor.left,info.rcMonitor.right-static_cast<LONG>(width)));
        const LONG y=card_bottom-static_cast<LONG>(std::lround((top_margin+content_height)*scale));
        SetWindowPos(hwnd,HWND_TOPMOST,x,y,static_cast<int>(width),static_cast<int>(height),SWP_NOACTIVATE);
        const auto c=card();
        if(auto region=CreateRoundRectRgn(static_cast<int>((c.left-6)*scale),static_cast<int>((c.top-6)*scale),static_cast<int>((c.right+6)*scale)+1,static_cast<int>((c.bottom+10)*scale)+1,static_cast<int>(28*scale),static_cast<int>(28*scale)))
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
        if(!host.attached()){host.attach(hwnd,factory.get());bound=false;}
        D2D1_MATRIX_3X2_F origin{};auto dc=host.begin(width,height,dpi,origin);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        painter.palette=palette;painter.live=live;
        if(!bound) {
            painter.bind(dc,factory.get());brush.reset();check(dc->CreateSolidColorBrush(palette.ink,brush.put()));
            icons.clear();
            for(const auto& s:sections){auto& list=icons.emplace_back();for(const auto& e:s.entries)list.push_back(e.glyph||e.separator?ComPtr<ID2D1Bitmap>{}:load_icon(dc,e));}
            bound=true;
        } else painter.retarget(dc);
        painter.origin=origin;painter.begin_frame();
        const auto p=pivot();const float s=reveal.scale();
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s,s,p)*(*D2D1::Matrix3x2F::ReinterpretBaseType(&origin)));
        painter.paint(card(),radius,GlassKind::Menu);
        for(size_t i=0;i<rows.size();++i) {
            const auto& row=rows[i];const auto r=row_rect(row);
            const auto c=card();
            if(row.kind==Kind::Separator) { // Full width, as in the system menu.
                color(palette.light?theme::rgb(0x000000,.10f):theme::rgb(0xffffff,.10f));const float y=(r.top+r.bottom)/2;
                dc->FillRectangle(D2D1::RectF(c.left,y-.5f,c.right,y+.5f),brush.get());continue;
            }
            if(row.kind==Kind::Header) {
                color(palette.ink);const auto& title=sections[row.section].title;
                dc->DrawTextW(title.c_str(),static_cast<UINT32>(title.size()),header_font.get(),D2D1::RectF(c.left+header_left,r.top,c.right-12,r.bottom),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                continue;
            }
            const auto& e=sections[row.section].entries[row.entry];
            if(static_cast<int>(i)==hot&&e.enabled){color(palette.hover);dc->FillRoundedRectangle(D2D1::RoundedRect(r,4,4),brush.get());}
            const float cy=(r.top+r.bottom)/2,ix=c.left+icon_left;
            const auto icon_box=D2D1::RectF(ix,cy-icon_size/2,ix+icon_size,cy+icon_size/2);
            const auto ink=e.enabled?palette.ink:palette.ink_muted;
            if(e.glyph){color(ink);dc->DrawTextW(&e.glyph,1,glyph_font.get(),D2D1::RectF(icon_box.left-4,r.top,icon_box.right+4,r.bottom),brush.get());}
            else if(const auto& bitmap=icons[row.section][row.entry])dc->DrawBitmap(bitmap.get(),icon_box,e.enabled?1.0f:.45f);
            color(ink);
            dc->DrawTextW(e.text.c_str(),static_cast<UINT32>(e.text.size()),text_font.get(),D2D1::RectF(c.left+text_left,r.top,c.right-12,r.bottom),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        host.end();
        host.backdrops(painter.shapes,dpi);
        host.present(reveal.opacity(),reveal.offset()*dpi/96,live);
    }
    void report(UINT id){if(reported)return;reported=true;if(IsWindow(owner))PostMessageW(owner,message,id,static_cast<LPARAM>(session));}
    void destroy() {
        report(0);
        if(hwnd){const auto window=hwnd;host.detach();bound=false;hwnd=nullptr;DestroyWindow(window);}
        closing=false;timer=false;tracking=false;in_context=false;hot=-1;rows.clear();sections.clear();icons.clear();context=nullptr;messages=nullptr;
    }
    void close(bool animated) {
        if(!hwnd)return;
        report(0);
        if(animated&&!reduced){if(!closing){closing=true;reveal.close(now());schedule();}return;}
        destroy();
    }
    void choose(int row){if(row<0||!entry(row).enabled)return;report(entry(row).id);close(true);}
    int hit(LPARAM l) const {
        const auto point=content(l);
        for(size_t i=0;i<rows.size();++i)if(rows[i].kind==Kind::Entry&&inside(row_rect(rows[i]),point.x,point.y))return static_cast<int>(i);
        return -1;
    }
    void set_hot(int row){if(row!=hot){hot=row;schedule();}}
    void step(int direction) {
        if(rows.empty())return;const int count=static_cast<int>(rows.size());
        int row=hot<0?(direction>0?-1:count):hot;
        for(int n=0;n<count;++n){row=(row+direction+count)%count;if(rows[static_cast<size_t>(row)].kind==Kind::Entry&&entry(row).enabled){set_hot(row);return;}}
    }
    void open_context(int row,POINT point) {
        if(row<0||!context||closing)return;
        in_context=true;bool done=false;
        try{done=context(entry(row).id,point,hwnd);}catch(...){in_context=false;throw;}
        in_context=false;
        if(done||GetForegroundWindow()!=hwnd)close(true);
    }
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(message==WM_NCDESTROY){SetWindowLongPtrW(window,GWLP_USERDATA,0);return DefWindowProcW(window,message,w,l);}
        if(!self)return DefWindowProcW(window,message,w,l);
        try {return self->handle(window,message,w,l);}catch(...){PostMessageW(window,WM_CLOSE,0,0);return 0;}
    }
    LRESULT handle(HWND window,UINT message,WPARAM w,LPARAM l) {
        if(in_context&&messages){LRESULT result=0;if(messages(message,w,l,result))return result;}
        switch(message) {
        case WM_ERASEBKGND:return 1;
        case WM_MOUSEACTIVATE:if(!activate)return MA_NOACTIVATE;return DefWindowProcW(window,message,w,l);
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);try{if(window==hwnd)render();}catch(...){EndPaint(window,&ps);throw;}EndPaint(window,&ps);return 0;}
        case WM_TIMER:
            if(w==motion_timer){reveal.tick(now());if(closing&&reveal.hidden()){destroy();return 0;}schedule();}
            return 0;
        case WM_ACTIVATE:
            if(LOWORD(w)==WA_INACTIVE&&!in_context)close(true);
            return 0;
        case WM_MOUSEMOVE:
            if(!tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window,0};TrackMouseEvent(&t);tracking=true;}
            if(!closing)set_hot(hit(l));return 0;
        case WM_MOUSELEAVE:tracking=false;set_hot(-1);return 0;
        case WM_LBUTTONUP:if(!closing)choose(hit(l));return 0;
        case WM_RBUTTONUP:if(!closing){const int row=hit(l);set_hot(row);POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(window,&point);open_context(row,point);}return 0;
        case WM_CONTEXTMENU:
            if(!closing&&hot>=0) { // Keyboard: menu key or Shift+F10.
                const auto r=row_rect(rows[static_cast<size_t>(hot)]);const float scale=dpi/96;
                POINT point{static_cast<LONG>(std::lround((card().left+text_left)*scale)),static_cast<LONG>(std::lround(r.bottom*scale))};
                ClientToScreen(window,&point);open_context(hot,point);
            }
            return 0;
        case WM_KEYDOWN:
            if(closing)return 0;
            if(w==VK_ESCAPE)close(true);
            else if(w==VK_UP||(w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000)))step(-1);
            else if(w==VK_DOWN||w==VK_TAB)step(1);
            else if(w==VK_RETURN||w==VK_SPACE)choose(hot);
            return 0;
        case WM_CLOSE:close(true);return 0;
        case WM_DESTROY:KillTimer(window,motion_timer);timer=false;return 0;
        default:return DefWindowProcW(window,message,w,l);
        }
    }
};
JumpList::JumpList(UINT message):impl_(std::make_unique<Impl>()){impl_->message=message;}
JumpList::~JumpList(){impl_->close(false);}
unsigned JumpList::open(HWND owner,std::vector<JumpSection> sections,RECT anchor,LONG dock_top,UINT dpi,const theme::Palette& palette,
                        bool live,bool reduced,bool keyboard,ContextHandler context,MenuMessages messages,bool activate) {
    auto& i=*impl_;
    i.close(false);
    if(!GlassHost::available())throw E_NOTIMPL;
    i.initialize();
    i.owner=owner;i.palette=palette;i.live=live;i.reduced=reduced;i.dpi=static_cast<float>(dpi);
    i.sections=std::move(sections);i.context=std::move(context);i.messages=std::move(messages);
    i.reported=false;i.closing=false;i.hot=-1;i.bound=false;++i.session;
    WNDCLASSW klass{};klass.lpfnWndProc=Impl::proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.JumpList";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw HRESULT_FROM_WIN32(GetLastError());
    i.activate=activate;
    const auto created=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|(activate?0:WS_EX_NOACTIVATE),klass.lpszClassName,L"MinimaDock jump list",WS_POPUP,0,0,1,1,owner,nullptr,klass.hInstance,&i);
    if(!created){i.hwnd=nullptr;i.reported=true;throw HRESULT_FROM_WIN32(GetLastError());}
    i.hwnd=created;
    try {
        i.reveal.start(Impl::now(),reduced);
        i.place(anchor,dock_top);
        if(keyboard)i.step(1);
        i.render();
    } catch(...) {i.reported=true;i.destroy();throw;}
    if(activate){ShowWindow(i.hwnd,SW_SHOW);SetForegroundWindow(i.hwnd);}
    else ShowWindow(i.hwnd,SW_SHOWNOACTIVATE);
    i.schedule();
    return i.session;
}
void JumpList::close(bool animated){impl_->close(animated);}
bool JumpList::visible() const{return impl_->hwnd!=nullptr;}
HWND JumpList::window() const{return impl_->hwnd;}
}
