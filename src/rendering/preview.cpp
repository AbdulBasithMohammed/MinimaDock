#include "rendering/preview.h"
#include "rendering/calendar.h"
#include "rendering/theme.h"
#include "rendering/launcher_logo.h"
#include "taskbar/state_machine.h"
#include "taskbar/layout.h"
#include "taskbar/media_marquee.h"
#include "windows/inventory.h"
#include "windows/window_state.h"
#include "windows/media.h"
#include "platform/com_ptr.h"
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <string_view>
#include <fstream>
#include <chrono>
#include <unordered_map>

namespace minima {
namespace {
constexpr UINT inventory_ready = WM_APP + 1, windows_changed = WM_APP + 2, visibility_changed=WM_APP+3;
constexpr UINT foreground_changed=WM_APP+4;
constexpr UINT_PTR frame_timer = 1, reconcile_timer = 2, clock_timer = 3, media_title_timer=4;
HWND event_target = nullptr;
std::atomic_bool event_queued=false;
std::atomic_bool visibility_queued=false;
void CALLBACK window_event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD event_time) {
    if(!event_target||!window||child!=CHILDID_SELF||(object!=OBJID_WINDOW&&event!=EVENT_SYSTEM_FOREGROUND))return;
    if(event==EVENT_SYSTEM_FOREGROUND)PostMessageW(event_target,foreground_changed,reinterpret_cast<WPARAM>(window),event_time);
    if(event==EVENT_OBJECT_LOCATIONCHANGE) {
        if(window==GetForegroundWindow()&&!visibility_queued.exchange(true))PostMessageW(event_target,visibility_changed,0,0);
        return;
    }
    if(event!=EVENT_SYSTEM_FOREGROUND) {
    if(GetAncestor(window,GA_ROOT)!=window||GetWindowTextLengthW(window)==0)return;
    if(GetWindowLongPtrW(window,GWL_EXSTYLE)&(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))return;
    wchar_t name[128]{};GetClassNameW(window,name,128);
    if(wcscmp(name,L"Progman")==0||wcscmp(name,L"WorkerW")==0||wcsstr(name,L"TrayWnd"))return;
    }
    if(!event_queued.exchange(true))PostMessageW(event_target,windows_changed,0,0);
}
void check(HRESULT hr) { if (FAILED(hr)) throw hr; }
std::string json_text(const std::wstring& value) {
    std::string utf8(WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),utf8.data(),static_cast<int>(utf8.size()),nullptr,nullptr);
    std::string escaped="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:utf8) {
        if(c=='"'||c=='\\'){escaped+='\\';escaped+=static_cast<char>(c);}
        else if(c<32){escaped+="\\u00";escaped+=hex[c>>4];escaped+=hex[c&15];}
        else escaped+=static_cast<char>(c);
    }
    return escaped+'"';
}
bool inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
struct Group { std::wstring name, initial, executable; std::vector<HWND> windows; std::wstring launch_path; NativeIcon icon; bool pinned=false; std::wstring app_id; ClickPulse pulse{}; bool pressed=false; };
enum class Panel { None, Launcher, Settings, Tray, Calendar };

class Scene {
public:
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1BitmapBrush> backdrop;
    ComPtr<ID2D1LinearGradientBrush> material, sheen;
    ComPtr<ID2D1PathGeometry> launcher_mark,media_play_mark;
    ComPtr<ID2D1Layer> panel_layer;
    ClickPulse launcher_pulse,tray_pulse,settings_pulse,clock_pulse;
    std::array<ClickPulse,3> media_pulses;
    MediaSnapshot media;
    MediaMarquee media_marquee;
    ComPtr<IDWriteTextLayout> media_title;
    std::wstring media_title_text;
    PanelReveal panel_reveal;
    struct IconBitmap { NativeIcon source;ComPtr<ID2D1Bitmap> bitmap; };
    std::vector<std::shared_ptr<IconBitmap>> app_icons, desktop_icons;
    std::unordered_map<void*,std::unordered_map<UINT,std::shared_ptr<IconBitmap>>> icon_cache;
    float cache_width=0,cache_height=0;
    std::array<ComPtr<IDWriteTextFormat>, 8> fonts;
    DockMachine machine;
    Inventory inventory;
    std::vector<Group> groups;
    BalancedApps placements;
    std::vector<size_t> results;
    Panel panel = Panel::None;
    float width = 1160, height = 740;
    float pointer_x = -1, pointer_y = -1;
    int focus = -1;
    bool reduced = false, high_contrast = false, transparency = true, initialized = false, pressed = false, desktop = false;
    std::wstring query;
    std::wstring recovery_entry;
    std::wstring clock_time,clock_date;

