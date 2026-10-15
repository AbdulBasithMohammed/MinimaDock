#include "rendering/prayer_toast.h"
#include "platform/com_ptr.h"
#include "rendering/glass_style.h"
#include "rendering/glass.h"
#include "rendering/icon_paths.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace minima {
namespace {
constexpr float canvas_width=320,canvas_height=92;     // DIP; shadow room around the card.
constexpr float card_width=292,card_height=62,pill_height=34,top_gap=14,pill_moon=15,pill_pad=14,pill_gap=8;
constexpr ULONGLONG card_time=20000,done_time=30000,plain_time=60000;
float ease_out(float t){t=std::clamp(t,0.0f,1.0f);return 1-(1-t)*(1-t)*(1-t);}
float back_out(float t){t=std::clamp(t,0.0f,1.0f);const float c=1.6f;return 1+(c+1)*std::pow(t-1,3.0f)+c*std::pow(t-1,2.0f);}
float lerp(float a,float b,float t){return a+(b-a)*t;}
}

struct PrayerToast::Impl {
    HWND hwnd=nullptr;
    PrayerToastInfo info;
    theme::Palette pal=theme::palette(false);
    bool reduced=false,hovered=false,tracking=false,closing=false;
    ULONGLONG shown=0,closed_at=0,raised=0;
    float shrink=0,shrink_from=0,shrink_to=0;ULONGLONG shrink_start=0; // 0 = card, 1 = pill.
    UINT dpi=96,width=0,height=0;
    ComPtr<ID2D1Factory> factory;ComPtr<IDWriteFactory> write;ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> title_font,small_font,count_font,pill_font;
    ComPtr<IWICBitmap> bitmap;ComPtr<ID2D1RenderTarget> target;ComPtr<ID2D1SolidColorBrush> brush;
    HDC dc=nullptr;HBITMAP dib=nullptr;HGDIOBJ previous=nullptr;void* pixels=nullptr;
    // Glass style: the dock's own glass material and its outline Lucide moon.
    bool glass=true,painter_bound=false;
    // Live glass: the compositor blurs what is behind the card (as for the dock);
    // without it the card falls back to a per-pixel-alpha layered window.
    GlassHost host;bool composed=false;unsigned host_generation=~0u;
    float present_alpha=1;D2D1_RECT_F card_box{};std::vector<RECT> region;
    GlassPainter painter;ID2D1Brush* backdrop=nullptr; // Simulated backdrop (snapshots only).
    ComPtr<ID2D1PathGeometry> moon_path;ComPtr<ID2D1StrokeStyle> round_stroke;