    void initialize() {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));
        check(factory->CreatePathGeometry(launcher_mark.put()));
        ComPtr<ID2D1GeometrySink> logo_sink;check(launcher_mark->Open(logo_sink.put()));
        logo_sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        for(size_t i=1;i<launcher_logo::offsets.size();++i) {
            const auto begin=launcher_logo::offsets[i-1],end=launcher_logo::offsets[i];
            logo_sink->BeginFigure(launcher_logo::points[begin],D2D1_FIGURE_BEGIN_FILLED);
            logo_sink->AddLines(launcher_logo::points.data()+begin+1,static_cast<UINT32>(end-begin-1));
            logo_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }
        check(logo_sink->Close());
        check(factory->CreatePathGeometry(media_play_mark.put()));
        ComPtr<ID2D1GeometrySink> play_sink;check(media_play_mark->Open(play_sink.put()));
        play_sink->BeginFigure(D2D1::Point2F(-3,-4.5f),D2D1_FIGURE_BEGIN_FILLED);
        play_sink->AddLine(D2D1::Point2F(4,0));play_sink->AddLine(D2D1::Point2F(-3,4.5f));
        play_sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(play_sink->Close());
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IDWriteFontCollection> collection;check(write->GetSystemFontCollection(collection.put()));
        const auto ui_family=theme::text_family(collection.get());
        const float sizes[] = {11, 13, 16, 28, 40, 18, 12, 12};
        for (size_t i = 0; i < fonts.size(); ++i) {
            const auto weight=i==5?DWRITE_FONT_WEIGHT_NORMAL:DWRITE_FONT_WEIGHT_LIGHT;
            check(write->CreateTextFormat(i==5?L"Segoe Fluent Icons":ui_family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-us", fonts[i].put()));
            fonts[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fonts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis; write->CreateEllipsisTrimmingSign(fonts[i].get(), ellipsis.put());
            fonts[i]->SetTrimming(&trimming, ellipsis.get());
        }
        initialized = true;
        update_clock();
    }
    void update_clock() {
        wchar_t time[64]{},date[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,nullptr,nullptr,time,64);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,nullptr,nullptr,date,64,nullptr);
        clock_time=time; clock_date=date;
    }
    void bind(ID2D1RenderTarget* render) {
        target.reset(); render->AddRef(); *target.put() = render;
        check(target->CreateSolidColorBrush(theme::primary(), brush.put()));
        const auto layer_size=target->GetSize();check(target->CreateLayer(&layer_size,panel_layer.put()));
        backdrop.reset(); material.reset(); sheen.reset(); cache_width=cache_height=0;
        icon_cache.clear();
        rebuild_icons();
    }
    std::shared_ptr<IconBitmap> bitmap_icon(const NativeIcon& icon,float dip_size=24) {
        if(!icon) return {};
        float dx=96,dy=96;target->GetDpi(&dx,&dy);
        const auto pixels=static_cast<UINT>(std::max(1.0f,std::round(dip_size*dx/96)));
        if(const auto found=icon_cache.find(icon.get());found!=icon_cache.end()) {
            if(const auto size=found->second.find(pixels);size!=found->second.end())return size->second;
        }
        auto result=std::make_shared<IconBitmap>();result->source=icon;
        ComPtr<IWICBitmap> bitmap;ComPtr<IWICBitmapScaler> scaled;ComPtr<IWICFormatConverter> converted;
        if(FAILED(wic->CreateBitmapFromHICON(static_cast<HICON>(icon.get()),bitmap.put())) || FAILED(wic->CreateFormatConverter(converted.put()))) return result;
        if(FAILED(wic->CreateBitmapScaler(scaled.put()))||FAILED(scaled->Initialize(bitmap.get(),pixels,pixels,WICBitmapInterpolationModeFant)))return {};
        if(SUCCEEDED(converted->Initialize(scaled.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) target->CreateBitmapFromWicBitmap(converted.get(),nullptr,result->bitmap.put());
        if(icon_cache.size()>=128)icon_cache.clear();
        icon_cache[icon.get()].emplace(pixels,result);
        return result;
    }
    void rebuild_icons() {
        app_icons.clear(); desktop_icons.clear();
        if(!target) return;
        for(const auto& g:groups) app_icons.push_back(bitmap_icon(g.icon));
        for(size_t i=0;i<std::min<size_t>(8,inventory.desktop.size());++i)desktop_icons.push_back(bitmap_icon(inventory.desktop[i].icon,34));
    }
    void background(ID2D1RenderTarget* rt,ID2D1SolidColorBrush* b) {
        rt->Clear(theme::background());
        const auto ellipse=[&](unsigned c,float x,float y,float rx,float ry) { b->SetColor(theme::rgb(c));rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(width*x,height*y),width*rx,height*ry),b); };
        ellipse(0x19232f,.77f,.12f,.47f,.65f); ellipse(0x243444,.93f,.38f,.38f,.55f);
        ellipse(0x30465a,1.02f,.70f,.34f,.51f); ellipse(0x496079,1.12f,.97f,.31f,.49f);
        ellipse(0x274f68,.17f,1.12f,.29f,.34f); ellipse(0x32637a,.12f,1.14f,.21f,.24f);
    }
    void cache_material() {
        if(cache_width==width&&cache_height==height&&material) return;
        if(!desktop) {
        const auto bw=static_cast<UINT>(std::max(1.0f,width/4)),bh=static_cast<UINT>(std::max(1.0f,height/4));
        ComPtr<IWICBitmap> image; ComPtr<ID2D1RenderTarget> offscreen; ComPtr<ID2D1SolidColorBrush> b;
        check(wic->CreateBitmap(bw,bh,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,image.put()));
        check(factory->CreateWicBitmapRenderTarget(image.get(),D2D1::RenderTargetProperties(),offscreen.put()));
        check(offscreen->CreateSolidColorBrush(theme::primary(),b.put()));
        offscreen->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(bw)/width,static_cast<float>(bh)/height));
        offscreen->BeginDraw(); background(offscreen.get(),b.get()); check(offscreen->EndDraw());
        ComPtr<IWICBitmapLock> lock; WICRect region{0,0,static_cast<INT>(bw),static_cast<INT>(bh)};
        check(image->Lock(&region,WICBitmapLockRead,lock.put()));
        UINT stride=0,bytes=0; BYTE* data=nullptr; lock->GetStride(&stride); lock->GetDataPointer(&bytes,&data);
        std::vector<BYTE> pixels(static_cast<size_t>(bw)*bh*4),temp(pixels.size());
        for(UINT y=0;y<bh;++y) std::copy_n(data+y*stride,bw*4,pixels.data()+static_cast<size_t>(y)*bw*4);
        // Two separable box passes approximate frosted blur. Cached on size changes only.
        for(int pass=0;pass<2;++pass) {
            for(UINT y=0;y<bh;++y) for(UINT x=0;x<bw;++x) for(UINT c=0;c<4;++c) {
                unsigned sum=0; for(int k=-3;k<=3;++k) { const auto xx=std::clamp(static_cast<int>(x)+k,0,static_cast<int>(bw)-1); sum+=pixels[(static_cast<size_t>(y)*bw+xx)*4+c]; }
                temp[(static_cast<size_t>(y)*bw+x)*4+c]=static_cast<BYTE>(sum/7);
            }
            for(UINT y=0;y<bh;++y) for(UINT x=0;x<bw;++x) for(UINT c=0;c<4;++c) {
                unsigned sum=0; for(int k=-3;k<=3;++k) { const auto yy=std::clamp(static_cast<int>(y)+k,0,static_cast<int>(bh)-1); sum+=temp[(static_cast<size_t>(yy)*bw+x)*4+c]; }
                pixels[(static_cast<size_t>(y)*bw+x)*4+c]=static_cast<BYTE>(sum/7);
            }
        }
        ComPtr<ID2D1Bitmap> bitmap;
        check(target->CreateBitmap(D2D1::SizeU(bw,bh),pixels.data(),bw*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),bitmap.put()));
        check(target->CreateBitmapBrush(bitmap.get(),backdrop.put()));
        backdrop->SetTransform(D2D1::Matrix3x2F::Scale(width/bw,height/bh));
        }
        const auto stops=theme::material_stops();
        ComPtr<ID2D1GradientStopCollection> collection; check(target->CreateGradientStopCollection(stops.data(),static_cast<UINT32>(stops.size()),collection.put()));
        check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,48)),collection.get(),material.put()));
        const auto highlights=theme::sheen_stops();
        check(target->CreateGradientStopCollection(highlights.data(),static_cast<UINT32>(highlights.size()),collection.put()));
        check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,24)),collection.get(),sheen.put()));
        cache_width=width;cache_height=height;
    }
    void glass(D2D1_RECT_F r,float radius,bool heavy=false) {
        if(high_contrast||!transparency) { rect(r,theme::rgb(0x111111),radius);outline(r,theme::rgb(0xffffff,high_contrast?1.0f:.3f),radius);return; }
        for(int i=6;i>0;--i) rect(D2D1::RectF(r.left-i*1.5f,r.top+i*.8f,r.right+i*1.5f,r.bottom+i*2.0f),theme::rgb(0x020202,.018f+i*.008f),radius+i*1.4f);
        if(!desktop&&backdrop) target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),backdrop.get());
        material->SetStartPoint(D2D1::Point2F(r.left,r.top));material->SetEndPoint(D2D1::Point2F(r.left,r.bottom));
        target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),material.get());
        if(heavy) rect(r,theme::rgb(0x080808,.23f),radius);
        sheen->SetStartPoint(D2D1::Point2F(r.left,r.top));sheen->SetEndPoint(D2D1::Point2F(r.left,r.top+(r.bottom-r.top)*.54f));
        target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),sheen.get());
        outline(r,theme::rgb(0xe6e6e6,.26f),radius);
        outline(D2D1::RectF(r.left+1.2f,r.top+1.2f,r.right-1.2f,r.bottom-1.2f),theme::rgb(0x080808,.42f),radius-1);
        line(r.left+radius,r.top+1,r.right-radius,r.top+1,theme::rgb(0xffffff,.46f));
        line(r.left+radius,r.bottom-1,r.right-radius,r.bottom-1,theme::rgb(0xc2c2c2,.13f));
    }
    void color(D2D1_COLOR_F c) { brush->SetColor(c); }
    void rect(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        color(c);
        if (radius) target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get()); else target->FillRectangle(r, brush.get());
    }
    void outline(D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float thickness=1) {
        color(c); target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get(), thickness);
    }
    void text(std::wstring_view label, D2D1_RECT_F r, size_t font, D2D1_COLOR_F c, bool center = false) {
        color(c); fonts[font]->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        target->DrawText(label.data(), static_cast<UINT32>(label.size()), fonts[font].get(), r, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float stroke=1) {
        color(c); target->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush.get(),stroke);
    }
    void media_symbol(size_t index,D2D1_RECT_F button,D2D1_COLOR_F ink) {
        const auto x=(button.left+button.right)/2,y=(button.top+button.bottom)/2;
        D2D1_MATRIX_3X2_F saved;target->GetTransform(&saved);
        target->SetTransform(D2D1::Matrix3x2F::Scale(1.5f,1.5f)*D2D1::Matrix3x2F::Translation(x,y)*saved);
        if(index==1) {
            if(media.playing){line(-2,-4,-2,4,ink,1.2f);line(2,-4,2,4,ink,1.2f);}
            else {color(ink);target->FillGeometry(media_play_mark.get(),brush.get());}
        } else {
            const float direction=index==0?-1.0f:1.0f;
            line(-3*direction,-4,2*direction,0,ink,.9f);
            line(2*direction,0,-3*direction,4,ink,.9f);
            line(5*direction,-4,5*direction,4,ink,.9f);
        }
        target->SetTransform(saved);
    }
    size_t shown_groups() const {
        return std::min(groups.size(),DockLayout::capacity(width-40));
    }
    bool overflow(size_t i) const { return groups.size()>shown_groups()&&i+1==shown_groups(); }
    bool tile_running(size_t i) const {
        if(overflow(i)) return std::any_of(groups.begin()+static_cast<std::ptrdiff_t>(i),groups.end(),[](const Group& g){return !g.windows.empty();});
        return !groups[i].windows.empty();
    }
    void balance(std::vector<size_t> previous_sides={}) {
        std::vector<DockApp> apps;
        for(size_t i=0;i<shown_groups();++i)apps.push_back({groups[i].pinned,tile_running(i)});
        if(previous_sides.empty()&&!placements.apps.empty())
            for(size_t i=0;i<apps.size();++i)previous_sides.push_back(i<placements.apps.size()?placements.apps[i].side:2);
        placements=balance_apps(apps,previous_sides);
    }
    DockLayout layout() const { return {width/2,height-34,width-40,shown_groups()}; }
    float app_x(size_t index) const { return layout().app_x(placements.apps[index]); }
    D2D1_RECT_F app_rect(size_t i) const { const auto x=app_x(i),y=layout().center_y;return D2D1::RectF(x-18,y-19,x+18,y+19); }
    D2D1_RECT_F launcher_rect() const { const auto y=layout().center_y;return D2D1::RectF(width/2-22,y-22,width/2+22,y+22); }
    D2D1_RECT_F dock() const {
        const auto l=layout();const auto p=machine.progress(),w=l.width(p),h=l.height(p);
        return D2D1::RectF(l.center-w/2,l.center_y-h/2,l.center+w/2,l.center_y+h/2);
    }
    D2D1_RECT_F media_rect() const { const auto l=layout();return D2D1::RectF(l.center-l.expanded_width()/2+12,l.center_y-18,l.center-l.expanded_width()/2+160,l.center_y+18); }
    D2D1_RECT_F media_button(size_t index) const {const auto r=media_rect();const auto x=(r.left+r.right)/2+(static_cast<float>(index)-1)*40;return D2D1::RectF(x-16,r.top+19,x+16,r.bottom+1);}
    void measure_media_title() {
        const auto label=media.available?media.title:(media.unavailable?L"Media unavailable":L"No media");
        if(media_title&&media_title_text==label)return;
        media_title_text=label;media_title.reset();
        check(write->CreateTextLayout(label.data(),static_cast<UINT32>(label.size()),fonts[6].get(),16384,17,media_title.put()));
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_NONE,0,0};media_title->SetTrimming(&trimming,nullptr);media_title->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DWRITE_TEXT_METRICS metrics{};check(media_title->GetMetrics(&metrics));
        const auto r=media_rect();media_marquee.configure(media.available?metrics.widthIncludingTrailingWhitespace-(r.right-r.left):0);
    }
    bool media_enabled(size_t index) const {return media.available&&(index==0?media.previous:index==1?media.toggle:media.next);}
    D2D1_RECT_F system_rect() const { const auto l=layout();return D2D1::RectF(l.center+l.expanded_width()/2-166-DockLayout::desktop_width,l.center_y-20,l.center+l.expanded_width()/2-10-DockLayout::desktop_width,l.center_y+20); }
    D2D1_RECT_F quick_settings_rect() const { const auto s=system_rect();const auto cy=layout().center_y;return D2D1::RectF(s.left+24,cy-19,s.left+77,cy+19); }
    D2D1_RECT_F clock_rect() const { const auto s=system_rect();const auto cy=layout().center_y;return D2D1::RectF(s.left+81,cy-19,s.right,cy+19); }
    D2D1_RECT_F desktop_button_rect() const {
        const auto r=dock();const auto cy=layout().center_y;
        return D2D1::RectF(r.right-4-DockLayout::desktop_width,cy-13,r.right-4,cy+13);
    }
    D2D1_RECT_F panel_rect() const {
        const float w=std::min(width-40,360.0f),bottom=dock().top-12+panel_reveal.offset();
        const float right=panel==Panel::Launcher?(width+w)/2:std::min(width-20,system_rect().right+8);
        return D2D1::RectF(right-w,bottom-348,right,bottom);
    }
    void clear_press(){pressed=false;for(auto& g:groups)g.pressed=false;}
    void press_at(float x,float y) {
        clear_press();if(!inside(dock(),x,y))return;
        if(inside(launcher_rect(),x,y)){pressed=true;return;}
        for(size_t i=0;i<shown_groups();++i)if(inside(app_rect(i),x,y)){groups[i].pressed=true;return;}
    }
    size_t active_pulses() const {
        size_t count=launcher_pulse.active()+tray_pulse.active()+settings_pulse.active()+clock_pulse.active();
        for(const auto& g:groups)if(g.pulse.active())++count;
        for(const auto& pulse:media_pulses)if(pulse.active())++count;
        return count;
    }
    bool feedback_moving() const{return active_pulses()!=0||panel_reveal.active();}
    void tick_feedback(DockMachine::Time time) {
        launcher_pulse.tick(time);tray_pulse.tick(time);settings_pulse.tick(time);clock_pulse.tick(time);panel_reveal.tick(time);
        for(auto& g:groups)g.pulse.tick(time);
        for(auto& pulse:media_pulses)pulse.tick(time);
    }
    void stop_feedback() {
        launcher_pulse.stop();tray_pulse.stop();settings_pulse.stop();clock_pulse.stop();panel_reveal.settle();
        for(auto& g:groups)g.pulse.stop();clear_press();
        for(auto& pulse:media_pulses)pulse.stop();
    }
    void bubble(float x,float y,const ClickPulse& pulse,float opacity=1) {
        if(!pulse.active())return;
        const auto radius=pulse.radius(),alpha=pulse.opacity()*opacity;
        const auto ellipse=D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius);
        color(theme::rgb(0xffffff,alpha));target->FillEllipse(ellipse,brush.get());
        color(theme::rgb(0xffffff,alpha*1.35f));target->DrawEllipse(ellipse,brush.get(),.8f);
    }
    D2D1_RECT_F desktop_item(size_t i) const {
        const float x = 30 + static_cast<float>(i / 4) * 112, y = 174 + static_cast<float>(i % 4) * 101;
        return D2D1::RectF(x,y,x+92,y+90);
    }
    bool accept(Inventory value, bool full) {
        if (full) inventory = std::move(value); else inventory.windows = std::move(value.windows);
        const auto previous=std::move(groups);groups.clear();
        for(const auto& pin:inventory.pinned) {
            if(std::any_of(groups.begin(),groups.end(),[&](const Group& g){return (!pin.app_id.empty()&&pin.app_id==g.app_id)||(!pin.target.empty()&&!g.executable.empty()&&_wcsicmp(g.executable.c_str(),pin.target.c_str())==0);}))continue;
            groups.push_back({pin.title,std::wstring(1,static_cast<wchar_t>(towupper(pin.title[0]))),pin.target,{},pin.path,pin.icon,true,pin.app_id});
        }
        for (const auto& window : inventory.windows) {
            auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
                if(!window.app_id.empty()&&!g.app_id.empty())return window.app_id==g.app_id;
                return !window.executable.empty()&&!g.executable.empty()&&_wcsicmp(g.executable.c_str(),window.executable.c_str())==0;
            });
            if (found == groups.end()) {
                std::wstring name = window.executable.empty() ? window.title : std::filesystem::path(window.executable).stem().wstring();
                groups.push_back({name, std::wstring(1, static_cast<wchar_t>(towupper(name[0]))), window.executable, {window.hwnd},window.executable,window.icon,false,window.app_id});
            } else { found->windows.push_back(window.hwnd); if(!found->icon) found->icon=window.icon; }
        }
        const auto same_identity=[](const Group& a,const Group& b) {
            if(!a.app_id.empty()&&!b.app_id.empty())return a.app_id==b.app_id;
            if(!a.executable.empty()&&!b.executable.empty())return _wcsicmp(a.executable.c_str(),b.executable.c_str())==0;
            if(!a.launch_path.empty()&&!b.launch_path.empty())return _wcsicmp(a.launch_path.c_str(),b.launch_path.c_str())==0;
            return !a.windows.empty()&&!b.windows.empty()&&a.windows.front()==b.windows.front();
        };
        const auto previous_rank=[&](const Group& g) {
            for(size_t i=0;i<previous.size();++i)if(same_identity(g,previous[i]))return i;
            return previous.size();
        };
        // EnumWindows follows foreground Z order. Keep established app order instead.
        std::stable_sort(groups.begin(),groups.end(),[&](const Group& a,const Group& b){
            if(a.pinned!=b.pinned)return a.pinned;
            return previous_rank(a)<previous_rank(b);
        });
        for(auto& g:groups)std::sort(g.windows.begin(),g.windows.end(),std::less<HWND>{});
        for(auto& g:groups) {
            const auto rank=previous_rank(g);
            if(rank<previous.size()){g.pulse=previous[rank].pulse;g.pressed=previous[rank].pressed;}
        }
        const bool changed=full||groups.size()!=previous.size()||!std::equal(groups.begin(),groups.end(),previous.begin(),[](const Group& a,const Group& b){
            return a.name==b.name&&a.executable==b.executable&&a.app_id==b.app_id&&a.windows==b.windows&&a.launch_path==b.launch_path&&a.pinned==b.pinned&&a.icon.get()==b.icon.get();
        });
        if(!changed)return false;
        std::vector<size_t> previous_sides;
        for(size_t i=0;i<shown_groups();++i) {
            const auto rank=previous_rank(groups[i]);
            previous_sides.push_back(rank<placements.apps.size()?placements.apps[rank].side:2);
        }
        balance(std::move(previous_sides));
        if(full)filter();
        rebuild_icons();
        return true;
    }
    void filter() {
        results.clear(); auto needle=query; std::transform(needle.begin(),needle.end(),needle.begin(),towlower);
        for (size_t i=0; i<inventory.launcher.size(); ++i) {
            auto name=inventory.launcher[i].title; std::transform(name.begin(),name.end(),name.begin(),towlower);
            if (name.find(needle) != std::wstring::npos) results.push_back(i);
        }
    }
    void launcher_symbol(float cx, float cy, float scale = 1) {
        D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
        const auto size=theme::launcher_icon*scale;
        target->SetTransform(D2D1::Matrix3x2F::Scale(size/24,size/24)*
            D2D1::Matrix3x2F::Translation(cx-size/2,cy-size/2)*previous);
        color(theme::primary());target->FillGeometry(launcher_mark.get(),brush.get());
        target->SetTransform(previous);
    }
    void draw() {
        if(transparency&&!high_contrast) cache_material();
        target->BeginDraw();
        if(desktop) target->Clear(D2D1::ColorF(0,0,0,0));
        else {
        if(!high_contrast) background(target.get(),brush.get());else target->Clear(D2D1::ColorF(0,0,0));
        text(L"MINIMA / DOCK",D2D1::RectF(34,24,300,47),0,theme::secondary());
        text(L"Fixed center. Balanced wings.",D2D1::RectF(32,54,width-32,108),4,theme::primary());
        text(L"24 DIP sword mark  ·  44 DIP button  ·  44 DIP glass pill",D2D1::RectF(34,110,width-32,138),1,theme::secondary());
        rect(D2D1::RectF(width-184,27,width-32,54),theme::rgb(0xffffff,.055f),13);
        text(L"DESIGN  /  02",D2D1::RectF(width-184,27,width-32,54),0,theme::secondary(),true);

        const size_t desktop_count = std::min<size_t>(inventory.desktop.size(),8);
        for (size_t i=0;i<desktop_count;++i) {
            const auto r=desktop_item(i); const bool over=inside(r,pointer_x,pointer_y);
            const float lift=over&&!reduced?3.0f:0.0f;
            const auto icon_rect=D2D1::RectF(r.left+24,r.top+12-lift,r.left+68,r.top+56-lift);
            if(over) rect(r,theme::rgb(0xffffff,.045f),14);
            if(i<desktop_icons.size()&&desktop_icons[i]&&desktop_icons[i]->bitmap) target->DrawBitmap(desktop_icons[i]->bitmap.get(),D2D1::RectF(r.left+29,r.top+15-lift,r.left+63,r.top+49-lift));
            else { rect(icon_rect,theme::rgb(inventory.desktop[i].folder?0xb5c6db:0x6987ac,.85f),11); text(L"↗",icon_rect,3,theme::primary(),true); }
            if(over) text(inventory.desktop[i].title,D2D1::RectF(r.left,r.top+58,r.right,r.bottom),0,theme::primary(),true);
        }
        if(desktop_count==0) text(L"Desktop shortcuts will appear here",D2D1::RectF(34,184,340,211),1,theme::secondary());
        text(L"HOVER TO EXPAND",D2D1::RectF(width-258,height-203,width-32,height-181),0,theme::secondary());
        text(L"Launcher stays at the center.",D2D1::RectF(width-258,height-178,width-32,height-150),1,theme::primary());
        text(L"F: fullscreen simulation  ·  R: reduced motion  ·  Tab / arrows: dock focus",D2D1::RectF(34,height-128,width-32,height-107),0,theme::secondary());
        }

        if (machine.visible()) {
            const auto r=dock(); const auto p=machine.progress();
            glass(r,(r.bottom-r.top)/2);
            const auto cy=layout().center_y;
            const bool launcher_over=inside(launcher_rect(),pointer_x,pointer_y);
            if(launcher_over)
                rect(D2D1::RectF(width/2-18,cy-19,width/2+18,cy+19),theme::rgb(0xffffff,.06f),16);
            bubble(width/2,cy,launcher_pulse);
            if(focus==0) line(width/2-4,cy+17,width/2+4,cy+17,theme::accent(),1.5f);
            const float launcher_lift=launcher_over&&!reduced?2.0f:0.0f;
            launcher_symbol(width/2,cy-launcher_lift,pressed?.95f:1.0f);
            constexpr unsigned colors[]={0x758bbb,0x618e7f,0xb39b78,0x9290b9};
            target->PushAxisAlignedClip(D2D1::RectF(r.left+3,r.top,r.right-3,r.bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for(size_t i=0;i<shown_groups();++i) {
                constexpr float opacity=1;
                const auto button=app_rect(i);const auto x=app_x(i);
                const bool over=inside(button,pointer_x,pointer_y);
                if(over) rect(button,theme::rgb(0xffffff,.06f),16);
                bubble(x,cy,groups[i].pulse,opacity);
                const float lift=over&&!reduced?2.0f:0.0f;
                const float half=groups[i].pressed?11.28f:12.0f;
                float dx=96,dy=96;target->GetDpi(&dx,&dy);
                const auto snap_x=[&](float value){return std::round(value*dx/96)*96/dx;};
                const auto snap_y=[&](float value){return std::round(value*dy/96)*96/dy;};
                const auto icon_rect=D2D1::RectF(snap_x(x-half),snap_y(cy-half-lift),snap_x(x+half),snap_y(cy+half-lift));
                if(overflow(i)) text(L"···",icon_rect,2,theme::rgb(0xf2f4fa,opacity),true);
                else if(i<app_icons.size()&&app_icons[i]&&app_icons[i]->bitmap) target->DrawBitmap(app_icons[i]->bitmap.get(),icon_rect,opacity);
                else {rect(icon_rect,theme::rgb(colors[i%4],opacity),7);text(groups[i].initial,icon_rect,1,theme::rgb(0xf2f4fa,opacity),true);}
                if(tile_running(i)) { rect(D2D1::RectF(x-4,cy+17,x+4,cy+19),theme::accent(),1);if(groups[i].windows.size()>1)rect(D2D1::RectF(x+6,cy+17,x+8,cy+19),theme::secondary(),1); }
                if(focus==static_cast<int>(i)+1) outline(button,theme::accent(),14,1.5f);
            }
            if(p>.01f) {
                const auto music=media_rect(),system=system_rect();
                const auto quick_settings=quick_settings_rect();
                const bool tray_over=inside(D2D1::RectF(system.left,cy-19,system.left+24,cy+19),pointer_x,pointer_y);
                const bool settings_over=inside(quick_settings,pointer_x,pointer_y);
                if(tray_over)rect(D2D1::RectF(system.left-7,cy-19,system.left+29,cy+19),theme::rgb(0xffffff,.06f*p),16);
                if(settings_over)rect(quick_settings,theme::rgb(0xffffff,.06f*p),16);
                bubble(system.left+11,cy,tray_pulse,p);
                bubble((quick_settings.left+quick_settings.right)/2,cy,settings_pulse,p);
                const wchar_t* system_symbols[]={L"\xE70E",L"\xE701",L"\xE767"};
                constexpr float icon_offsets[]={0,27,52};
                for(size_t i=0;i<3;++i) {
                    const bool over=i==0?tray_over:settings_over;
                    const float x=system.left+icon_offsets[i]+11;
                    const float lift=over&&!reduced?2.0f:0.0f;
                    text(system_symbols[i],D2D1::RectF(x-11,cy-17-lift,x+11,cy+17-lift),5,theme::rgb(0xe1e9f0,p),true);
                }
                line(music.right+6,cy-12,music.right+6,cy+12,theme::rgb(0xffffff,.13f*p));
                measure_media_title();
                const auto title_area=D2D1::RectF(music.left,music.top,music.right,music.top+17);
                if(reduced)text(media_title_text,title_area,6,theme::rgb(0xf2f4fa,p));
                else {target->PushAxisAlignedClip(title_area,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);color(theme::rgb(0xf2f4fa,p));target->DrawTextLayout(D2D1::Point2F(music.left-media_marquee.offset(),music.top),media_title.get(),brush.get());target->PopAxisAlignedClip();}
                for(size_t i=0;i<3;++i) {
                    auto button=media_button(i);const bool enabled=media_enabled(i),over=enabled&&inside(button,pointer_x,pointer_y);
                    bubble((button.left+button.right)/2,(button.top+button.bottom)/2,media_pulses[i],p*.45f);
                    const auto lift=over&&!reduced?1.0f:0.0f;button.top-=lift;button.bottom-=lift;
                    media_symbol(i,button,theme::rgb(0xf2f2f2,p*(enabled?(over||high_contrast?1.0f:.82f):(high_contrast?.50f:.28f))));
                    if(focus==static_cast<int>(shown_groups())+5+static_cast<int>(i))outline(media_button(i),theme::accent(),8,1.5f);
                }
                line(system.left-6,cy-12,system.left-6,cy+12,theme::rgb(0xffffff,.13f*p));
                const auto clock=clock_rect();const bool clock_over=inside(clock,pointer_x,pointer_y);
                if(clock_over)rect(clock,theme::rgb(0xffffff,.06f*p),16);
                bubble((clock.left+clock.right)/2,cy,clock_pulse,p);
                const float clock_lift=clock_over&&!reduced?2.0f:0.0f;
                text(clock_time,D2D1::RectF(clock.left,cy-17-clock_lift,clock.right,cy+1-clock_lift),6,theme::rgb(0xf2f4fa,p),true);
                text(clock_date,D2D1::RectF(clock.left,cy+1-clock_lift,clock.right,cy+18-clock_lift),7,theme::rgb(0xf2f4fa,p),true);
                if(focus==static_cast<int>(shown_groups())+1)outline(D2D1::RectF(system.left,system.top,system.left+23,system.bottom),theme::accent(),10,1.5f);
                if(focus==static_cast<int>(shown_groups())+2)outline(quick_settings,theme::accent(),14,1.5f);
                if(focus==static_cast<int>(shown_groups())+3)outline(clock,theme::accent(),14,1.5f);
            }
            const auto show_desktop=desktop_button_rect();
            const bool desktop_over=inside(show_desktop,pointer_x,pointer_y);
            const bool desktop_focused=focus==static_cast<int>(shown_groups())+4;
            // A slim separator at the outer edge, like the native taskbar.
            // Hover brightens the edge instead of drawing a floating button box.
            const auto separator=show_desktop.left+3;
            line(separator,show_desktop.top,separator,show_desktop.bottom,
                desktop_focused?theme::accent():theme::rgb(0xe6e6e6,desktop_over?1.0f:.4f),desktop_over||desktop_focused?2.0f:1.0f);
            target->PopAxisAlignedClip();
            if(desktop_over||desktop_focused) {
                const auto tooltip=D2D1::RectF(r.right-112,cy-52,r.right,cy-26);
                rect(tooltip,theme::rgb(0x202020,.97f),5);
                outline(tooltip,theme::rgb(0xffffff,.14f),5);
                text(L"Show desktop",tooltip,0,theme::primary(),true);
            }
            const auto media_area=media_rect();
            if(media.available&&p>.85f)for(size_t i=0;i<3;++i)if(!media_enabled(i)&&inside(media_button(i),pointer_x,pointer_y)) {
                const auto tooltip=D2D1::RectF(media_area.left,cy-52,std::min(width-20,media_area.left+300),cy-26);
                const wchar_t* unavailable[]={L"Previous track unavailable in this player",L"Playback control unavailable in this player",L"Next track unavailable in this player"};
                rect(tooltip,theme::rgb(0x202020,.97f),5);outline(tooltip,theme::rgb(0xffffff,.14f),5);text(unavailable[i],D2D1::RectF(tooltip.left+8,tooltip.top,tooltip.right-8,tooltip.bottom),6,theme::primary());
            }
            if(media.available&&media.toggle&&p>.85f&&inside(media_button(1),pointer_x,pointer_y)) {
                const auto center=(media_button(1).left+media_button(1).right)/2;
                const auto tooltip=D2D1::RectF(center-28,cy-52,center+28,cy-26);
                rect(tooltip,theme::rgb(0x202020,.97f),5);outline(tooltip,theme::rgb(0xffffff,.14f),5);text(media.playing?L"Pause":L"Play",tooltip,6,theme::primary(),true);
            }
            if(media.available&&!media.artist.empty()&&p>.85f&&inside(D2D1::RectF(media_area.left,media_area.top,media_area.right,media_area.top+17),pointer_x,pointer_y)) {
                const auto tooltip=D2D1::RectF(media_area.left,cy-52,std::min(width-20,media_area.left+360),cy-26);
                rect(tooltip,theme::rgb(0x202020,.97f),5);outline(tooltip,theme::rgb(0xffffff,.14f),5);text(media.artist,D2D1::RectF(tooltip.left+8,tooltip.top,tooltip.right-8,tooltip.bottom),6,theme::primary());
            }
        } else if(!desktop) text(L"Overlay suppressed — press F to return",D2D1::RectF(0,height-100,width,height-50),2,theme::secondary(),true);

        if(panel != Panel::None && panel != Panel::Calendar && machine.visible()) {
            const auto r=panel_rect();
            const bool fading=panel_reveal.active();
            if(fading)target->PushLayer(D2D1::LayerParameters(
                D2D1::RectF(r.left-12,r.top-12,r.right+12,r.bottom+18),nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),panel_reveal.opacity()),panel_layer.get());
            glass(r,20,true);
            if(panel==Panel::Launcher) {
                text(L"Your applications",D2D1::RectF(r.left+24,r.top+14,r.right-24,r.top+49),2,theme::primary());
                rect(D2D1::RectF(r.left+20,r.top+57,r.right-20,r.top+96),theme::rgb(0x141414),10);
                if(query.empty()) text(L"Search installed shortcuts…",D2D1::RectF(r.left+32,r.top+57,r.right-32,r.top+96),1,theme::secondary());
                else if(desktop) text(query,D2D1::RectF(r.left+32,r.top+57,r.right-32,r.top+96),1,theme::primary());
                const size_t count=std::min<size_t>(results.size(),5);
                for(size_t i=0;i<count;++i) {
                    const auto row=D2D1::RectF(r.left+16,r.top+110+static_cast<float>(i)*42,r.right-16,r.top+150+static_cast<float>(i)*42);
                    if(inside(row,pointer_x,pointer_y)) rect(row,theme::rgb(0xffffff,.065f),10);
                    text(L"↗",D2D1::RectF(row.left+10,row.top,row.left+34,row.bottom),2,theme::accent(),true);
                    text(inventory.launcher[results[i]].title,D2D1::RectF(row.left+44,row.top,row.right-8,row.bottom),1,theme::primary());
                }
                if(count==0) text(L"No matching shortcuts",D2D1::RectF(r.left+24,r.top+110,r.right-24,r.top+154),1,theme::secondary());
                text(L"Enter opens the first result  ·  Esc closes",D2D1::RectF(r.left+24,r.bottom-30,r.right-24,r.bottom-8),0,theme::secondary());
            } else if(panel==Panel::Settings) {
                text(L"Quick settings",D2D1::RectF(r.left+20,r.top+12,r.right-20,r.top+40),2,theme::primary());
                const wchar_t* labels[]={L"Wi-Fi",L"Bluetooth",L"Airplane mode",L"Accessibility",L"Energy saver",L"Live captions"};
                const wchar_t* symbols[]={L"\xE701",L"\xE702",L"\xE709",L"\xE776",L"\xE945",L"\xE8F2"};
                for(size_t i=0;i<6;++i) {
                    const float x=r.left+20+static_cast<float>(i%3)*109,y=r.top+56+static_cast<float>(i/3)*85;
                    const auto tile=D2D1::RectF(x,y,x+100,y+48);
                    rect(tile,theme::rgb(0xc5def4,inside(tile,pointer_x,pointer_y)?.17f:.09f),10);
                    outline(tile,theme::rgb(0xe5f2ff,.16f),10);
                    text(symbols[i],D2D1::RectF(x+9,y,x+58,y+48),5,theme::primary(),true);
                    if(i<2) {line(x+67,y+10,x+67,y+38,theme::rgb(0xffffff,.12f));text(L"\xE76C",D2D1::RectF(x+69,y,x+100,y+48),5,theme::primary(),true);}
                    text(labels[i],D2D1::RectF(x-2,y+51,x+102,y+72),0,theme::primary(),true);
                }
                line(r.left+20,r.top+231,r.right-20,r.top+231,theme::rgb(0xffffff,.09f));
                text(L"\xE767",D2D1::RectF(r.left+20,r.top+245,r.left+47,r.top+277),5,theme::secondary(),true);
                const auto a=r.left+60,b=r.right-58,y=r.top+262;
                rect(D2D1::RectF(a,y-2,b,y+2),theme::rgb(0xa6bdd1,.16f),2);
                color(theme::rgb(0x99aab9));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(a+8,y),5,5),brush.get());
                text(L"\xE76C",D2D1::RectF(r.right-46,r.top+245,r.right-18,r.top+277),5,theme::primary(),true);
                text(L"Volume unavailable in preview",D2D1::RectF(r.left+60,r.top+277,r.right-20,r.top+297),0,theme::secondary());
                line(r.left+1,r.bottom-40,r.right-1,r.bottom-40,theme::rgb(0xffffff,.10f));
                text(desktop?L"Revert taskbar · Ctrl+Alt+Backspace":(recovery_entry.empty()?L"Recovery bundle not prepared":L"Restore Windows desktop"),D2D1::RectF(r.left+20,r.bottom-33,r.right-55,r.bottom-8),0,theme::primary());
                text(L"\xE713",D2D1::RectF(r.right-45,r.bottom-36,r.right-16,r.bottom-5),5,theme::primary(),true);
            } else {
                text(L"System tray",D2D1::RectF(r.left+20,r.top+15,r.right-20,r.top+44),2,theme::primary());
                text(L"Third-party tray icons are unavailable in this preview.",D2D1::RectF(r.left+20,r.top+54,r.right-20,r.top+84),0,theme::secondary());
                text(desktop?L"Revert to access the Windows tray.":L"Your Windows taskbar still hosts them.",D2D1::RectF(r.left+20,r.top+88,r.right-20,r.top+115),1,theme::primary());
                rect(D2D1::RectF(r.left+20,r.top+137,r.right-20,r.top+185),theme::rgb(0xffffff,.07f),12);
                text(L"Open notification settings  ↗",D2D1::RectF(r.left+34,r.top+137,r.right-34,r.top+185),1,theme::primary());
                if(groups.size()>shown_groups()) {
                    rect(D2D1::RectF(r.left+20,r.top+198,r.right-20,r.top+246),theme::rgb(0xffffff,.07f),12);
                    text(L"More applications  ↗",D2D1::RectF(r.left+34,r.top+198,r.right-34,r.top+246),1,theme::primary());
                }
            }
            if(fading)target->PopLayer();
        }
    }
};