    ~Impl(){destroy();release_surface();}
    void release_surface() {
        target.reset();brush.reset();bitmap.reset();painter_bound=false;
        if(dc&&previous)SelectObject(dc,previous);
        if(dib)DeleteObject(dib);
        if(dc)DeleteDC(dc);
        dc=nullptr;dib=nullptr;previous=nullptr;pixels=nullptr;
    }
    void destroy(){if(hwnd){KillTimer(hwnd,1);host.detach();painter_bound=false;const auto window=hwnd;hwnd=nullptr;DestroyWindow(window);}region.clear();}
    bool resources() {
        if(factory)return true;
        if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()))||
           FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())))||
           FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())))){factory.reset();return false;}
        ComPtr<IDWriteFontCollection> fonts;write->GetSystemFontCollection(fonts.put());
        const auto family=theme::text_family(fonts.get());
        const auto make=[&](ComPtr<IDWriteTextFormat>& format,float size,DWRITE_FONT_WEIGHT weight,DWRITE_TEXT_ALIGNMENT align) {
            write->CreateTextFormat(family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",format.put());
            if(!format)return;
            format->SetTextAlignment(align);format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(title_font,14,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_LEADING);
        make(small_font,12,DWRITE_FONT_WEIGHT_LIGHT,DWRITE_TEXT_ALIGNMENT_LEADING);
        make(count_font,19,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_TRAILING);
        make(pill_font,13,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_LEADING);
        return title_font&&small_font&&count_font&&pill_font;
    }
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,w,l);
        switch(message) {
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE; // Never take focus from a game.
        case WM_MOUSEMOVE:
            if(!self->tracking){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};self->tracking=TrackMouseEvent(&track)!=FALSE;}
            if(!self->hovered){self->hovered=true;self->retarget();}
            return 0;
        case WM_MOUSELEAVE:self->tracking=false;self->hovered=false;self->retarget();return 0;
        case WM_LBUTTONUP:self->dismiss();return 0;
        case WM_TIMER:self->tick();return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);if(self->hwnd==window)self->hwnd=nullptr;break;
        default:break;
        }
        return DefWindowProcW(window,message,w,l);
    }
    ULONGLONG now() const{return GetTickCount64();}
    bool countdown() const{return info.end_tick!=0;}
    bool finished() const{return countdown()&&now()>=info.end_tick;}
    // Card while new or hovered; a pill afterwards.
    void retarget() {
        const float want=(hovered||now()-shown<card_time)?0.0f:1.0f;
        if(want==shrink_to)return;
        shrink_from=shrink;shrink_to=want;shrink_start=now();
        if(reduced)shrink=want;
        arm();
    }
    void dismiss(){if(!closing){closing=true;closed_at=now();arm();}}
    bool animating() const {
        const auto t=now();
        return (!reduced&&(t-shown<300||t-shrink_start<300||closing))||closing;
    }
    void arm(){if(hwnd)SetTimer(hwnd,1,animating()?15:250,nullptr);}
    void tick() {
        const auto t=now();
        retarget();
        if(!reduced&&shrink!=shrink_to)shrink=lerp(shrink_from,shrink_to,ease_out(static_cast<float>(t-shrink_start)/260));
        else shrink=shrink_to;
        // Close after the countdown has shown "now" for a while, or a minute without one.
        if(!closing&&((countdown()&&t>=info.end_tick+done_time)||(!countdown()&&t-shown>=plain_time)))dismiss();
        if(closing&&(reduced||t-closed_at>=200)){destroy();return;}
        // Games can raise their own topmost windows; stay above them.
        if(t-raised>=2000){raised=t;SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);}
        render();arm();
    }
    void place() {
        // The screen in use: the foreground window's monitor (a game's, usually).
        HWND foreground=GetForegroundWindow();
        const auto monitor=foreground?MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY):MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};mi.cbSize=sizeof(mi);GetMonitorInfoW(monitor,&mi);
        SetWindowPos(hwnd,HWND_TOPMOST,mi.rcMonitor.left,mi.rcMonitor.top,1,1,SWP_NOACTIVATE);
        dpi=GetDpiForWindow(hwnd);if(!dpi)dpi=96;
        const float scale=static_cast<float>(dpi)/96;
        width=static_cast<UINT>(std::ceil(canvas_width*scale));height=static_cast<UINT>(std::ceil(canvas_height*scale));
        // Below the top edge of the work area, so a top taskbar never covers it.
        const LONG top=mi.rcWork.top>mi.rcMonitor.top?mi.rcWork.top:mi.rcMonitor.top;
        const LONG x=(mi.rcMonitor.left+mi.rcMonitor.right-static_cast<LONG>(width))/2;
        SetWindowPos(hwnd,HWND_TOPMOST,x,top,static_cast<int>(width),static_cast<int>(height),SWP_NOACTIVATE);
        release_surface();
    }
    bool surface() {
        if(target)return true;
        BITMAPINFO header{};header.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);header.bmiHeader.biWidth=static_cast<LONG>(width);
        header.bmiHeader.biHeight=-static_cast<LONG>(height);header.bmiHeader.biPlanes=1;header.bmiHeader.biBitCount=32;header.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(nullptr);dib=dc?CreateDIBSection(dc,&header,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
        if(!dc||!dib||!pixels){release_surface();return false;}
        previous=SelectObject(dc,dib);
        if(FAILED(wic->CreateBitmap(width,height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()))||
           FAILED(factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),target.put()))){release_surface();return false;}
        target->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->CreateSolidColorBrush(pal.ink,brush.put());
        return brush.get()!=nullptr;
    }
    void color(D2D1_COLOR_F c,float alpha){c.a*=alpha;brush->SetColor(c);}
    void text(const std::wstring& value,IDWriteTextFormat* format,D2D1_RECT_F r,D2D1_COLOR_F c,float alpha) {
        if(alpha<=0.01f)return;color(c,alpha);
        target->DrawText(value.c_str(),static_cast<UINT32>(value.size()),format,r,brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    // Text laid out once per frame; digits use tabular figures so a countdown keeps its width.
    ComPtr<IDWriteTextLayout> layout(const std::wstring& value,IDWriteTextFormat* format,bool tabular) {
        ComPtr<IDWriteTextLayout> result;
        if(FAILED(write->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),format,4096,64,result.put())))return result;
        result->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);result->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if(tabular) {
            ComPtr<IDWriteTypography> typography;
            if(SUCCEEDED(write->CreateTypography(typography.put()))) {
                typography->AddFontFeature(DWRITE_FONT_FEATURE{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES,1});
                result->SetTypography(typography.get(),DWRITE_TEXT_RANGE{0,static_cast<UINT32>(value.size())});
            }
        }
        return result;
    }
    static float width_of(IDWriteTextLayout* text){DWRITE_TEXT_METRICS m{};if(!text||FAILED(text->GetMetrics(&m)))return 0;return m.widthIncludingTrailingWhitespace;}
    void draw(IDWriteTextLayout* text,float x,float centre,D2D1_COLOR_F c,float alpha) {
        if(!text||alpha<=0.01f)return;DWRITE_TEXT_METRICS m{};text->GetMetrics(&m);
        color(c,alpha);target->DrawTextLayout(D2D1::Point2F(x,centre-m.height/2),text,brush.get());
    }
    // The dock's outline moon (Lucide, 24-unit box), stroked like its other icons.
    void outline_moon(float cx,float cy,float size,float alpha) {
        if(!moon_path) {
            if(FAILED(factory->CreatePathGeometry(moon_path.put())))return;
            ComPtr<ID2D1GeometrySink> sink;if(FAILED(moon_path->Open(sink.put()))){moon_path.reset();return;}
            bool open=false;
            for(const auto& part:icon_paths::moon)for(size_t k=0;k<part.count;++k) {
                const auto& op=part.ops[k];
                if(op.op=='M'){if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->BeginFigure(D2D1::Point2F(op.v[0],op.v[1]),D2D1_FIGURE_BEGIN_HOLLOW);open=true;}
                else if(op.op=='L')sink->AddLine(D2D1::Point2F(op.v[0],op.v[1]));
                else if(op.op=='C')sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(op.v[0],op.v[1]),D2D1::Point2F(op.v[2],op.v[3]),D2D1::Point2F(op.v[4],op.v[5])));
                else if(op.op=='Z'&&open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;}
            }
            if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            const D2D1_STROKE_STYLE_PROPERTIES round{D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,10,D2D1_DASH_STYLE_SOLID,0};
            factory->CreateStrokeStyle(round,nullptr,0,round_stroke.put());
        }
        D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
        const float k=size/24;
        target->SetTransform(D2D1::Matrix3x2F::Translation(-12,-12)*D2D1::Matrix3x2F::Scale(k,k)*D2D1::Matrix3x2F::Translation(cx,cy)*before);
        color(pal.accent,alpha);target->DrawGeometry(moon_path.get(),brush.get(),2,round_stroke.get());
        target->SetTransform(before);
    }
    void moon(float cx,float cy,float radius,float alpha){if(glass)outline_moon(cx,cy,radius*2.1f,alpha);else crescent(cx,cy,radius,alpha);}
    // A crescent: one circle less an offset one.
    void crescent(float cx,float cy,float radius,float alpha) {
        ComPtr<ID2D1EllipseGeometry> outer,inner;ComPtr<ID2D1PathGeometry> moon;ComPtr<ID2D1GeometrySink> sink;
        factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx,cy),radius,radius),outer.put());
        factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx+radius*.42f,cy-radius*.30f),radius*.82f,radius*.82f),inner.put());
        if(!outer||!inner||FAILED(factory->CreatePathGeometry(moon.put()))||FAILED(moon->Open(sink.put())))return;
        outer->CombineWithGeometry(inner.get(),D2D1_COMBINE_MODE_EXCLUDE,nullptr,sink.get());sink->Close();
        color(pal.accent,alpha);target->FillGeometry(moon.get(),brush.get());
    }
    std::wstring remaining() const {
        const auto t=now();if(t>=info.end_tick)return {};
        const auto seconds=(info.end_tick-t+999)/1000;wchar_t value[32]{};
        if(seconds>=3600)swprintf(value,32,L"%llu:%02llu:%02llu",seconds/3600,(seconds/60)%60,seconds%60);
        else swprintf(value,32,L"%llu:%02llu",seconds/60,seconds%60);
        return value;
    }
    void render() {
        if(!hwnd)return;
        if(composed){render_composed();return;}
        if(!surface()||!paint())return;
        WICRect all{0,0,static_cast<INT>(width),static_cast<INT>(height)};
        if(FAILED(bitmap->CopyPixels(&all,width*4,width*height*4,static_cast<BYTE*>(pixels))))return;
        RECT position{};GetWindowRect(hwnd,&position);
        POINT destination{position.left,position.top},source{0,0};SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd,nullptr,&destination,&size,dc,&source,0,&blend,ULW_ALPHA);
    }
    void render_composed() {
        if(!host.attached()){host.attach(hwnd,factory.get());painter_bound=false;}
        D2D1_MATRIX_3X2_F origin{};auto* context=host.begin(width,height,static_cast<float>(dpi),origin);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target.reset();context->AddRef();*target.put()=context;
        // Device resources follow the compositor's device.
        if(host.allocations()!=host_generation||!brush){host_generation=host.allocations();brush.reset();painter_bound=false;context->CreateSolidColorBrush(pal.ink,brush.put());}
        else if(painter_bound)painter.retarget(context);
        paint(false,origin);
        host.end();
        host.backdrops(painter.shapes,static_cast<float>(dpi));
        host.present(present_alpha,0,true);
        target.reset();
        // Only the card (and its soft shadow, which the region also clips) takes
        // the pointer; the rest of the strip passes clicks through.
        const float scale=static_cast<float>(dpi)/96;
        const RECT r{static_cast<LONG>(std::floor((card_box.left-12)*scale)),static_cast<LONG>(std::floor((card_box.top-6)*scale)),static_cast<LONG>(std::ceil((card_box.right+12)*scale)),static_cast<LONG>(std::ceil((card_box.bottom+14)*scale))};
        if(region.empty()||!EqualRect(&region.front(),&r)) {
            region.assign(1,r);const int corner=static_cast<int>(std::lround(std::min(28.0f,static_cast<float>(r.bottom-r.top))));
            if(auto shape=CreateRoundRectRgn(r.left,r.top,r.right+1,r.bottom+1,corner,corner))if(!SetWindowRgn(hwnd,shape,FALSE))DeleteObject(shape);
        }
    }
    bool paint(bool frame=true,D2D1_MATRIX_3X2_F base=D2D1::Matrix3x2F::Identity()) {
        const auto t=now();
        // Appear: slide down and spring to size. Close: fade and lift.
        float alpha=1,lift=0,grow=1;
        if(!reduced) {
            const float in=static_cast<float>(t-shown)/260;alpha=std::min(1.0f,in*1.6f);lift=(1-ease_out(in))*-10;grow=.94f+.06f*back_out(in);
            if(closing){const float out=std::clamp(static_cast<float>(t-closed_at)/200,0.0f,1.0f);alpha*=1-out;lift-=out*6;}
        }
        present_alpha=alpha;if(composed)alpha=1;
        const bool done=finished();const auto left=countdown()?(done?info.done:remaining()):std::wstring();
        // The pill fits the moon, the name and the countdown (or the "done" text),
        // centred as one group. Tabular digits keep its width steady each second.
        const auto name=layout(countdown()&&done?info.done:info.pill,pill_font.get(),false);
        const auto clock=countdown()&&!done?layout(left,pill_font.get(),true):ComPtr<IDWriteTextLayout>();
        const float slot=clock?width_of(clock.get()):0;
        const float group=pill_moon+pill_gap+width_of(name.get())+(clock?pill_gap+slot:0);
        const float pill_width=group+2*pill_pad;
        const float w=lerp(card_width,pill_width,shrink),h=lerp(card_height,pill_height,shrink);
        const float cx=canvas_width/2,top=top_gap+lift;
        const auto box=D2D1::RectF(cx-w/2,top,cx+w/2,top+h);const float radius=lerp(14,pill_height/2,shrink);
        card_box=D2D1::RectF(cx-(cx-box.left)*grow,top,cx+(box.right-cx)*grow,top+(box.bottom-top)*grow);
        if(frame){target->BeginDraw();target->Clear(D2D1::ColorF(0,0));}
        target->SetTransform(D2D1::Matrix3x2F::Scale(grow,grow,D2D1::Point2F(cx,top))*base);
        if(glass) {
            painter.palette=pal;painter.live=composed;
            if(!painter_bound){painter.bind(target.get(),factory.get());painter_bound=true;}
            painter.origin=base;painter.begin_frame();painter.layer_opacity=alpha;
            painter.paint(box,radius,GlassKind::Panel,backdrop);
        } else {
            color(pal.shadow_color,.22f*alpha);target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left+1,box.top+3,box.right+1,box.bottom+5),radius,radius),brush.get());
            auto background=pal.solid_top;background.a=.97f;color(background,alpha);
            target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush.get());
            color(pal.tile_edge,alpha*1.6f);target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left+.5f,box.top+.5f,box.right-.5f,box.bottom-.5f),radius,radius),brush.get(),1);
        }
        // Contents (and the progress bar) are clipped to the rounded card.
        ComPtr<ID2D1RoundedRectangleGeometry> shape;ComPtr<ID2D1Layer> layer;
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box,radius,radius),shape.put());target->CreateLayer(nullptr,layer.put());
        if(shape&&layer)target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),shape.get()),layer.get());
        // Card contents fade out as it shrinks; the pill's fade in.
        const float card_alpha=alpha*std::clamp(1-shrink*2,0.0f,1.0f),pill_alpha=alpha*std::clamp(shrink*2-1,0.0f,1.0f);
        if(card_alpha>0) {
            moon(box.left+26,box.top+h/2,10,card_alpha);
            text(info.title,title_font.get(),D2D1::RectF(box.left+48,box.top+9,box.right-86,box.top+31),pal.ink,card_alpha);
            text(done?info.done:info.subtitle,small_font.get(),D2D1::RectF(box.left+48,box.top+30,box.right-86,box.top+50),pal.ink_secondary,card_alpha);
            if(countdown()&&!done){const auto big=layout(left,count_font.get(),true);draw(big.get(),box.right-16-width_of(big.get()),box.top+h/2,pal.accent,card_alpha);}
            // Time left until the countdown ends.
            if(countdown()&&!done&&info.end_tick>info.start_tick) {
                const float k=std::clamp(static_cast<float>(info.end_tick-t)/static_cast<float>(info.end_tick-info.start_tick),0.0f,1.0f);
                color(pal.track,card_alpha);target->FillRectangle(D2D1::RectF(box.left,box.bottom-3,box.right,box.bottom),brush.get());
                color(pal.accent,card_alpha);target->FillRectangle(D2D1::RectF(box.left,box.bottom-3,box.left+(box.right-box.left)*k,box.bottom),brush.get());
            }
        }
        if(pill_alpha>0) {
            const float start=(box.left+box.right)/2-group/2,centre=box.top+h/2;
            moon(start+pill_moon/2,centre,7.5f,pill_alpha);
            const float name_x=start+pill_moon+pill_gap;draw(name.get(),name_x,centre,pal.ink,pill_alpha);
            if(clock)draw(clock.get(),name_x+width_of(name.get())+pill_gap,centre,pal.accent,pill_alpha);
        }
        if(shape&&layer)target->PopLayer();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        if(frame&&FAILED(target->EndDraw())){release_surface();return false;}
        return true;
    }
    void show(const PrayerToastInfo& value,const theme::Palette& palette,bool reduced_motion) {
        destroy();
        if(!resources())return;
        info=value;pal=palette;reduced=reduced_motion;hovered=tracking=closing=false;
        shown=now();shrink=shrink_from=shrink_to=0;shrink_start=0;
        WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.PrayerToast";klass.hCursor=LoadCursorW(nullptr,IDC_HAND);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        composed=GlassHost::available();
        hwnd=CreateWindowExW((composed?WS_EX_NOREDIRECTIONBITMAP:WS_EX_LAYERED)|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,klass.lpszClassName,L"MinimaDock prayer time",WS_POPUP,0,0,1,1,nullptr,nullptr,klass.hInstance,this);
        if(!hwnd)return;
        place();render();
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        if(info.chime) {
            wchar_t folder[MAX_PATH]{};GetWindowsDirectoryW(folder,MAX_PATH);
            const auto sound=std::wstring(folder)+(info.sound==1?L"\\Media\\Windows Notify Messaging.wav":L"\\Media\\Windows Notify Calendar.wav");
            if(GetFileAttributesW(sound.c_str())==INVALID_FILE_ATTRIBUTES||!PlaySoundW(sound.c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT))PlaySoundW(L"SystemAsterisk",nullptr,SND_ALIAS|SND_ASYNC);
        }
        arm();
    }
};
PrayerToast::PrayerToast():impl_(std::make_unique<Impl>()){}
namespace {
PrayerToastInfo sample(ULONGLONG start,ULONGLONG seconds) {
    PrayerToastInfo info;info.title=L"Asr \x00B7 Adhan";info.subtitle=L"Iqama at 5:10 PM";info.pill=L"Asr";info.done=L"Iqama now";
    info.start_tick=start;info.end_tick=start+seconds*1000;return info;
}
}
int save_prayer_toast_snapshot(const std::wstring& path,bool pill) {
    PrayerToast::Impl toast;
    if(!toast.resources())return 1;
    const auto now=GetTickCount64();
    toast.info=sample(now-38000,600);toast.pal=theme::palette(false);toast.reduced=true;toast.shown=now-60000;
    toast.shrink=toast.shrink_to=pill?1.0f:0.0f;toast.dpi=192;
    toast.width=static_cast<UINT>(canvas_width*2);toast.height=static_cast<UINT>(canvas_height*2);
    if(!toast.surface()||!toast.paint())return 1;
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    if(FAILED(toast.wic->CreateStream(stream.put()))||FAILED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))||
       FAILED(toast.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()))||FAILED(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(frame.put(),nullptr))||FAILED(frame->Initialize(nullptr))||FAILED(frame->SetSize(toast.width,toast.height)))return 1;
    auto format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetPixelFormat(&format))||FAILED(frame->WriteSource(toast.bitmap.get(),nullptr))||FAILED(frame->Commit())||FAILED(encoder->Commit()))return 1;
    return 0;
}
// Current (flat) and glass styles, card and pill, over a game-like scene.
int save_prayer_toast_compare(const std::wstring& path) {
    PrayerToast::Impl toast;
    if(!toast.resources())return 1;
    constexpr float sheet_width=680,sheet_height=232,scale=2;
    toast.dpi=192;toast.width=static_cast<UINT>(sheet_width*scale);toast.height=static_cast<UINT>(sheet_height*scale);
    if(!toast.surface())return 1;
    auto* t=toast.target.get();
    t->BeginDraw();t->Clear(D2D1::ColorF(0x0b1220));
    // Scene: deep blue night with a bright cyan energy glow and a warm light.
    ComPtr<ID2D1RadialGradientBrush> glow,warm;ComPtr<ID2D1GradientStopCollection> a,b;
    const D2D1_GRADIENT_STOP cyan[]={{0,D2D1::ColorF(0x7fe3ff,1)},{.35f,D2D1::ColorF(0x1f7fd8,.85f)},{1,D2D1::ColorF(0x0b1220,0)}};
    const D2D1_GRADIENT_STOP amber[]={{0,D2D1::ColorF(0xffd9a0,.95f)},{.5f,D2D1::ColorF(0xd86a2a,.5f)},{1,D2D1::ColorF(0x0b1220,0)}};
    t->CreateGradientStopCollection(cyan,3,a.put());t->CreateGradientStopCollection(amber,3,b.put());
    t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(470,70),D2D1::Point2F(0,0),260,170),a.get(),glow.put());
    t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(150,180),D2D1::Point2F(0,0),200,120),b.get(),warm.put());
    t->FillRectangle(D2D1::RectF(0,0,sheet_width,sheet_height),glow.get());
    t->FillRectangle(D2D1::RectF(0,0,sheet_width,sheet_height),warm.get());
    toast.pal=theme::palette(false);toast.reduced=true;
    const auto now=GetTickCount64();
    toast.info=sample(now-38000,600);toast.shown=now-60000;
    // The glass blurs what is behind it; this smooth scene stands in for that.
    toast.backdrop=glow.get();
    for(int column=0;column<2;++column)for(int row=0;row<2;++row) {
        toast.glass=column==1;toast.shrink=toast.shrink_to=row==1?1.0f:0.0f;
        const float x=static_cast<float>(column)*340+10,y=static_cast<float>(row)*96+20;
        toast.paint(false,D2D1::Matrix3x2F::Translation(x,y));
    }
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto label=[&](const wchar_t* value,float x){toast.text(value,toast.small_font.get(),D2D1::RectF(x,2,x+300,22),theme::rgb(0xffffff),.85f);};
    label(L"Now: flat",24);label(L"Glass (dock material, outline moon)",364);
    if(FAILED(t->EndDraw()))return 1;
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    if(FAILED(toast.wic->CreateStream(stream.put()))||FAILED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))||
       FAILED(toast.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()))||FAILED(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(frame.put(),nullptr))||FAILED(frame->Initialize(nullptr))||FAILED(frame->SetSize(toast.width,toast.height)))return 1;
    auto format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetPixelFormat(&format))||FAILED(frame->WriteSource(toast.bitmap.get(),nullptr))||FAILED(frame->Commit())||FAILED(encoder->Commit()))return 1;
    return 0;
}
// Shows the real card with a 90 second countdown and returns when it closes.
int run_prayer_toast_demo() {
    PrayerToast toast;
    toast.show(sample(GetTickCount64(),90),theme::palette(false),false);
    MSG message{};
    while(toast.visible()&&GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return 0;
}
PrayerToast::~PrayerToast()=default;
void PrayerToast::show(const PrayerToastInfo& info,const theme::Palette& palette,bool reduced_motion){impl_->show(info,palette,reduced_motion);}
void PrayerToast::close(){impl_->destroy();}
bool PrayerToast::visible() const{return impl_->hwnd!=nullptr;}
}