class Preview {
public:
    Scene scene;
    CalendarPopup calendar;
    MediaController media;
    DWORD calendar_opened=0;
    HWND calendar_previous=nullptr;
    HWND hwnd=nullptr, search=nullptr;
    ComPtr<ID2D1HwndRenderTarget> render;
    ComPtr<IWICBitmap> layer_bitmap;
    ComPtr<ID2D1RenderTarget> layer_render;
    HDC layer_dc=nullptr;
    HBITMAP layer_dib=nullptr;
    void* layer_pixels=nullptr;
    HGDIOBJ layer_previous=nullptr;
    UINT layer_width=0,layer_height=0;
    RECT pending_layer_bounds{};
    bool layer_bounds_pending=false,presenting_layer=false;
    UINT presented_height=0,layer_allocations=0,layer_frames=0;
    LONG presented_center_y=0;
    bool layer_presented=false,inventory_loaded=false;
    std::jthread worker;
    std::mutex mutex;
    std::unique_ptr<Inventory> pending;
    std::atomic_bool busy=false;
    bool pending_full=false, rescan=false, tracking=false, animation_timer=false, reconcile_scheduled=false, media_timer=false;
    std::array<HWINEVENTHOOK,5> hooks{};
    DockVisibility visibility;
    HWND edge=nullptr,last_foreground=nullptr;
    int edge_indicator_alpha=-1;
    bool live_dock=false,auto_hidden=false,edge_tracking=false,reveal_hotkey=false;
    HFONT search_font=nullptr;
    HBRUSH edit_background=nullptr;
    float dpi=96;
    HRESULT desktop_action_result=E_PENDING;
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto self=reinterpret_cast<Preview*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) { self=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->hwnd=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if(!self) return DefWindowProcW(window,message,w,l);
        try { return self->message(message,w,l); }
        catch(HRESULT error) {
            if(self->scene.desktop) {
                wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
                std::ofstream(std::filesystem::path(path).parent_path()/L"dock-error.txt")<<"message="<<message<<" HRESULT="<<std::hex<<static_cast<unsigned long>(error)<<'\n';
                PostMessageW(window,WM_CLOSE,0,0);
            }
            return DefWindowProcW(window,message,w,l);
        }
        catch(...) { MessageBoxW(window,L"Preview operation failed. Windows remains active.",L"MinimaDock",MB_OK|MB_ICONERROR); return 0; }
    }
    std::int64_t now() const { return static_cast<std::int64_t>(GetTickCount64()); }
    static LRESULT CALLBACK edge_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Preview*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,msg,w,l);
        if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(msg==WM_MOUSEMOVE&&self->auto_hidden&&self->scene.machine.visible()) {
            if(!self->edge_tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window,0};TrackMouseEvent(&t);self->edge_tracking=true;}
            self->scene.machine.hold(Hold::Pointer,true,self->now());self->schedule();return 0;
        }
        if(msg==WM_MOUSELEAVE){self->edge_tracking=false;self->pointer_hold();self->schedule();return 0;}
        return DefWindowProcW(window,msg,w,l);
    }
    void create_edge() {
        WNDCLASSW klass{};klass.lpfnWndProc=edge_proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.RevealEdge";
        klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw E_FAIL;
        edge=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,klass.lpszClassName,L"MinimaDock reveal edge",WS_POPUP,0,0,1,1,hwnd,nullptr,klass.hInstance,this);
        if(!edge)throw E_FAIL;
        reveal_hotkey=RegisterHotKey(hwnd,3,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_SPACE)!=FALSE;
    }
    void present_edge(int x,int y,int width,int height,int indicator_height,int alpha) {
        // Nonzero alpha retains a continuous, non-activating pointer bridge.
        // The neutral rounded indicator fades away as the pill reveals.
        BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=width;
        bitmap.bmiHeader.biHeight=-height;bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
        auto dc=CreateCompatibleDC(nullptr);void* data=nullptr;
        auto dib=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&data,nullptr,0);
        if(!dc||!dib||!data){if(dib)DeleteObject(dib);if(dc)DeleteDC(dc);throw E_OUTOFMEMORY;}
        auto previous=SelectObject(dc,dib);auto pixels=static_cast<BYTE*>(data);
        for(int row=0;row<height;++row)for(int col=0;col<width;++col) {
            const auto index=(static_cast<size_t>(row)*width+col)*4;
            BYTE a=1,color=0;
            if(row>=height-indicator_height) {
                const float radius=indicator_height*.5f,cy=row-(height-indicator_height)+.5f-radius;
                const float cx=col+.5f-std::clamp(col+.5f,radius,width-radius);
                const float coverage=std::clamp(radius+.5f-std::sqrt(cx*cx+cy*cy),0.0f,1.0f);
                a=static_cast<BYTE>(std::max(1L,std::lround(alpha*coverage)));
                const int shade=row==height-indicator_height?210:150;
                color=static_cast<BYTE>(shade*a/255);
            }
            pixels[index]=pixels[index+1]=pixels[index+2]=color;pixels[index+3]=a;
        }
        POINT destination{x,y},source{0,0};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        const bool updated=UpdateLayeredWindow(edge,nullptr,&destination,&size,dc,&source,0,&blend,ULW_ALPHA)!=FALSE;
        SelectObject(dc,previous);DeleteObject(dib);DeleteDC(dc);
        if(!updated)throw E_FAIL;
        edge_indicator_alpha=alpha;
    }
    void pointer_hold() {
        POINT p{};bool over=false;
        if(GetCursorPos(&p)&&ScreenToClient(hwnd,&p)) {
            const float x=p.x*96.0f/dpi,y=p.y*96.0f/dpi;
            const auto r=scene.dock();
            over=visibility.wanted()&&(inside(r,x,y)||inside(r,x,y-visibility.offset())||(scene.panel!=Panel::None&&scene.panel!=Panel::Calendar&&inside(scene.panel_rect(),x,y)));
            scene.pointer_x=x;scene.pointer_y=y-visibility.offset();
        }
        scene.machine.hold(Hold::Pointer,over||edge_tracking,now());
    }
    void sync_visibility() {
        if(!live_dock)return;
        visibility.update(auto_hidden,!scene.machine.visible(),scene.machine.holds()!=0,now());
        auto style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
        const auto wanted=visibility.wanted()?style&~WS_EX_TRANSPARENT:style|WS_EX_TRANSPARENT;
        if(style!=wanted)SetWindowLongPtrW(hwnd,GWL_EXSTYLE,wanted);
        if(edge) {
            MONITORINFO info{sizeof(info),{},{},0};
            if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY),&info)) {
                const auto pill=scene.dock();POINT origin{0,0};
                if(layer_bounds_pending)origin.x=pending_layer_bounds.left;else ClientToScreen(hwnd,&origin);
                const auto x=static_cast<int>(origin.x)+static_cast<int>(std::lround(pill.left*dpi/96));
                const auto width=std::max(1,static_cast<int>(origin.x)+static_cast<int>(std::lround(pill.right*dpi/96))-x);
                const auto indicator_height=std::max(2,static_cast<int>(std::lround(3*dpi/96)));
                const auto height=visibility.wanted()?std::max(indicator_height,static_cast<int>(std::ceil(14*dpi/96))):indicator_height;
                const auto y=info.rcMonitor.bottom-height;
                const auto alpha=static_cast<int>(std::lround(180*(1-visibility.opacity())));
                RECT current{};GetWindowRect(edge,&current);
                if(current.left!=x||current.top!=y||current.right-current.left!=width||current.bottom-current.top!=height||edge_indicator_alpha!=alpha)
                    present_edge(x,y,width,height,indicator_height,alpha);
                const bool shown=auto_hidden&&scene.machine.visible();
                if((IsWindowVisible(edge)!=FALSE)!=shown)ShowWindow(edge,shown?SW_SHOWNOACTIVATE:SW_HIDE);
            }
        }
    }
    void release_layer() {
        layer_render.reset();layer_bitmap.reset();layer_presented=false;
        if(layer_dc&&layer_previous) SelectObject(layer_dc,layer_previous);
        if(layer_dib) DeleteObject(layer_dib);
        if(layer_dc) DeleteDC(layer_dc);
        layer_dc=nullptr;layer_dib=nullptr;layer_previous=nullptr;layer_pixels=nullptr;
    }
    void desktop_position() {
        POINT origin{0,0};const auto monitor=MonitorFromPoint(origin,MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info),{},{},0};if(!GetMonitorInfoW(monitor,&info)) throw E_FAIL;
        const float scale=dpi/96;
        const auto desired=DockLayout::natural_expanded_width(scene.groups.size())+40;
        const auto w=std::min(info.rcMonitor.right-info.rcMonitor.left,static_cast<LONG>(std::ceil(desired*scale)));
        const auto h=std::min(info.rcMonitor.bottom-info.rcMonitor.top,static_cast<LONG>((scene.panel==Panel::None||scene.panel==Panel::Calendar?88:440)*scale));
        // Keep the currently displayed bitmap in place until its replacement is
        // ready. UpdateLayeredWindow commits destination, size and pixels together.
        const auto left=info.rcMonitor.left+(info.rcMonitor.right-info.rcMonitor.left-w)/2;
        pending_layer_bounds={left,info.rcMonitor.bottom-h,left+w,info.rcMonitor.bottom};layer_bounds_pending=true;
        scene.width=w/scale;scene.height=h/scale;scene.balance();
        if(tracking) {
            POINT pointer{};
            if(GetCursorPos(&pointer)) {
                scene.pointer_x=(pointer.x-left)/scale;
                scene.pointer_y=(pointer.y-pending_layer_bounds.top)/scale-(live_dock?visibility.offset():0);
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void fullscreen() {
        if(!live_dock)return;
        auto foreground=GetForegroundWindow();RECT bounds{};MONITORINFO info{sizeof(info),{},{},0};
        DWORD owner=0;GetWindowThreadProcessId(foreground,&owner);
        // Activation can briefly report no foreground while an owned popup closes.
        // Retain the application used for visibility and focus return through it.
        if(!foreground||owner==GetCurrentProcessId())foreground=last_foreground;else last_foreground=foreground;
        owner=0;GetWindowThreadProcessId(foreground,&owner);
        const auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY);
        wchar_t klass[128]{};GetClassNameW(foreground,klass,128);
        const bool desktop=wcscmp(klass,L"Progman")==0||wcscmp(klass,L"WorkerW")==0;
        const bool covers=foreground&&owner!=GetCurrentProcessId()&&!desktop&&!IsIconic(foreground)&&!IsZoomed(foreground)&&
            MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY)==monitor&&GetWindowRect(foreground,&bounds)&&GetMonitorInfoW(monitor,&info)&&
            bounds.left<=info.rcMonitor.left&&bounds.top<=info.rcMonitor.top&&bounds.right>=info.rcMonitor.right&&bounds.bottom>=info.rcMonitor.bottom;
        if(covers) {calendar.close(false);set_panel(Panel::None);scene.stop_feedback();}
        auto_hidden=foreground&&!desktop&&IsWindowVisible(foreground)&&!IsIconic(foreground)&&
            (IsZoomed(foreground)||window_arranged(foreground))&&MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY)==monitor;
        scene.machine.suppress(covers,now());
        sync_visibility();
    }
    void desktop_menu() {
        auto menu=CreatePopupMenu();
        AppendMenuW(menu,MF_STRING,1,L"Keep this test running");
        AppendMenuW(menu,MF_STRING,2,L"Revert to Windows taskbar  ·  Ctrl+Alt+Backspace");
        AppendMenuW(menu,MF_STRING,3,L"Quick settings");
        scene.machine.hold(Hold::ContextMenu,true,now());
        POINT point{};GetCursorPos(&point);SetForegroundWindow(hwnd);
        const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,hwnd,nullptr);
        DestroyMenu(menu);scene.machine.hold(Hold::ContextMenu,false,now());
        if(GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
        if(selected==2)PostMessageW(hwnd,WM_CLOSE,0,0);
        else if(selected==1) {
            wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
            const auto path=(std::filesystem::path(module).parent_path()/L"MinimaDockTrial.exe").wstring();
            ShellExecuteW(hwnd,L"open",path.c_str(),L"--keep",nullptr,SW_HIDE);
        } else if(selected==3)set_panel(Panel::Settings);
        schedule();
    }
    void schedule() {
        sync_visibility();
        scene.measure_media_title();
        scene.media_marquee.visible(scene.machine.state()==DockState::Expanded&&visibility.progress()>.99f&&scene.panel==Panel::None,scene.reduced,now());
        if(scene.media_marquee.active()&&!media_timer)media_timer=SetTimer(hwnd,media_title_timer,33,nullptr)!=0;
        else if(!scene.media_marquee.active()&&media_timer){KillTimer(hwnd,media_title_timer);media_timer=false;}
        const bool moving=scene.machine.needs_tick()||visibility.needs_tick()||scene.feedback_moving();
        if(moving&&!animation_timer) animation_timer=SetTimer(hwnd,frame_timer,10,nullptr)!=0;
        else if(!moving&&animation_timer) { KillTimer(hwnd,frame_timer); animation_timer=false; }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void update_preferences() {
        BOOL animations=TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast); SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        scene.reduced=!animations; scene.high_contrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        scene.machine.reduced_motion(scene.reduced);
        if(scene.reduced)scene.stop_feedback();
        visibility.reduced_motion(scene.reduced);
        calendar.preferences(scene.reduced);
        DWORD enabled=1,size=sizeof(enabled);
        if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&enabled,&size)==ERROR_SUCCESS)scene.transparency=enabled!=0;
    }
    void scan(bool full) {
        if(busy.exchange(true)) { rescan=true; return; }
        if(worker.joinable()) worker.join();
        worker=std::jthread([this,full](std::stop_token stop) {
            const auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            try {
                auto data=std::make_unique<Inventory>();
                if(full) *data=collect_inventory(GetCurrentProcessId()); else data->windows=collect_windows(GetCurrentProcessId());
                if(!stop.stop_requested()) { std::lock_guard lock(mutex); pending=std::move(data); pending_full=full; PostMessageW(hwnd,inventory_ready,0,0); }
            } catch(...) {}
            if(SUCCEEDED(hr)) CoUninitialize(); busy=false;
        });
    }
    void set_panel(Panel p) {
        const bool changed=scene.panel!=p;
        if(changed&&p==Panel::Calendar) {
            calendar_opened=GetTickCount();const auto foreground=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
            calendar_previous=foreground&&pid!=GetCurrentProcessId()?foreground:last_foreground;
        }
        scene.panel=p;
        if(p!=Panel::Calendar)calendar.close(scene.machine.visible());
        if(changed) {
            if(p==Panel::None||p==Panel::Calendar)scene.panel_reveal.settle();
            else scene.panel_reveal.start(now(),scene.reduced||!scene.desktop);
        }
        if(changed&&scene.desktop)desktop_position();
        scene.machine.hold(Hold::Flyout,p!=Panel::None,now());
        if(p==Panel::Launcher&&scene.desktop) { SetForegroundWindow(hwnd);SetFocus(hwnd); }
        else if(p==Panel::Launcher) { layout_search(); ShowWindow(search,SW_SHOW); SetFocus(search); }
        else { ShowWindow(search,SW_HIDE); if(GetFocus()==search) SetFocus(hwnd); }
        schedule();
        if(changed&&scene.desktop&&layer_presented)paint_layer();
        if(changed&&p==Panel::Calendar) {
            const auto r=scene.clock_rect();POINT origin{};ClientToScreen(hwnd,&origin);const auto scale=dpi/96;
            const RECT anchor{origin.x+static_cast<LONG>(std::lround(r.left*scale)),origin.y+static_cast<LONG>(std::lround(r.top*scale)),origin.x+static_cast<LONG>(std::lround(r.right*scale)),origin.y+static_cast<LONG>(std::lround(r.bottom*scale))};
            calendar.open(hwnd,anchor,scene.reduced);
        }
    }
    void layout_search() {
        const auto r=scene.panel_rect(); const float scale=dpi/96;
        MoveWindow(search,static_cast<int>((r.left+32)*scale),static_cast<int>((r.top+65)*scale),static_cast<int>((r.right-r.left-64)*scale),static_cast<int>(24*scale),TRUE);
    }
    void launch(const std::wstring& path) {
        const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
        if(result<=32) MessageBoxW(hwnd,L"Windows could not open this item. Its original shortcut or file association has been preserved.",L"MinimaDock",MB_OK|MB_ICONERROR);
    }
    void group_action(size_t group) {
            const auto& g=scene.groups[group];
            if(g.windows.empty()) {launch(g.launch_path);if(scene.desktop)set_panel(Panel::None);return;}
            if(g.windows.size()==1) { if(!activate_window(g.windows[0])) MessageBoxW(hwnd,L"Windows did not allow activation of this window.",L"MinimaDock",MB_OK); }
            else {
                auto menu=CreatePopupMenu();
                for(size_t i=0;i<g.windows.size();++i) { wchar_t title[512]{}; GetWindowTextW(g.windows[i],title,512); AppendMenuW(menu,MF_STRING,static_cast<UINT_PTR>(i+1),title); }
                scene.machine.hold(Hold::ContextMenu,true,now());
                POINT pt{}; GetCursorPos(&pt); const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,pt.x,pt.y,0,hwnd,nullptr);
                DestroyMenu(menu); scene.machine.hold(Hold::ContextMenu,false,now());
                if(selected>0 && static_cast<size_t>(selected)<=g.windows.size()) activate_window(g.windows[static_cast<size_t>(selected-1)]); schedule();
            }
            if(scene.desktop)set_panel(Panel::None);
    }
    void overflow_menu() {
        auto menu=CreatePopupMenu();
        const auto first=scene.shown_groups()-1;
        for(size_t i=first;i<scene.groups.size();++i)AppendMenuW(menu,MF_STRING,static_cast<UINT_PTR>(i+1),scene.groups[i].name.c_str());
        scene.machine.hold(Hold::ContextMenu,true,now());
        POINT point{};GetCursorPos(&point);const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,hwnd,nullptr);
        DestroyMenu(menu);scene.machine.hold(Hold::ContextMenu,false,now());
        if(selected>0&&static_cast<size_t>(selected)<=scene.groups.size())group_action(static_cast<size_t>(selected)-1);
        schedule();
    }
    void action(int index) {
        if(!scene.machine.visible())return;
        const auto time=now();
        const auto count=static_cast<int>(scene.shown_groups());
        if(index>=count+5&&index<=count+7) {
            const auto control=static_cast<size_t>(index-count-5);
            const auto command=control==1?(scene.media.playing?MediaCommand::Pause:MediaCommand::Play):static_cast<MediaCommand>(control);
            if(scene.media_enabled(control)&&media.command(command,scene.media)){scene.media_pulses[control].start(time,scene.reduced);schedule();}
            return;
        }
        if(index==0)scene.launcher_pulse.start(time,scene.reduced);
        else if(index>0&&index<=count)scene.groups[static_cast<size_t>(index-1)].pulse.start(time,scene.reduced);
        else if(index==count+1)scene.tray_pulse.start(time,scene.reduced);
        else if(index==count+2)scene.settings_pulse.start(time,scene.reduced);
        else if(index==count+3)scene.clock_pulse.start(time,scene.reduced);
        schedule();
        if(scene.desktop&&layer_presented)paint_layer();
        if(index==0) { set_panel(scene.panel==Panel::Launcher?Panel::None:Panel::Launcher); return; }
        if(index<=count&&index>0) { const auto group=static_cast<size_t>(index-1);if(scene.overflow(group))overflow_menu();else group_action(group); }
        else if(index==count+1) set_panel(scene.panel==Panel::Tray?Panel::None:Panel::Tray);
        else if(index==count+2) set_panel(scene.panel==Panel::Settings?Panel::None:Panel::Settings);
        else if(index==count+3) set_panel(scene.panel==Panel::Calendar?Panel::None:Panel::Calendar);
        else if(index==count+4) {
            set_panel(Panel::None);scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
            desktop_action_result=toggle_desktop();
            if(FAILED(desktop_action_result)) {
                wchar_t details[256]{};
                swprintf(details,256,L"Windows could not show the desktop (error 0x%08lX).",static_cast<unsigned long>(desktop_action_result));
                MessageBoxW(hwnd,details,L"MinimaDock",MB_OK|MB_ICONERROR);
            }
        }
    }
    void click(float x,float y) {
        if(!scene.machine.visible()) return;
        if(scene.panel!=Panel::None&&scene.panel!=Panel::Calendar) {
            const auto r=scene.panel_rect();
            if(inside(r,x,y)) {
                if(scene.panel==Panel::Launcher) {
                    for(size_t i=0;i<std::min<size_t>(5,scene.results.size());++i) {
                        if(inside(D2D1::RectF(r.left+16,r.top+110+static_cast<float>(i)*42,r.right-16,r.top+150+static_cast<float>(i)*42),x,y)) { launch(scene.inventory.launcher[scene.results[i]].path); set_panel(Panel::None); return; }
                    }
                } else if(scene.panel==Panel::Settings) {
                    const wchar_t* links[]={L"ms-settings:network-wifi",L"ms-settings:bluetooth",L"ms-settings:network-airplanemode",L"ms-settings:easeofaccess",L"ms-settings:powersleep",L"ms-settings:easeofaccess-closedcaptioning"};
                    for(size_t i=0;i<6;++i) { const auto a=r.left+20+static_cast<float>(i%3)*109,b=r.top+56+static_cast<float>(i/3)*85;if(inside(D2D1::RectF(a,b,a+100,b+48),x,y))launch(links[i]); }
                    if(inside(D2D1::RectF(r.right-46,r.top+245,r.right-18,r.top+277),x,y))launch(L"ms-settings:sound");
                    if(inside(D2D1::RectF(r.right-45,r.bottom-36,r.right-16,r.bottom-5),x,y))launch(L"ms-settings:");
                    if(!scene.recovery_entry.empty() && inside(D2D1::RectF(r.left+20,r.bottom-33,r.right-55,r.bottom-8),x,y)) {
                        if(scene.desktop) {PostMessageW(hwnd,WM_CLOSE,0,0);return;}
                        const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",scene.recovery_entry.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
                        if(result>32) { scene.machine.recover(now()); PostMessageW(hwnd,WM_CLOSE,0,0); }
                        else MessageBoxW(hwnd,L"The recovery launcher could not start. Use the documented Task Manager recovery command.",L"MinimaDock",MB_OK|MB_ICONERROR);
                    }
                } else if(scene.panel==Panel::Tray) {
                    if(inside(D2D1::RectF(r.left+20,r.top+137,r.right-20,r.top+185),x,y))launch(L"ms-settings:notifications");
                    if(scene.groups.size()>scene.shown_groups()&&inside(D2D1::RectF(r.left+20,r.top+198,r.right-20,r.top+246),x,y))overflow_menu();
                }
                return;
            }
            if(!inside(scene.dock(),x,y)) { set_panel(Panel::None); return; }
        }
        const auto r=scene.dock();
        if(inside(r,x,y)) {
            if(inside(scene.desktop_button_rect(),x,y)){action(static_cast<int>(scene.shown_groups())+4);return;}
            if(inside(scene.launcher_rect(),x,y)) {action(0);return;}
            for(size_t i=0;i<scene.shown_groups();++i)if(inside(scene.app_rect(i),x,y)){action(static_cast<int>(i)+1);return;}
            if(scene.machine.progress()>.01f&&inside(scene.media_rect(),x,y)) {
                for(size_t i=0;i<3;++i)if(inside(scene.media_button(i),x,y)){action(static_cast<int>(scene.shown_groups())+5+static_cast<int>(i));return;}
            }
            if(scene.machine.progress()>.85f&&inside(scene.system_rect(),x,y)) {
                const auto s=scene.system_rect();const auto count=static_cast<int>(scene.shown_groups());
                if(x<s.left+24)action(count+1);
                else if(inside(scene.quick_settings_rect(),x,y))action(count+2);
                else if(inside(scene.clock_rect(),x,y))action(count+3);
            }
        }
    }
    void paint() {
        PAINTSTRUCT ps{}; BeginPaint(hwnd,&ps);
        if(scene.desktop) {
            // EndPaint even when a rendering failure triggers supervised shutdown.
            try { paint_layer(); } catch(...) {EndPaint(hwnd,&ps);throw;}
            EndPaint(hwnd,&ps);return;
        }
        if(!render) {
            RECT r{}; GetClientRect(hwnd,&r);
            const auto hr=scene.factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(r.right,r.bottom)),render.put());
            if(FAILED(hr)) { EndPaint(hwnd,&ps); return; }
            render->SetDpi(dpi,dpi); scene.bind(render.get());
        }
        scene.draw(); const auto hr=render->EndDraw();
        if(hr==D2DERR_RECREATE_TARGET) { scene.brush.reset(); scene.target.reset(); render.reset(); }
        EndPaint(hwnd,&ps);
    }
    void paint_layer() {
        sync_visibility();
        RECT position{};
        if(layer_bounds_pending)position=pending_layer_bounds;else GetWindowRect(hwnd,&position);
        const auto w=static_cast<UINT>(position.right-position.left),h=static_cast<UINT>(position.bottom-position.top);
        if(!w||!h)return;
        // Reserve popup height once; opening/closing panels then retains the
        // render target and its icon resources. Only the visible rows are copied.
        const auto capacity=std::max(h,static_cast<UINT>(std::ceil(440*dpi/96)));
        if(!layer_render||w!=layer_width||capacity>layer_height) {
            release_layer();layer_width=w;layer_height=capacity;++layer_allocations;
            BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=static_cast<LONG>(w);
            bitmap.bmiHeader.biHeight=-static_cast<LONG>(capacity);bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
            layer_dc=CreateCompatibleDC(nullptr);layer_dib=CreateDIBSection(layer_dc,&bitmap,DIB_RGB_COLORS,&layer_pixels,nullptr,0);
            if(!layer_dc||!layer_dib||!layer_pixels)throw E_OUTOFMEMORY;
            layer_previous=SelectObject(layer_dc,layer_dib);
            check(scene.wic->CreateBitmap(w,capacity,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,layer_bitmap.put()));
            check(scene.factory->CreateWicBitmapRenderTarget(layer_bitmap.get(),D2D1::RenderTargetProperties(),layer_render.put()));
            layer_render->SetDpi(dpi,dpi);layer_render->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);scene.bind(layer_render.get());
        }
        layer_render->SetTransform(D2D1::Matrix3x2F::Translation(0,live_dock?visibility.offset():0));
        scene.draw();check(layer_render->EndDraw());layer_render->SetTransform(D2D1::Matrix3x2F::Identity());
        WICRect visible{0,0,static_cast<INT>(w),static_cast<INT>(h)};
        check(layer_bitmap->CopyPixels(&visible,w*4,w*h*4,static_cast<BYTE*>(layer_pixels)));
        if(live_dock) {
            const auto alpha=visibility.opacity();auto pixels=static_cast<BYTE*>(layer_pixels);
            if(alpha==0)std::fill_n(pixels,static_cast<size_t>(w)*h*4,BYTE{0});
            else if(alpha<1)for(size_t i=0;i<static_cast<size_t>(w)*h*4;++i)pixels[i]=static_cast<BYTE>(std::lround(pixels[i]*alpha));
            // A nearly transparent final-position hit surface lets the pointer
            // enter a moving pill without chasing the animated geometry.
            if(auto_hidden&&visibility.wanted()) {
                const auto r=scene.dock();const float scale=dpi/96;
                const auto left=std::clamp(static_cast<int>(r.left*scale),0,static_cast<int>(w)),right=std::clamp(static_cast<int>(r.right*scale),0,static_cast<int>(w));
                const auto top=std::clamp(static_cast<int>(r.top*scale),0,static_cast<int>(h)),bottom=std::clamp(static_cast<int>(r.bottom*scale),0,static_cast<int>(h));
                for(int y=top;y<bottom;++y)for(int x=left;x<right;++x){auto& a=pixels[(static_cast<size_t>(y)*w+x)*4+3];if(!a)a=1;}
            }
        }
        POINT destination{position.left,position.top},source{0,0};SIZE size{static_cast<LONG>(w),static_cast<LONG>(h)};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        presenting_layer=true;
        const bool updated=UpdateLayeredWindow(hwnd,nullptr,&destination,&size,layer_dc,&source,0,&blend,ULW_ALPHA)!=FALSE;
        presenting_layer=false;
        if(!updated)throw E_FAIL;
        layer_bounds_pending=false;layer_presented=true;presented_height=h;++layer_frames;
        presented_center_y=position.top+static_cast<LONG>(std::lround(scene.layout().center_y*dpi/96));
    }
    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        switch(msg) {
        case WM_CREATE: {
            scene.initialize(); update_preferences(); dpi=static_cast<float>(GetDpiForWindow(hwnd));
            wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
            auto parent=std::filesystem::path(module).parent_path();
            for(unsigned i=0;i<3;++i) {
                const auto candidate=parent/L"recovery-bundle/Restore-Windows.cmd";
                if(std::filesystem::is_regular_file(candidate)) { scene.recovery_entry=candidate.wstring(); break; }
                parent=parent.parent_path();
            }
            if(scene.desktop) scene.recovery_entry=L"session-revert";
            search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,hwnd,reinterpret_cast<HMENU>(101),nullptr,nullptr);
            edit_background=CreateSolidBrush(RGB(20,27,37));
            SendMessageW(search,EM_SETLIMITTEXT,120,0); SendMessageW(search,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(L"Search installed shortcuts"));
            wchar_t family[64]{};scene.fonts[0]->GetFontFamilyName(family,64);
            search_font=CreateFontW(-static_cast<int>(16*dpi/96),0,0,0,FW_LIGHT,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
            SendMessageW(search,WM_SETFONT,reinterpret_cast<WPARAM>(search_font),TRUE);
            event_queued=false;visibility_queued=false;event_target=hwnd;
            hooks[0]=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            hooks[1]=SetWinEventHook(EVENT_OBJECT_SHOW,EVENT_OBJECT_HIDE,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            hooks[2]=SetWinEventHook(EVENT_OBJECT_NAMECHANGE,EVENT_OBJECT_NAMECHANGE,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            if(live_dock) {
                hooks[3]=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
                hooks[4]=SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
                create_edge();visibility.reduced_motion(true);fullscreen();visibility.reduced_motion(scene.reduced);
            }
            SetTimer(hwnd,clock_timer,60000,nullptr);
            media.start(hwnd);
            scan(true); return 0;
        }
        case WM_APP+40: return layer_presented&&inventory_loaded?1:0;
        // Read-only diagnostics used by native component checks; no external input hook.
        case WM_APP+41: return static_cast<LRESULT>(scene.machine.state());
        case WM_APP+42: return static_cast<LRESULT>(scene.shown_groups());
        case WM_APP+43: return static_cast<size_t>(w)<scene.shown_groups()?static_cast<LRESULT>((scene.app_x(static_cast<size_t>(w))-scene.width/2)*1000):0;
        case WM_APP+44: return static_cast<size_t>(w)<scene.shown_groups()?(scene.tile_running(static_cast<size_t>(w))?1:0)|(scene.groups[static_cast<size_t>(w)].pinned?2:0):0;
        case WM_APP+45: return static_cast<LRESULT>(scene.layout().height(scene.machine.progress())*1000);
        case WM_APP+46: {const auto r=scene.desktop_button_rect();return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+47: return static_cast<LRESULT>(scene.layout().center_y*1000);
        case WM_APP+48: return static_cast<LRESULT>(desktop_action_result);
        case WM_APP+49: return static_cast<LRESULT>(visibility.progress()*1000);
        case WM_APP+50: return auto_hidden?1:0;
        case WM_APP+51: return animation_timer?1:0;
        case WM_APP+52: return visibility.wanted()?1:0;
        case WM_APP+53: return reveal_hotkey?1:0;
        case WM_APP+54: {const auto r=scene.dock();return static_cast<LRESULT>((r.right-r.left)*1000);}
        case WM_APP+55: return edge_indicator_alpha;
        case WM_APP+56: return presented_center_y;
        case WM_APP+57: return layer_allocations;
        case WM_APP+58: return layer_frames;
        case WM_APP+59: return static_cast<LRESULT>(scene.panel);
        case WM_APP+60: return static_cast<LRESULT>(scene.active_pulses());
        case WM_APP+61: return static_cast<LRESULT>(scene.panel_reveal.opacity()*1000);
        case WM_APP+62: return static_cast<LRESULT>(scene.panel_reveal.offset()*1000);
        case WM_APP+63: return scene.pressed?1:0;
        case WM_APP+64: return scene.reduced?1:0;
        case WM_APP+65: {const auto r=scene.clock_rect();return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+66:return reinterpret_cast<LRESULT>(last_foreground);
        case WM_APP+67:return scene.media.available|(scene.media.playing<<1)|(scene.media.previous<<2)|(scene.media.toggle<<3)|(scene.media.next<<4)|(scene.media.unavailable<<5);
        case WM_APP+68:case WM_APP+69:case WM_APP+70:{const auto r=scene.media_button(msg-(WM_APP+68));return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+71:{const auto r=scene.media_button(0);return static_cast<LRESULT>((r.top+r.bottom)*500);}
        case WM_APP+72:return media_label_hash(scene.media.title);
        case WM_APP+73:return static_cast<LRESULT>(scene.media_marquee.offset()*1000);
        case WM_APP+74:return media_timer?1:0;
        case media_changed_message:scene.media=media.snapshot();schedule();return 0;
        case calendar_closed_message:
            if(!calendar.window()&&(scene.panel==Panel::Calendar||scene.panel==Panel::None)) {
                set_panel(Panel::None);scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
                if((l||GetForegroundWindow()==hwnd)&&IsWindow(calendar_previous))SetForegroundWindow(calendar_previous);
                schedule();
            }return 0;
        case foreground_changed:
            // The event timestamp excludes queued activation from before opening.
            // Querying current foreground here can observe a transient null HWND.
            if(scene.panel==Panel::Calendar&&static_cast<LONG>(static_cast<DWORD>(l)-calendar_opened)>=0){DWORD pid=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(w),&pid);if(pid&&pid!=GetCurrentProcessId())set_panel(Panel::None);}return 0;
        case WM_DISPLAYCHANGE: if(scene.desktop){release_layer();desktop_position();fullscreen();schedule();}return 0;
        case WM_MOUSEACTIVATE: {
            if(scene.desktop&&scene.panel!=Panel::Launcher) return MA_NOACTIVATE;
            return DefWindowProcW(hwnd,msg,w,l);
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_SIZE: {
            if(scene.desktop&&presenting_layer)return 0;
            scene.width=LOWORD(l)*96.0f/dpi; scene.height=HIWORD(l)*96.0f/dpi;
            scene.balance();
            if(render) render->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));
            layout_search(); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        case WM_GETMINMAXINFO: {
            if(!scene.desktop) {auto info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize.x=static_cast<LONG>(740*dpi/96); info->ptMinTrackSize.y=static_cast<LONG>(620*dpi/96);} return 0;
        }
        case WM_DPICHANGED: {
            dpi=static_cast<float>(HIWORD(w)); if(render) render->SetDpi(dpi,dpi);
            if(scene.desktop){release_layer();desktop_position();}
            else {const auto r=reinterpret_cast<RECT*>(l); SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);} return 0;
        }
        case WM_SETTINGCHANGE: update_preferences(); schedule(); return 0;
        case WM_MOUSEMOVE: {
            const float x=GET_X_LPARAM(l)*96.0f/dpi,raw_y=GET_Y_LPARAM(l)*96.0f/dpi,y=raw_y-(live_dock?visibility.offset():0);
            scene.pointer_x=x; scene.pointer_y=y;
            if(!tracking) { TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0}; TrackMouseEvent(&t); tracking=true; }
            const bool over=inside(scene.dock(),x,y)||(live_dock&&visibility.wanted()&&inside(scene.dock(),x,raw_y))||(scene.panel!=Panel::None && scene.panel!=Panel::Calendar && inside(scene.panel_rect(),x,y));
            scene.machine.hold(Hold::Pointer,over,now()); schedule(); return 0;
        }
        case WM_MOUSELEAVE: tracking=false; scene.pointer_x=scene.pointer_y=-1;if(live_dock)pointer_hold();else scene.machine.hold(Hold::Pointer,false,now());schedule();return 0;
        case WM_LBUTTONDOWN: scene.press_at(GET_X_LPARAM(l)*96.0f/dpi,GET_Y_LPARAM(l)*96.0f/dpi-(live_dock?visibility.offset():0));SetCapture(hwnd);schedule();return 0;
        case WM_LBUTTONUP: scene.clear_press();ReleaseCapture();if(!live_dock||visibility.wanted())click(GET_X_LPARAM(l)*96.0f/dpi,GET_Y_LPARAM(l)*96.0f/dpi-(live_dock?visibility.offset():0));schedule();return 0;
        case WM_CAPTURECHANGED: scene.clear_press();schedule();return 0;
        case WM_RBUTTONUP: if(scene.desktop)desktop_menu();return 0;
        case WM_CHAR:
            if(scene.desktop&&scene.panel==Panel::Launcher) {
                if(w==VK_BACK){if(!scene.query.empty())scene.query.pop_back();}
                else if(w>=32&&w!=127&&scene.query.size()<120)scene.query.push_back(static_cast<wchar_t>(w));
                scene.filter();schedule();
            }return 0;
        case WM_LBUTTONDBLCLK: {
            const float x=GET_X_LPARAM(l)*96.0f/dpi,y=GET_Y_LPARAM(l)*96.0f/dpi;
            for(size_t i=0;i<std::min<size_t>(8,scene.inventory.desktop.size());++i) if(inside(scene.desktop_item(i),x,y)) launch(scene.inventory.desktop[i].path);
            return 0;
        }
        case WM_KEYDOWN: {
            if(w==VK_ESCAPE) { if(scene.panel!=Panel::None) set_panel(Panel::None); else { scene.focus=-1; scene.machine.hold(Hold::Keyboard,false,now());if(live_dock&&GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);schedule(); } }
            else if(w=='F'&&!scene.desktop) { set_panel(Panel::None); scene.machine.suppress(scene.machine.visible(),now()); schedule(); }
            else if(w=='R'&&!scene.desktop) { scene.reduced=!scene.reduced; scene.machine.reduced_motion(scene.reduced); calendar.preferences(scene.reduced);if(scene.reduced)scene.stop_feedback(); schedule(); }
            else if(w==VK_RETURN&&scene.desktop&&scene.panel==Panel::Launcher) {if(!scene.results.empty())launch(scene.inventory.launcher[scene.results[0]].path);set_panel(Panel::None);}
            else if(w==VK_TAB||w==VK_RIGHT||w==VK_LEFT) {
                const int count=static_cast<int>(scene.shown_groups())+8;
                const int direction=(w==VK_LEFT||(w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000)))?-1:1;
                scene.focus=(scene.focus+direction+count)%count; scene.machine.hold(Hold::Keyboard,true,now()); schedule();
            } else if((w==VK_RETURN||w==VK_SPACE)&&!(scene.desktop&&scene.panel==Panel::Launcher)) action(std::max(0,scene.focus));
            return 0;
        }
        case WM_HOTKEY: if(w==3){fullscreen();if(scene.machine.visible()){SetForegroundWindow(hwnd);SetFocus(hwnd);scene.focus=0;scene.machine.hold(Hold::Keyboard,true,now());schedule();}}return 0;
        case WM_KILLFOCUS: if(reinterpret_cast<HWND>(w)!=search&&reinterpret_cast<HWND>(w)!=calendar.window()) { if(scene.desktop)set_panel(Panel::None);scene.focus=-1; scene.machine.hold(Hold::Keyboard,false,now()); schedule(); } return 0;
        case WM_COMMAND:
            if(LOWORD(w)==101&&HIWORD(w)==EN_CHANGE) { wchar_t value[128]{}; GetWindowTextW(search,value,128); scene.query=value; scene.filter(); InvalidateRect(hwnd,nullptr,FALSE); } return 0;
        case WM_CTLCOLOREDIT:
            if(reinterpret_cast<HWND>(l)==search) { SetTextColor(reinterpret_cast<HDC>(w),RGB(242,244,250)); SetBkColor(reinterpret_cast<HDC>(w),RGB(20,27,37)); return reinterpret_cast<LRESULT>(edit_background); }
            return DefWindowProcW(hwnd,msg,w,l);
        case WM_TIMER:
            if(w==frame_timer) { const auto time=now();scene.machine.tick(time);visibility.tick(time);scene.tick_feedback(time);schedule(); }
            else if(w==media_title_timer){scene.media_marquee.tick(now());InvalidateRect(hwnd,nullptr,FALSE);}
            else if(w==reconcile_timer) { KillTimer(hwnd,reconcile_timer);reconcile_scheduled=false; scan(false); }
            else if(w==clock_timer) {scene.update_clock();if(scene.machine.progress()>.01f)InvalidateRect(hwnd,nullptr,FALSE);} return 0;
        case visibility_changed: {
            visibility_queued=false;const auto before=scene.machine.state();const bool maximized=auto_hidden,wanted=visibility.wanted();
            fullscreen();if(before!=scene.machine.state()||maximized!=auto_hidden||wanted!=visibility.wanted())schedule();return 0;
        }
        case windows_changed: {
            event_queued=false;
            fullscreen();schedule();
            if(!reconcile_scheduled)reconcile_scheduled=SetTimer(hwnd,reconcile_timer,180,nullptr)!=0;
            return 0;
        }
        case inventory_ready: {
            std::unique_ptr<Inventory> data; bool full;
            { std::lock_guard lock(mutex); data=std::move(pending); full=pending_full; }
            bool changed=false;
            if(data) {changed=scene.accept(std::move(*data),full);inventory_loaded=true;}
            if(rescan) { rescan=false; SetTimer(hwnd,reconcile_timer,180,nullptr); }
            if(changed){if(scene.desktop)desktop_position();InvalidateRect(hwnd,nullptr,FALSE);} return 0;
        }
        case WM_DESTROY:
            media.stop();
            calendar.close(false);
            event_target=nullptr; for(auto hook:hooks) if(hook) UnhookWinEvent(hook);
            if(edge)DestroyWindow(edge);UnregisterHotKey(hwnd,3);
            KillTimer(hwnd,frame_timer); KillTimer(hwnd,reconcile_timer);KillTimer(hwnd,clock_timer);release_layer();
            worker.request_stop(); if(worker.joinable()) worker.join();
            if(search_font) DeleteObject(search_font); if(edit_background) DeleteObject(edit_background); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd,msg,w,l);
        }
    }
};
}
int run_preview(HINSTANCE instance, int show) {
    Preview preview;
    WNDCLASSW klass{}; klass.style=CS_DBLCLKS; klass.lpfnWndProc=Preview::proc; klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW); klass.lpszClassName=L"MinimaDock.Preview";
    if(!RegisterClassW(&klass)) return 1;
    const auto hwnd=CreateWindowExW(0,klass.lpszClassName,L"MinimaDock — Native design preview",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1180,790,nullptr,nullptr,instance,&preview);
    if(!hwnd) return 1;
    BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
    ShowWindow(hwnd,show); MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0) {
        if(msg.hwnd==preview.search&&msg.message==WM_KEYDOWN) {
            if(msg.wParam==VK_ESCAPE) { preview.set_panel(Panel::None); continue; }
            if(msg.wParam==VK_RETURN) { if(!preview.scene.results.empty()) preview.launch(preview.scene.inventory.launcher[preview.scene.results[0]].path); preview.set_panel(Panel::None); continue; }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
int run_desktop_dock(HINSTANCE instance) {
    Preview dock;dock.scene.desktop=true;dock.live_dock=true;
    WNDCLASSW klass{};klass.style=CS_DBLCLKS;klass.lpfnWndProc=Preview::proc;klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.lpszClassName=L"MinimaDock.DesktopDock";
    if(!RegisterClassW(&klass))return 1;
    const auto hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,klass.lpszClassName,L"MinimaDock — Temporary taskbar test",WS_POPUP,0,0,1000,440,nullptr,nullptr,instance,&dock);
    if(!hwnd)return 1;
    dock.desktop_position();ShowWindow(hwnd,SW_SHOWNOACTIVATE);InvalidateRect(hwnd,nullptr,FALSE);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return static_cast<int>(message.wParam);
}
int benchmark_dock_render(HINSTANCE instance,const std::wstring& path) {
    try {
        Preview dock;dock.scene.desktop=true;
        WNDCLASSW klass{};klass.lpfnWndProc=Preview::proc;klass.hInstance=instance;klass.lpszClassName=L"MinimaDock.RenderBenchmark";
        if(!RegisterClassW(&klass))return 1;
        const auto window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,klass.lpszClassName,L"MinimaDock renderer measurement",WS_POPUP,0,0,1000,440,nullptr,nullptr,instance,&dock);
        if(!window)return 1;
        dock.desktop_position();
        const auto deadline=GetTickCount64()+15000;
        while(!dock.inventory_loaded&&GetTickCount64()<deadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);
        }
        if(!dock.inventory_loaded){DestroyWindow(window);return 1;}
        dock.scene.machine.reduced_motion(true);dock.scene.machine.hold(Hold::Keyboard,true,0);
        std::vector<double> times;
        for(unsigned i=0;i<104;++i) {
            dock.scene.pointer_x=dock.scene.width/2+36;dock.scene.pointer_y=dock.scene.height-34;
            const auto start=std::chrono::steady_clock::now();dock.paint_layer();
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(i>=8)times.push_back(ms);
        }
        std::sort(times.begin(),times.end());
        std::ofstream output{std::filesystem::path(path)};
        output<<"{\n  \"Kind\": \"cached native frame microbenchmark; hidden test window, not end-to-end pointer latency\",\n  \"Samples\": "<<times.size()<<",\n  \"LayerWidthPx\": "<<dock.layer_width<<",\n  \"LayerHeightPx\": "<<dock.presented_height<<",\n  \"MedianMs\": "<<times[times.size()/2]<<",\n  \"P95Ms\": "<<times[static_cast<size_t>(times.size()*.95)]<<",\n  \"MaxMs\": "<<times.back()<<",\n  \"Groups\": [";
        for(size_t i=0;i<dock.scene.groups.size();++i) {
            const auto& group=dock.scene.groups[i];
            if(i)output<<',';
            output<<"\n    {\"Name\": "<<json_text(group.name)<<", \"AppId\": "<<json_text(group.app_id)<<", \"Pinned\": "<<(group.pinned?"true":"false")<<", \"Running\": "<<(!group.windows.empty()?"true":"false")<<", \"HasIcon\": "<<(group.icon?"true":"false");
            if(i<dock.scene.shown_groups())output<<", \"Side\": "<<dock.scene.placements.apps[i].side;
            output<<'}';
        }
        output<<"\n  ],\n  \"WindowIconSources\": [";
        for(size_t i=0;i<dock.scene.inventory.windows.size();++i) {
            const auto& app=dock.scene.inventory.windows[i];if(i)output<<',';
            output<<"\n    {\"Title\": "<<json_text(app.title)<<", \"AppId\": "<<json_text(app.app_id)<<", \"Source\": "<<json_text(app.icon_source)<<'}';
        }
        output<<"\n  ]\n}\n";
        // Export this app's rendered pixels for visual QA, never capture the desktop.
        ComPtr<IWICBitmap> image;
        check(dock.scene.wic->CreateBitmapFromMemory(dock.layer_width,dock.presented_height,GUID_WICPixelFormat32bppPBGRA,dock.layer_width*4,dock.layer_width*dock.presented_height*4,static_cast<BYTE*>(dock.layer_pixels),image.put()));
        auto png=std::filesystem::path(path);png.replace_extension(L".png");
        ComPtr<IWICStream> stream;check(dock.scene.wic->CreateStream(stream.put()));check(stream->InitializeFromFilename(png.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;check(dock.scene.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));check(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(frame.put(),nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(dock.layer_width,dock.presented_height));
        auto format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format));check(frame->WriteSource(image.get(),nullptr));check(frame->Commit());check(encoder->Commit());
        DestroyWindow(window);return output?0:1;
    }catch(...){return 1;}
}
int save_preview_snapshot(const std::wstring& path, bool expanded,int panel) {
    try {
        Scene scene; scene.initialize();scene.desktop=panel>=3&&panel<=13;
        const UINT image_width=scene.desktop?1000:1160,image_height=(panel>=4&&panel<=6)||(panel>=8&&panel<=13)?88:(scene.desktop?440:740);
        scene.width=static_cast<float>(image_width);scene.height=static_cast<float>(image_height);
        ComPtr<IWICImagingFactory> wic;
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IWICBitmap> bitmap; check(wic->CreateBitmap(image_width,image_height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
        ComPtr<ID2D1RenderTarget> rt; check(scene.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),rt.put()));
        scene.bind(rt.get()); scene.accept(collect_inventory(GetCurrentProcessId()),true);
        if(expanded) { scene.machine.reduced_motion(true); scene.machine.hold(Hold::Keyboard,true,0); }
        if(panel==1)scene.panel=Panel::Settings;else if(panel==2)scene.panel=Panel::Tray;
        else if(panel==4){const auto button=scene.desktop_button_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==5){const auto button=scene.launcher_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==6){scene.launcher_pulse.start(0);scene.launcher_pulse.tick(60);const auto button=scene.launcher_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==7){scene.panel=Panel::Settings;scene.panel_reveal.start(0);scene.panel_reveal.tick(60);}
        else if(panel>=8&&panel<=10){const auto system=scene.system_rect();constexpr float offsets[]={11,38,63};scene.pointer_x=system.left+offsets[panel-8];scene.pointer_y=scene.layout().center_y;}
        else if(panel==11){const auto clock=scene.clock_rect();scene.pointer_x=(clock.left+clock.right)/2;scene.pointer_y=scene.layout().center_y;}
        else if(panel==12||panel==13){scene.media.available=scene.media.previous=scene.media.toggle=scene.media.next=true;scene.media.playing=panel==12;scene.media.title=L"Midnight Drive";scene.media.artist=L"Local player";const auto button=scene.media_button(1);scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        scene.draw(); check(rt->EndDraw());
        ComPtr<IWICStream> stream; check(wic->CreateStream(stream.put())); check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder; check(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put())); check(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(frame.put(),nullptr)); check(frame->Initialize(nullptr)); check(frame->SetSize(image_width,image_height));
        auto format=GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format)); check(frame->WriteSource(bitmap.get(),nullptr)); check(frame->Commit()); check(encoder->Commit()); return 0;
    } catch(...) { return 1; }
}
}
