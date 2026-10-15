#include "rendering/customize.h"
#include "rendering/glass.h"
#include "rendering/glass_style.h"
#include "platform/com_ptr.h"
#include "taskbar/prayer_methods.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace minima {
namespace {
constexpr float window_width=820,window_height=580,side_width=204,pad=26,row_height=60,chip_row_height=88,group_gap=14,radius=14;
constexpr float margin_x=24,margin_top=16,margin_bottom=30; // Room for the glass shadow.
enum class Page {Look,Dock,Prayer,Launch,About};
constexpr Page pages[]={Page::Look,Page::Dock,Page::Prayer,Page::Launch,Page::About};
const wchar_t* page_name(Page p){switch(p){case Page::Look:return L"Appearance";case Page::Dock:return L"Dock";case Page::Prayer:return L"Prayer times";case Page::Launch:return L"Launcher";default:return L"About and updates";}}
wchar_t page_glyph(Page p){switch(p){case Page::Look:return L'\xE790';case Page::Dock:return L'\xE8A9';case Page::Prayer:return L'\xE708';case Page::Launch:return L'\xE721';default:return L'\xE946';}}
const wchar_t* page_lede(Page p) {
    switch(p) {
    case Page::Look:return L"The look of the dock, its flyouts, menus and the prayer card.";
    case Page::Dock:return L"Style, behavior, size and what the dock shows.";
    case Page::Prayer:return L"Read from your masjid's website and kept on this PC.";
    case Page::Launch:return L"The search that opens from the MinimaDock button.";
    default:return L"";
    }
}
std::wstring clock_text(int minutes,bool marker=true) {
    if(minutes<0)return L"\x2014";
    SYSTEMTIME t{};t.wHour=static_cast<WORD>((minutes/60)%24);t.wMinute=static_cast<WORD>(minutes%60);
    wchar_t text[32]{};GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS|(marker?0:TIME_NOTIMEMARKER),&t,nullptr,text,32);return text;
}
const wchar_t* chime_file(int chime){return chime==1?L"\\Media\\Windows Notify Messaging.wav":L"\\Media\\Windows Notify Calendar.wav";}
}

struct CustomizeWindow::Impl {
    // ---- State ----
    HWND hwnd=nullptr;
    DockSettings dock;PrayerSettings prayer;CustomizeHooks hooks;bool reduced=false;
    theme::Palette pal=theme::palette(false);bool live=true;
    Page page=Page::Look;float scroll=0,scroll_target=0,content_height=0;
    std::int64_t page_changed=-1,opened=-1;
    float px=-1,py=-1;bool slider_drag=false,mouse_down=false;
    // Text fields: 0 none, 1 website, 2 location.
    int editing=0;std::wstring edit_text;bool select_all=false;std::int64_t caret_start=0;
    // ---- Drawing ----
    UINT dpi=96;UINT pixel_width=0,pixel_height=0;
    ComPtr<ID2D1Factory> factory;ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteTextFormat> f_title,f_body,f_small,f_strong,f_icon,f_icon_small,f_button;
    ID2D1RenderTarget* t=nullptr;ComPtr<ID2D1SolidColorBrush> brush;
    GlassHost host;bool composed=false;unsigned host_generation=~0u;GlassPainter painter;bool painter_bound=false;
    ComPtr<ID2D1HwndRenderTarget> hwnd_target;
    struct Hit {D2D1_RECT_F r;std::function<void(float x,float y)> click;bool slider=false;std::function<void(float x)> drag;};
    std::vector<Hit> hits;

    static std::int64_t now(){return static_cast<std::int64_t>(GetTickCount64());}
    // ---- Motion (in the spirit of animate-ui: springy highlights, sliding
    // indicators, staggered fade-up entrances and rolling numbers) ----
    struct Spring {float x=0,v=0;bool init=false;};
    std::unordered_map<std::wstring,Spring> springs;
    struct Roll {std::wstring now,before;std::int64_t at=-1;int dir=1;};
    std::unordered_map<std::wstring,Roll> rolls;
    std::int64_t last_frame=0;float frame_dt=0;bool animating=false;int last_step=1;
    std::wstring row_key;int control_index=0,item_index=0;
    std::wstring key(const wchar_t* kind){return row_key+L"|"+kind+std::to_wstring(control_index++);}
    // Damped spring toward `target`; first use (and reduced motion) snaps.
    float spring(const std::wstring& k,float target,float stiffness=520,float damping=40,float tolerance=.05f) {
        auto& s=springs[k];
        if(!s.init||reduced){s={target,0,true};return target;}
        for(float dt=frame_dt;dt>0;dt-=.004f){const float h=std::min(dt,.004f);s.v+=(stiffness*(target-s.x)-damping*s.v)*h;s.x+=s.v*h;}
        if(std::abs(target-s.x)<tolerance&&std::abs(s.v)<tolerance*10){s.x=target;s.v=0;}else animating=true;
        return s.x;
    }
    float peek(const std::wstring& k,float fallback){const auto it=springs.find(k);return it==springs.end()||!it->second.init?fallback:it->second.x;}
    void snap(const std::wstring& k,float value){springs[k]={value,0,true};}
    static float ease_out(float e){e=std::clamp(e,0.0f,1.0f);return 1-(1-e)*(1-e)*(1-e);}
    static D2D1_COLOR_F mix(D2D1_COLOR_F a,D2D1_COLOR_F b,float k){return {a.r+(b.r-a.r)*k,a.g+(b.g-a.g)*k,a.b+(b.b-a.b)*k,a.a+(b.a-a.a)*k};}
    // Page content enters in order: each item fades in and rises 14 DIP.
    struct Item {size_t first_hit;float dy,saved_py;D2D1_MATRIX_3X2_F before;bool layer;};
    Item begin_item() {
        const float local=std::clamp((since(page_changed)-static_cast<float>(item_index)*45)/340,0.0f,1.0f);++item_index;
        const float e=ease_out(local);if(local<1)animating=true;
        Item it{hits.size(),(1-e)*14,py,{},false};t->GetTransform(&it.before);
        if(e<1) {
            t->SetTransform(D2D1::Matrix3x2F::Translation(0,it.dy)*it.before);py-=it.dy;
            t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),e),nullptr);it.layer=true;
        }
        return it;
    }
    void end_item(const Item& it) {
        if(it.layer)t->PopLayer();
        t->SetTransform(it.before);py=it.saved_py;
        for(size_t i=it.first_hit;i<hits.size();++i){hits[i].r.top+=it.dy;hits[i].r.bottom+=it.dy;}
    }
    // Text that rolls up (or down) to its new value, like a sliding number.
    void rolling_text(const std::wstring& k,const std::wstring& label,D2D1_RECT_F r,IDWriteTextFormat* f,D2D1_COLOR_F c) {
        auto& roll=rolls[k];
        if(roll.now.empty()&&roll.at<0)roll.now=label;
        else if(label!=roll.now){roll.before=roll.now;roll.now=label;roll.at=now();roll.dir=last_step<0?-1:1;}
        const float local=std::clamp(since(roll.at)/280,0.0f,1.0f),e=ease_out(local);if(local<1)animating=true;
        const float h=(r.bottom-r.top)*.7f*static_cast<float>(roll.dir);
        t->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if(e<1)text(roll.before,D2D1::RectF(r.left,r.top-h*e,r.right,r.bottom-h*e),f,fade(c,1-e),DWRITE_TEXT_ALIGNMENT_CENTER);
        text(roll.now,D2D1::RectF(r.left,r.top+h*(1-e),r.right,r.bottom+h*(1-e)),f,fade(c,e),DWRITE_TEXT_ALIGNMENT_CENTER);
        t->PopAxisAlignedClip();
    }
    float since(std::int64_t start) const{return reduced||start<0?1e9f:static_cast<float>(now()-start);}
    bool resources() {
        if(factory)return true;
        if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()))||
           FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())))){factory.reset();return false;}
        ComPtr<IDWriteFontCollection> fonts;write->GetSystemFontCollection(fonts.put());
        const auto family=theme::text_family(fonts.get());
        const auto make=[&](ComPtr<IDWriteTextFormat>& f,const wchar_t* face,float size,DWRITE_FONT_WEIGHT weight) {
            write->CreateTextFormat(face,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",f.put());
            if(!f)return;
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};ComPtr<IDWriteInlineObject> dots;write->CreateEllipsisTrimmingSign(f.get(),dots.put());f->SetTrimming(&trim,dots.get());
        };
        make(f_title,family,22,DWRITE_FONT_WEIGHT_NORMAL);make(f_body,family,14,DWRITE_FONT_WEIGHT_NORMAL);make(f_small,family,12,DWRITE_FONT_WEIGHT_LIGHT);
        make(f_strong,family,15,DWRITE_FONT_WEIGHT_NORMAL);make(f_button,family,12.5f,DWRITE_FONT_WEIGHT_NORMAL);
        make(f_icon,L"Segoe Fluent Icons",16,DWRITE_FONT_WEIGHT_NORMAL);make(f_icon_small,L"Segoe Fluent Icons",11,DWRITE_FONT_WEIGHT_NORMAL);
        return f_title&&f_body&&f_small&&f_strong&&f_icon&&f_button&&f_icon_small;
    }
    // ---- Primitives ----
    static D2D1_COLOR_F fade(D2D1_COLOR_F c,float a){c.a*=a;return c;}
    void color(D2D1_COLOR_F c){brush->SetColor(c);}
    void fill(D2D1_RECT_F r,D2D1_COLOR_F c,float rr=0){color(c);if(rr>0)t->FillRoundedRectangle(D2D1::RoundedRect(r,rr,rr),brush.get());else t->FillRectangle(r,brush.get());}
    void stroke(D2D1_RECT_F r,D2D1_COLOR_F c,float rr,float w=1){color(c);t->DrawRoundedRectangle(D2D1::RoundedRect(r,rr,rr),brush.get(),w);}
    void text(std::wstring_view s,D2D1_RECT_F r,IDWriteTextFormat* f,D2D1_COLOR_F c,DWRITE_TEXT_ALIGNMENT a=DWRITE_TEXT_ALIGNMENT_LEADING) {
        color(c);f->SetTextAlignment(a);t->DrawText(s.data(),static_cast<UINT32>(s.size()),f,r,brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float measure(std::wstring_view s,IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> layout;if(FAILED(write->CreateTextLayout(s.data(),static_cast<UINT32>(s.size()),f,4096,64,layout.put())))return 0;
        DWRITE_TEXT_METRICS m{};layout->GetMetrics(&m);return m.widthIncludingTrailingWhitespace;
    }
    bool over(D2D1_RECT_F r) const{return px>=r.left&&px<r.right&&py>=r.top&&py<r.bottom;}
    void hit(D2D1_RECT_F r,std::function<void(float,float)> click){hits.push_back({r,std::move(click),false,nullptr});}
    // ---- Controls (right-aligned in [left,right], vertically centred on cy) ----
    // Segmented choice.
    float segmented(float right,float cy,const std::vector<std::wstring>& options,int selected,std::function<void(int)> pick) {
        std::vector<float> widths;float total=4;for(const auto& o:options){widths.push_back(measure(o,f_button.get())+22);total+=widths.back();}
        const float left=right-total;const auto box=D2D1::RectF(left,cy-15,right,cy+15);
        fill(box,pal.field,9);stroke(box,pal.tile_edge,9);
        const auto k=key(L"seg");
        std::vector<D2D1_RECT_F> rs;float x=left+2;
        for(const float w:widths){rs.push_back(D2D1::RectF(x,cy-13,x+w,cy+13));x+=w;}
        for(size_t i=0;i<rs.size();++i) { // Hover fades in and out.
            const float a=spring(k+L"h"+std::to_wstring(i),over(rs[i])&&static_cast<int>(i)!=selected?1.0f:0.0f,600,48,.004f);
            if(a>.004f)fill(rs[i],fade(pal.hover,a),7);
        }
        float il=0,ir=0;
        if(selected>=0&&selected<static_cast<int>(rs.size())) {
            // The leading edge moves faster, so the indicator stretches as it travels.
            const auto& to=rs[static_cast<size_t>(selected)];
            const bool rightward=to.left>peek(k+L"l",to.left);
            il=spring(k+L"l",to.left,rightward?360.0f:620.0f,rightward?36.0f:46.0f);
            ir=spring(k+L"r",to.right,rightward?620.0f:360.0f,rightward?46.0f:36.0f);
            fill(D2D1::RectF(il,cy-13,ir,cy+13),pal.accent,7);
        }
        for(size_t i=0;i<rs.size();++i) {
            const auto& r=rs[i];
            const float covered=std::clamp((std::min(ir,r.right)-std::max(il,r.left))/(r.right-r.left),0.0f,1.0f);
            text(options[i],r,f_button.get(),mix(pal.ink_secondary,pal.on_accent,covered),DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(r,[pick,i](float,float){pick(static_cast<int>(i));});
        }
        return left;
    }
    float toggle(float right,float cy,bool on,std::function<void(bool)> set) {
        const auto r=D2D1::RectF(right-40,cy-10,right,cy+10);const auto k=key(L"tog");const bool hovered=over(r);
        const float off_x=r.left+10,on_x=r.right-10;
        const float kx=spring(k+L"x",on?on_x:off_x,560,34),p=std::clamp((kx-off_x)/(on_x-off_x),0.0f,1.0f);
        if(p<1){if(hovered)fill(r,fade(pal.hover,1-p),10);stroke(r,fade(pal.ink_secondary,1-p),10);}
        if(p>0)fill(r,fade(pal.accent,p),10);
        const float size=spring(k+L"s",hovered?7.0f:6.0f,600,40,.01f),stretch=spring(k+L"p",mouse_down&&hovered?5.0f:0.0f,700,42,.01f);
        const auto knob=on?D2D1::RectF(kx-size-stretch,cy-size,kx+size,cy+size):D2D1::RectF(kx-size,cy-size,kx+size+stretch,cy+size);
        fill(knob,mix(pal.ink_secondary,pal.on_accent,p),size);
        hit(D2D1::RectF(r.left-6,r.top-8,r.right+4,r.bottom+8),[set,on](float,float){set(!on);});
        return r.left;
    }
    float button(float right,float cy,const std::wstring& label,std::function<void()> act,bool primary=false,bool danger=false) {
        const float w=measure(label,f_button.get())+28;const auto r=D2D1::RectF(right-w,cy-15,right,cy+15);
        const auto k=key(L"btn");const bool hovered=over(r);
        const float lit=spring(k+L"h",hovered?1.0f:0.0f,600,48,.004f),press=spring(k+L"p",mouse_down&&hovered?.95f:1.0f,800,36,.001f);
        D2D1_MATRIX_3X2_F before;t->GetTransform(&before);
        t->SetTransform(D2D1::Matrix3x2F::Scale(press,press,D2D1::Point2F((r.left+r.right)/2,cy))*before);
        fill(r,primary?mix(pal.accent,theme::rgb(0xffffff),lit*.12f):mix(pal.hover,pal.hover_strong,lit),8);stroke(r,pal.tile_edge,8);
        text(label,r,f_button.get(),primary?pal.on_accent:(danger?(pal.light?theme::rgb(0xb8321f):theme::rgb(0xff9b8e)):pal.ink),DWRITE_TEXT_ALIGNMENT_CENTER);
        t->SetTransform(before);
        hit(r,[act](float,float){act();});
        return r.left;
    }
    float swatches(float right,float cy,const std::vector<std::pair<D2D1_COLOR_F,D2D1_COLOR_F>>& colors,int selected,std::function<void(int)> pick,std::function<void()> custom) {
        float x=right;const auto k=key(L"sw");
        const auto ring=[&](float cx,int index,bool on) {
            const float rr=spring(k+L"r"+std::to_wstring(index),on?15.5f:11.0f,520,30,.02f);
            if(rr>12.2f){color(fade(pal.ink,std::clamp((rr-12)/3.5f,0.0f,1.0f)));t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),rr,rr),brush.get(),2);}
        };
        const auto grow=[&](float cx,int index){return spring(k+L"g"+std::to_wstring(index),over(D2D1::RectF(cx-14,cy-14,cx+14,cy+14))?13.0f:12.0f,600,36,.01f);};
        if(custom) { // "+" opens Windows' color picker.
            const auto r=D2D1::RectF(x-26,cy-13,x,cy+13);const float g=grow(x-13,-1);
            color(over(r)?pal.hover_strong:pal.hover);t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x-13,cy),g,g),brush.get());
            ring(x-13,-1,selected<0);
            text(L"\xE710",r,f_icon_small.get(),pal.ink,DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(r,[custom](float,float){custom();});x-=34;
        }
        for(int i=static_cast<int>(colors.size())-1;i>=0;--i) {
            const float cx=x-13;
            ComPtr<ID2D1GradientStopCollection> stops;ComPtr<ID2D1LinearGradientBrush> g;
            const D2D1_GRADIENT_STOP s[]={{0,colors[static_cast<size_t>(i)].first},{1,colors[static_cast<size_t>(i)].second}};
            t->CreateGradientStopCollection(s,2,stops.put());
            t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx,cy-12),D2D1::Point2F(cx,cy+12)),stops.get(),g.put());
            const float size=grow(cx,i);
            if(g)t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),size,size),g.get());
            color(pal.tile_edge);t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),size,size),brush.get(),1);
            ring(cx,i,i==selected);
            hit(D2D1::RectF(cx-14,cy-14,cx+14,cy+14),[pick,i](float,float){pick(i);});
            x-=34;
        }
        return x;
    }
    // Disabled sliders are drawn faded and ignore the pointer.
    float slider(float right,float cy,int value,std::function<void(int)> set,bool enabled=true) {
        const float w=170,left=right-w;
        if(!enabled)t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),.35f),nullptr);
        text(L"Clear",D2D1::RectF(left-48,cy-10,left-8,cy+10),f_small.get(),pal.ink_secondary,DWRITE_TEXT_ALIGNMENT_TRAILING);
        const float track_left=left,track_right=right-42;
        text(L"Solid",D2D1::RectF(track_right+8,cy-10,right,cy+10),f_small.get(),pal.ink_secondary);
        fill(D2D1::RectF(track_left,cy-2,track_right,cy+2),pal.track,2);
        const float target=track_left+(track_right-track_left)*static_cast<float>(value)/100;const auto sk=key(L"sl");
        if(slider_drag)snap(sk,target);const float k=spring(sk,target,700,52);
        fill(D2D1::RectF(track_left,cy-2,k,cy+2),pal.accent,2);
        color(pal.light?theme::rgb(0xffffff):theme::rgb(0x2b2b31));t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k,cy),9,9),brush.get());
        color(pal.tile_edge);t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(k,cy),9,9),brush.get(),1);
        color(pal.accent);t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k,cy),slider_drag?5.5f:4.5f,slider_drag?5.5f:4.5f),brush.get());
        const auto at=[set,track_left,track_right](float x){set(static_cast<int>(std::lround(std::clamp((x-track_left)/(track_right-track_left),0.0f,1.0f)*100)));};
        if(enabled)hits.push_back({D2D1::RectF(track_left-10,cy-14,track_right+10,cy+14),[at](float x,float){at(x);},true,at});
        else t->PopLayer();
        return left-56;
    }
    // A single-line text field; `id` identifies which setting it edits.
    float field(float right,float cy,float width,int id,const std::wstring& value,const wchar_t* placeholder) {
        const auto r=D2D1::RectF(right-width,cy-16,right,cy+16);
        const bool active=editing==id;
        fill(r,pal.field,8);stroke(r,active?pal.accent:pal.tile_edge,8,active?1.5f:1);
        const auto& shown=active?edit_text:value;const auto inner=D2D1::RectF(r.left+10,r.top,r.right-10,r.bottom);
        if(active&&select_all&&!shown.empty()){const float w=std::min(measure(shown,f_button.get()),inner.right-inner.left);fill(D2D1::RectF(inner.left-2,cy-10,inner.left+w+2,cy+10),fade(pal.accent,.35f),3);}
        if(shown.empty())text(placeholder,inner,f_button.get(),pal.ink_muted);else text(shown,inner,f_button.get(),pal.ink);
        if(active&&!select_all&&(reduced||((now()-caret_start)/530)%2==0)) {
            const float x=std::min(inner.right,inner.left+measure(shown,f_button.get())+1);
            color(pal.accent);t->DrawLine(D2D1::Point2F(x,cy-9),D2D1::Point2F(x,cy+9),brush.get(),1.5f);
        }
        hit(r,[this,id,value](float,float){begin_edit(id,value);});
        return r.left;
    }
    float stepper(float right,float cy,const std::wstring& label,std::function<void(int)> step) {
        const auto r=D2D1::RectF(right-122,cy-15,right,cy+15);fill(r,pal.field,8);stroke(r,pal.tile_edge,8);
        const auto minus=D2D1::RectF(r.left,r.top,r.left+30,r.bottom),plus=D2D1::RectF(r.right-30,r.top,r.right,r.bottom);
        if(over(minus))fill(minus,pal.hover,8);if(over(plus))fill(plus,pal.hover,8);
        text(L"\xE738",minus,f_icon_small.get(),pal.ink_secondary,DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"\xE710",plus,f_icon_small.get(),pal.ink_secondary,DWRITE_TEXT_ALIGNMENT_CENTER);
        rolling_text(key(L"step"),label,D2D1::RectF(minus.right,r.top+1,plus.left,r.bottom-1),f_button.get(),pal.ink);
        hit(minus,[this,step](float,float){last_step=-1;step(-1);});hit(plus,[this,step](float,float){last_step=1;step(1);});
        return r.left;
    }
    // Chips that wrap within [left,right]; `selected` <0 for static chips.
    void chips(float left,float right,float top,const std::vector<std::wstring>& labels,int selected,std::function<void(int)> pick,int marked=-1) {
        float x=left,y=top;const auto k=key(L"chip");
        for(size_t i=0;i<labels.size();++i) {
            const std::wstring label=(static_cast<int>(i)==marked?L"\x2713 ":L"")+labels[i];
            const float w=measure(label,f_button.get())+22;
            if(x+w>right){x=left;y+=34;}
            const auto r=D2D1::RectF(x,y,x+w,y+28);
            const float on=spring(k+std::to_wstring(i),static_cast<int>(i)==selected?1.0f:0.0f,520,40,.004f);
            const float lit=pick?spring(k+L"h"+std::to_wstring(i),over(r)?1.0f:0.0f,600,48,.004f):0.0f;
            fill(r,mix(mix(pal.field,pal.hover_strong,lit),pal.accent,on),7);if(on<1)stroke(r,fade(pal.tile_edge,1-on),7);
            text(label,r,f_button.get(),mix(pal.ink,pal.on_accent,on),DWRITE_TEXT_ALIGNMENT_CENTER);
            if(pick)hit(r,[pick,i](float,float){pick(static_cast<int>(i));});
            x+=w+8;
        }
    }
    // ---- Rows and groups ----
    struct Row {std::wstring title,sub;float height=row_height;std::function<float(float right,float cy,float left,float top)> control;};
    // Draws a group of rows from y and returns the y below it.
    float group(float left,float right,float y,const std::vector<Row>& rows) {
        float h=0;for(const auto& r:rows)h+=r.height;
        const auto item=begin_item();
        const auto box=D2D1::RectF(left,y,right,y+h);fill(box,pal.tile,12);stroke(box,pal.tile_edge,12);
        float top=y;
        for(size_t i=0;i<rows.size();++i) {
            const auto& r=rows[i];
            if(i)fill(D2D1::RectF(left+16,top,right-16,top+1),pal.separator);
            const bool tall=r.height>row_height;
            const float cy=tall?top+26:top+r.height/2;
            float control_left=right-16;row_key=std::wstring(page_name(page))+L"|"+r.title;control_index=0;
            if(r.control)control_left=r.control(right-16,cy,left+16,top);
            const float title_right=tall?right-16:control_left-14;
            if(r.sub.empty())text(r.title,D2D1::RectF(left+16,cy-11,title_right,cy+11),f_body.get(),pal.ink);
            else {
                text(r.title,D2D1::RectF(left+16,cy-19,title_right,cy+1),f_body.get(),pal.ink);
                text(r.sub,D2D1::RectF(left+16,cy+1,title_right,cy+19),f_small.get(),pal.ink_secondary);
            }
            top+=r.height;
        }
        end_item(item);
        return top+group_gap;
    }
    // ---- Pages ----
    void changed_dock(){if(hooks.dock_changed)hooks.dock_changed(dock);pal=dock_palette(dock);live=dock_transparency(dock);redraw();}
    void changed_prayer(bool website){if(hooks.prayer_changed)hooks.prayer_changed(prayer,website);redraw();}
    float look_page(float left,float right,float y) {
        y=group(left,right,y,{
            {L"Theme",L"System follows your Windows light or dark mode.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"System",L"Light",L"Dark"},static_cast<int>(dock.theme),[this](int i){dock.theme=static_cast<DockSettings::Theme>(i);changed_dock();});}},
            {L"Accent color",L"Highlights, toggles and the active app.",row_height,[this](float r,float cy,float,float){
                static const unsigned presets[]={0x5fd1c0,0xf0c36b,0xf29bb6,0xb49cf2,0x8fe0a0};
                std::vector<std::pair<D2D1_COLOR_F,D2D1_COLOR_F>> c;auto windows=theme::palette(dock_light(dock));theme::apply_system_accent(windows);
                c.push_back({windows.accent,windows.accent});for(const auto p:presets)c.push_back({theme::rgb(p),theme::rgb(p)});
                int selected=dock.custom_accent?-1:0;for(int i=0;i<5;++i)if(dock.custom_accent&&dock.accent==presets[i])selected=i+1;
                return swatches(r,cy,c,selected,[this](int i){if(i==0)dock.custom_accent=false;else{static const unsigned p[]={0x5fd1c0,0xf0c36b,0xf29bb6,0xb49cf2,0x8fe0a0};dock.custom_accent=true;dock.accent=p[i-1];}changed_dock();},[this]{pick_color();});
            }},
        });
        y=group(left,right,y,{
            {L"Dock color",L"The tint of the glass. Smoke is the default.",row_height,[this](float r,float cy,float,float){
                static const unsigned tints[][2]={{0x26262c,0x0c0c0f},{0x16213a,0x080d1a},{0x17261f,0x08110d},{0x2a1a2c,0x120a14},{0x3a3125,0x1a150e},{0xfbfbfd,0xdcdce3}};
                std::vector<std::pair<D2D1_COLOR_F,D2D1_COLOR_F>> c;for(const auto& t2:tints)c.push_back({theme::rgb(t2[0]),theme::rgb(t2[1])});
                return swatches(r,cy,c,static_cast<int>(dock.tint),[this](int i){dock.tint=static_cast<DockSettings::Tint>(i);changed_dock();},nullptr);
            }},
            {L"Glass",dock.solid?L"Not used while solid colors are on.":L"How much of what is behind the dock shows through.",row_height,[this](float r,float cy,float,float){return slider(r,cy,dock.glass,[this](int v){if(v!=dock.glass){dock.glass=v;changed_dock();}},!dock.solid);}},
            {L"Use solid colors",L"No blur or transparency (also when Windows transparency is off).",row_height,[this](float r,float cy,float,float){return toggle(r,cy,dock.solid,[this](bool v){dock.solid=v;changed_dock();});}},
        });
        return y;
    }
    float dock_page(float left,float right,float y) {
        const bool bar=dock.style==DockSettings::Style::Bar;
        std::vector<Row> first={
            {L"Style",bar?L"A full-width bar like the Windows taskbar; windows stop above it.":L"A floating dock that hides over maximized and full-screen windows.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Floating dock",L"Full-width bar"},static_cast<int>(dock.style),[this](int i){dock.style=static_cast<DockSettings::Style>(i);changed_dock();});}},
        };
        if(!bar)first.push_back({L"Hide the dock",L"It slides away so it never covers app controls.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Over maximized windows",L"Always",L"Never"},static_cast<int>(dock.hide),[this](int i){dock.hide=static_cast<DockSettings::Hide>(i);changed_dock();});}});
        else {
            first.push_back({L"Auto-hide the bar",L"Off: the bar always shows and maximized windows stop above it.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,dock.bar_autohide,[this](bool v){dock.bar_autohide=v;changed_dock();});}});
            first.push_back({L"App alignment",L"",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Centre",L"Left"},dock.bar_left?1:0,[this](int i){dock.bar_left=i==1;changed_dock();});}});
        }
        first.push_back({L"Icon size",L"",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Small",L"Default",L"Large"},dock.icon==20?0:dock.icon==28?2:1,[this](int i){dock.icon=i==0?20:i==2?28:24;changed_dock();});}});
        first.push_back({L"Hover magnification",L"How much an icon grows under the pointer.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Off",L"Subtle",L"Default",L"Big"},dock.magnify==100?0:dock.magnify==112?1:dock.magnify==140?3:2,[this](int i){static const int v[]={100,112,122,140};dock.magnify=v[i];changed_dock();});}});
        first.push_back({L"Hide delay",L"How long the dock stays up after the pointer leaves, before it hides again.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"None",L"0.5 s",L"1 s",L"2 s"},dock.hide_delay==500?1:dock.hide_delay==1000?2:dock.hide_delay==2000?3:0,[this](int i){static const int v[]={0,500,1000,2000};dock.hide_delay=v[i];changed_dock();});}});
        first.push_back({L"Animations",L"Follow Windows uses its Animation effects setting.",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Follow Windows",L"Full",L"Reduced"},static_cast<int>(dock.motion),[this](int i){dock.motion=static_cast<DockSettings::Motion>(i);changed_dock();});}});
        y=group(left,right,y,first);
        y=group(left,right,y,{
            {L"Media controls",L"Previous, play and next with the current title.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,dock.media,[this](bool v){dock.media=v;changed_dock();});}},
            {L"Date under the clock",L"Shown when no prayer is coming up.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,dock.date,[this](bool v){dock.date=v;changed_dock();});}},
            {L"Clock",L"",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"12-hour",L"24-hour"},dock.clock24?1:0,[this](int i){dock.clock24=i==1;changed_dock();});}},
        });
        return y;
    }
    float prayer_page(float left,float right,float y) {
        const auto snap=hooks.prayer_status?hooks.prayer_status():PrayerSnapshot{};
        std::wstring status;
        if(prayer.website.empty())status=L"Add your masjid's website to show its prayer times.";
        else if(snap.method.rfind(L"Calculated",0)==0)status=L"Couldn't read the website ("+(snap.error.empty()?std::wstring(L"no times found"):snap.error)+L"). Showing calculated Adhan times.";
        else if(!snap.error.empty())status=L"Couldn't read today's times: "+snap.error+(snap.have?L". Showing the last times found.":L".");
        else if(snap.have)status=L"Found today's times"+(snap.method.empty()?std::wstring():L" with "+snap.method)+L". Updated "+snap.updated+L".";
        else status=L"Checking the website\x2026";
        const float grid_h=snap.have?64.0f:0;
        y=group(left,right,y,{
            {L"Masjid website",L"",row_height,[this](float r,float cy,float,float){
                const float b=button(r,cy,L"Check now",[this]{commit_edit();if(hooks.check_prayers)hooks.check_prayers();});
                return field(b-8,cy,300,1,prayer.website,L"https://your-masjid.org");
            }},
            {status,L"",row_height+grid_h-14,[this,snap](float,float cy,float l,float top){
                (void)cy;
                if(snap.have) {
                    SYSTEMTIME now_time{};GetLocalTime(&now_time);const bool friday=now_time.wDayOfWeek==5;
                    const auto order=daily_salah(snap.day,friday);
                    const Salah cells[]={order[0],Salah::Sunrise,order[1],order[2],order[3],order[4]};
                    const float w=(right_edge-l-16-5*6)/6;
                    for(int i=0;i<6;++i) {
                        const auto& time=snap.day[cells[i]];const float x=l+static_cast<float>(i)*(w+6);
                        const auto box=D2D1::RectF(x,top+42,x+w,top+42+48);fill(box,pal.field,8);
                        text(salah_name(cells[i]),D2D1::RectF(x+8,box.top+3,box.right-4,box.top+18),f_small.get(),pal.ink_secondary);
                        text(clock_text(cells[i]==Salah::Sunrise?time.begin:time.adhan),D2D1::RectF(x+8,box.top+16,box.right-4,box.top+32),f_button.get(),pal.ink);
                        text(cells[i]==Salah::Sunrise||time.iqama<0?std::wstring(L"\x2014"):L"Iqama "+clock_text(time.iqama,false),D2D1::RectF(x+8,box.top+31,box.right-4,box.bottom-2),f_small.get(),pal.ink_secondary);
                    }
                }
                return right_edge;
            }},
        });
        // How times are read.
        std::vector<std::wstring> methods;
        for(int m=0;m<static_cast<int>(PrayerMethod::Count);++m)methods.push_back(prayer_method_name(static_cast<PrayerMethod>(m)));
        int marked=-1;for(size_t i=1;i<methods.size();++i)if(snap.method==methods[i])marked=static_cast<int>(i);
        y=group(left,right,y,{
            {L"How times are read",L"Automatic tries each method until one gives a complete, sensible day.",chip_row_height+34,[this,methods,marked](float,float,float l,float top){
                chips(l,right_edge-16,top+48,methods,prayer.method,[this](int i){prayer.method=i;changed_prayer(true);},marked);return right_edge;}},
            {L"Calculate if the website fails",L"Adhan times from your location (ISNA method). Iqama needs the website.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,prayer.calculate,[this](bool v){prayer.calculate=v;changed_prayer(false);});}},
            {L"Location",L"Latitude, longitude for calculated times.",row_height,[this](float r,float cy,float,float){wchar_t place[64]{};swprintf(place,64,L"%.4f, %.4f",prayer.latitude,prayer.longitude);return field(r,cy,190,2,place,L"43.6532, -79.3832");}},
            {L"When times are checked",L"When the day starts and every 3 hours; every 15 minutes while the site is down.",row_height,nullptr},
        });
        y=group(left,right,y,{
            {L"Next prayer on the clock",L"\x201C" L"Asr in 15 min\x201D replaces the date as it gets close.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,prayer.clock,[this](bool v){prayer.clock=v;changed_prayer(false);});}},
            {L"Adhan card",L"A small card at the top of the screen, also over games, counting down to Iqama.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,prayer.reminders,[this](bool v){prayer.reminders=v;changed_prayer(false);});}},
            {L"Chime",L"",row_height,[this](float r,float cy,float,float){return segmented(r,cy,{L"Calendar",L"Soft bell",L"None"},prayer.chime,[this](int i){prayer.chime=i;changed_prayer(false);play_chime();});}},
            {L"Iqama reminder",L"Shows the card again before Iqama if you dismissed it.",row_height,[this](float r,float cy,float,float){return stepper(r,cy,prayer.iqama_lead?std::to_wstring(prayer.iqama_lead)+L" min":std::wstring(L"Off"),[this](int d){prayer.iqama_lead=std::clamp(prayer.iqama_lead+d*5,0,30);changed_prayer(false);});}},
            {L"Sunrise reminder",L"15 minutes before Fajr time ends.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,prayer.sunrise,[this](bool v){prayer.sunrise=v;changed_prayer(false);});}},
        });
        return y;
    }
    float launch_page(float left,float right,float y) {
        std::vector<Row> rows;
        if(hooks.windows_key)rows.push_back({L"Windows key opens MinimaDock",L"Shortcuts like Win+E keep working; full-screen games keep Start.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,hooks.windows_key(),[this](bool v){if(hooks.set_windows_key)hooks.set_windows_key(v);redraw();});}});
        rows.push_back({L"Search in",L"Apps, open windows, files, Settings pages and actions. Everything stays on this PC.",row_height,nullptr});
        y=group(left,right,y,rows);
        y=group(left,right,y,{
            {L"Quick web searches",L"Type the prefix, a space and your words. Nothing is sent until you press Enter.",chip_row_height,[this](float,float,float l,float top){
                chips(l,right_edge-16,top+50,{L"yt  YouTube",L"g  Google",L"gh  GitHub",L"wiki  Wikipedia",L"maps  Google Maps"},-1,nullptr);return right_edge;}},
            {L"Search history",L"Used to rank the apps you open often. Kept on this PC.",row_height,[this](float r,float cy,float,float){return button(r,cy,L"Clear history",[this]{if(hooks.clear_history)hooks.clear_history();});}},
        });
        return y;
    }
    float about_page(float left,float right,float y) {
        if(hooks.update_status) {
            const auto u=hooks.update_status();
            std::wstring status;
            if(u.checking)status=L"Checking for updates\x2026";
            else if(u.available)status=L"Version "+u.latest+L" is available.";
            else if(!u.error.empty())status=L"Couldn't check for updates: "+u.error+L".";
            else if(u.checked)status=L"You're up to date. Checked at "+u.checked_at+L".";
            else status=L"Not checked yet.";
            y=group(left,right,y,{
                {L"MinimaDock "+hooks.version,status,row_height,[this,u](float r,float cy,float,float){
                    if(u.available)return button(r,cy,L"Update to "+u.latest,[this]{if(hooks.install_update)hooks.install_update();},true);
                    return button(r,cy,L"Check now",[this]{if(hooks.check_updates)hooks.check_updates();redraw();});
                }},
                {L"Check for updates automatically",L"Once a day from GitHub. Updates install only when you choose.",row_height,[this](float r,float cy,float,float){
                    return toggle(r,cy,hooks.auto_update&&hooks.auto_update(),[this](bool v){if(hooks.set_auto_update)hooks.set_auto_update(v);redraw();});}},
            });
        }
        std::vector<Row> rows;
        if(hooks.start_with_windows)rows.push_back({L"Start with Windows",L"MinimaDock replaces the taskbar each time you sign in.",row_height,[this](float r,float cy,float,float){return toggle(r,cy,hooks.start_with_windows(),[this](bool v){if(hooks.set_start_with_windows)hooks.set_start_with_windows(v);redraw();});}});
        if(hooks.return_to_windows)rows.push_back({L"Return to the Windows taskbar",L"Until you sign in again (Ctrl+Alt+Backspace). Pins and settings are kept.",row_height,[this](float r,float cy,float,float){return button(r,cy,L"Return now",[this]{hooks.return_to_windows();});}});
        rows.push_back({L"Reset appearance",L"Theme, colors and glass go back to the defaults.",row_height,[this](float r,float cy,float,float){return button(r,cy,L"Reset",[this]{dock=default_appearance(dock);changed_dock();});}});
        rows.push_back({L"Reset everything",L"All settings, dock-only pins and search history.",row_height,[this](float r,float cy,float,float){return button(r,cy,L"Reset everything",[this]{
            if(MessageBoxW(hwnd,L"Reset all MinimaDock settings, dock-only pins and search history?",L"MinimaDock",MB_OKCANCEL|MB_ICONWARNING)!=IDOK)return;
            if(hooks.reset_everything)hooks.reset_everything();dock=DockSettings{};prayer=PrayerSettings{};pal=dock_palette(dock);redraw();},false,true);}});
        return group(left,right,y,rows);
    }
    float right_edge=0;
    // ---- Frame ----
    void switch_page(Page p) {
        commit_edit();if(page==p)return;
        page=p;scroll=scroll_target=0;springs.erase(L"scroll");page_changed=now();redraw();
    }
    void draw(D2D1_MATRIX_3X2_F base) {
        hits.clear();
        const auto tick=now();frame_dt=last_frame>0?std::clamp(static_cast<float>(tick-last_frame)/1000,0.0f,1.0f/30):0;last_frame=tick;animating=false;
        const auto card=D2D1::RectF(margin_x,margin_top,margin_x+window_width,margin_top+window_height);
        t->SetTransform(base);
        if(!painter_bound){painter.palette=pal;painter.bind(t,factory.get());painter_bound=true;}
        painter.palette=pal;painter.live=composed&&live;painter.origin=base;painter.begin_frame();
        painter.paint(card,radius,GlassKind::Panel);
        // Sidebar.
        fill(D2D1::RectF(card.left+side_width,card.top+14,card.left+side_width+1,card.bottom-14),pal.separator);
        {   // Mark and name.
            const float mx=card.left+30,my=card.top+38;color(pal.ink);
            const float s=26.0f/24;
            const D2D1_RECT_F bars[]={{4.5f,4,7.7f,15.5f},{10.4f,8,13.6f,15.5f},{16.3f,4,19.5f,15.5f},{4.5f,17.8f,19.5f,20.4f}};
            for(const auto& b:bars){const auto r=D2D1::RectF(mx-13+b.left*s,my-13+b.top*s,mx-13+b.right*s,my-13+b.bottom*s);t->FillRoundedRectangle(D2D1::RoundedRect(r,1.6f*s,1.6f*s),brush.get());}
            text(L"MinimaDock",D2D1::RectF(mx+20,my-17,card.left+side_width-8,my+1),f_strong.get(),pal.ink);
            text(L"Version "+hooks.version,D2D1::RectF(mx+20,my+1,card.left+side_width-8,my+17),f_small.get(),pal.ink_secondary);
        }
        {   // Pages: one highlight glides to the selected page, another follows the pointer.
            const float sl=card.left+10,sr=card.left+side_width-10;
            const auto item_y=[&](size_t i){return card.top+78+static_cast<float>(i)*40;};
            size_t selected=0;int hovered=-1;
            for(size_t i=0;i<std::size(pages);++i){if(pages[i]==page)selected=i;else if(over(D2D1::RectF(sl,item_y(i),sr,item_y(i)+38)))hovered=static_cast<int>(i);}
            const float shown=spring(L"side.hover",hovered>=0?1.0f:0.0f,600,48,.004f);
            if(hovered>=0&&shown<.08f)snap(L"side.hover.y",item_y(static_cast<size_t>(hovered)));
            const float hy=spring(L"side.hover.y",hovered>=0?item_y(static_cast<size_t>(hovered)):peek(L"side.hover.y",0),560,44);
            if(shown>.004f)fill(D2D1::RectF(sl,hy,sr,hy+38),fade(pal.hover,shown),9);
            const float target=item_y(selected),sy=spring(L"side.selected",target,480,38);
            fill(D2D1::RectF(sl,sy,sr,sy+38),pal.hover_strong,9);
            const float bar=8+std::min(std::abs(target-sy)*.35f,12.0f); // Stretches while travelling.
            fill(D2D1::RectF(sl+2,sy+19-bar,sl+5,sy+19+bar),pal.accent,1.5f);
            for(size_t i=0;i<std::size(pages);++i) {
                const auto p=pages[i];const auto r=D2D1::RectF(sl,item_y(i),sr,item_y(i)+38);
                const float e=ease_out(std::clamp((since(opened)-static_cast<float>(i)*40)/320,0.0f,1.0f));if(e<1)animating=true;
                const float nudge=spring(L"side.nudge"+std::to_wstring(i),static_cast<int>(i)==hovered?2.0f:0.0f,600,40,.01f)-(1-e)*10;
                text(std::wstring(1,page_glyph(p)),D2D1::RectF(r.left+12+nudge,r.top,r.left+36+nudge,r.bottom),f_icon.get(),fade(pal.ink,e),DWRITE_TEXT_ALIGNMENT_CENTER);
                text(page_name(p),D2D1::RectF(r.left+44+nudge,r.top,r.right-6,r.bottom),f_body.get(),fade(pal.ink,e));
                hit(r,[this,p](float,float){switch_page(p);});
            }
        }
        text(L"Changes save as you go.",D2D1::RectF(card.left+22,card.bottom-46,card.left+side_width-10,card.bottom-22),f_small.get(),pal.ink_secondary);
        // Close button.
        const auto close_box=D2D1::RectF(card.right-46,card.top+8,card.right-10,card.top+40);
        if(over(close_box))fill(close_box,pal.light?theme::rgb(0xc42b1c,.9f):theme::rgb(0xc42b1c,.9f),8);
        text(L"\xE8BB",close_box,f_icon_small.get(),over(close_box)?theme::rgb(0xffffff):pal.ink,DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(close_box,[this](float,float){commit_edit();PostMessageW(hwnd,WM_CLOSE,0,0);});
        // Page, scrolled (smoothly) and clipped; its parts fade up in turn when switched.
        const float left=card.left+side_width+pad,right=card.right-pad;right_edge=right;
        const auto clip=D2D1::RectF(card.left+side_width+1,card.top+12,card.right-2,card.bottom-2);
        t->PushAxisAlignedClip(clip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        scroll=spring(L"scroll",scroll_target,320,38,.1f);
        D2D1_MATRIX_3X2_F before;t->GetTransform(&before);
        t->SetTransform(D2D1::Matrix3x2F::Translation(0,-scroll)*before);
        const float saved_py=py;py+=scroll; // Hover tests in page coordinates.
        float y=card.top+30;item_index=0;
        const size_t first_hit=hits.size();
        {
            const auto item=begin_item();
            text(page_name(page),D2D1::RectF(left,y,right-50,y+30),f_title.get(),pal.ink);y+=32;
            if(*page_lede(page)){text(page_lede(page),D2D1::RectF(left,y,right,y+20),f_small.get(),pal.ink_secondary);y+=34;}else y+=14;
            end_item(item);
        }
        switch(page) {
        case Page::Look:y=look_page(left,right,y);break;
        case Page::Dock:y=dock_page(left,right,y);break;
        case Page::Prayer:y=prayer_page(left,right,y);break;
        case Page::Launch:y=launch_page(left,right,y);break;
        default:y=about_page(left,right,y);break;
        }
        py=saved_py;
        // Page hits move with the scroll.
        for(size_t i=first_hit;i<hits.size();++i){hits[i].r.top-=scroll;hits[i].r.bottom-=scroll;}
        std::erase_if(hits,[&](const Hit& h){return h.r.bottom<clip.top||h.r.top>clip.bottom;});
        content_height=y-(card.top+30);
        t->SetTransform(before);t->PopAxisAlignedClip();
        // Scroll indicator.
        const float view=clip.bottom-clip.top;
        if(content_height>view) {
            const float bar_h=std::max(30.0f,view*view/content_height),bar_y=clip.top+(view-bar_h)*scroll/std::max(1.0f,content_height-view);
            fill(D2D1::RectF(card.right-7,bar_y,card.right-4,bar_y+bar_h),pal.track,1.5f);
        }
    }
    void clamp_scroll() {
        const float view=window_height-46;scroll_target=std::clamp(scroll_target,0.0f,std::max(0.0f,content_height-view+20));
    }
    // ---- Rendering paths ----
    void render() {
        if(!hwnd)return;
        if(composed) {
            if(!host.attached()){host.attach(hwnd,factory.get());painter_bound=false;}
            D2D1_MATRIX_3X2_F origin{};auto* context=host.begin(pixel_width,pixel_height,static_cast<float>(dpi),origin);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);t=context;
            if(host.allocations()!=host_generation||!brush){host_generation=host.allocations();brush.reset();painter_bound=false;context->CreateSolidColorBrush(pal.ink,brush.put());}
            else if(painter_bound)painter.retarget(context);
            draw(origin);
            host.end();host.backdrops(painter.shapes,static_cast<float>(dpi));host.present(1,0,live);
            t=nullptr;
        } else {
            if(!hwnd_target) {
                if(FAILED(factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),static_cast<float>(dpi),static_cast<float>(dpi)),
                    D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(pixel_width,pixel_height)),hwnd_target.put())))return;
                brush.reset();painter_bound=false;hwnd_target->CreateSolidColorBrush(pal.ink,brush.put());
            }
            t=hwnd_target.get();hwnd_target->BeginDraw();hwnd_target->Clear(pal.light?theme::rgb(0xe8e8ed):theme::rgb(0x18181c));
            draw(D2D1::Matrix3x2F::Identity());
            if(hwnd_target->EndDraw()==D2DERR_RECREATE_TARGET){hwnd_target.reset();brush.reset();}
            t=nullptr;
        }
    }
    void redraw(){if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);}
    void frames(){if(!hwnd)return;if(animating)SetTimer(hwnd,2,10,nullptr);else KillTimer(hwnd,2);}
    // ---- Editing ----
    void begin_edit(int id,const std::wstring& value) {
        if(editing==id)return;
        commit_edit();editing=id;edit_text=value;select_all=true;caret_start=now();SetTimer(hwnd,1,530,nullptr);redraw();
    }
    void commit_edit() {
        if(!editing)return;
        const int id=editing;editing=0;KillTimer(hwnd,1);
        if(id==1) {
            auto value=edit_text;while(!value.empty()&&value.back()==L' ')value.pop_back();while(!value.empty()&&value.front()==L' ')value.erase(value.begin());
            if(!value.empty()&&value.find(L"://")==std::wstring::npos)value=L"https://"+value;
            if(!value.empty()&&value!=prayer.website){prayer.website=value;changed_prayer(true);}
        } else if(id==2) {
            double latitude=0,longitude=0;
            if(swscanf(edit_text.c_str(),L"%lf , %lf",&latitude,&longitude)==2&&std::abs(latitude)<=90&&std::abs(longitude)<=180){prayer.latitude=latitude;prayer.longitude=longitude;changed_prayer(false);}
        }
        redraw();
    }
    void key_char(wchar_t c) {
        if(!editing)return;
        if(c==8){if(select_all)edit_text.clear();else if(!edit_text.empty())edit_text.pop_back();select_all=false;}
        else if(c==13){commit_edit();return;}
        else if(c==27){editing=0;KillTimer(hwnd,1);}
        else if(c==1){select_all=true;}                 // Ctrl+A
        else if(c==22){if(select_all)edit_text.clear();select_all=false;paste();} // Ctrl+V
        else if(c>=32&&edit_text.size()<300){if(select_all)edit_text.clear();select_all=false;edit_text.push_back(c);}
        caret_start=now();redraw();
    }
    void paste() {
        if(!OpenClipboard(hwnd))return;
        if(const auto data=GetClipboardData(CF_UNICODETEXT))if(const auto s=static_cast<const wchar_t*>(GlobalLock(data))) {
            for(const wchar_t* c=s;*c&&*c!=L'\r'&&*c!=L'\n'&&edit_text.size()<300;++c)edit_text.push_back(*c);
            GlobalUnlock(data);
        }
        CloseClipboard();
    }
    void pick_color() {
        static COLORREF custom[16]{};
        CHOOSECOLORW choose{};choose.lStructSize=sizeof(choose);choose.hwndOwner=hwnd;choose.lpCustColors=custom;choose.Flags=CC_FULLOPEN|CC_RGBINIT;
        const unsigned a=dock.accent;choose.rgbResult=RGB((a>>16)&255,(a>>8)&255,a&255);
        if(!ChooseColorW(&choose))return;
        const auto c=choose.rgbResult;dock.custom_accent=true;dock.accent=(static_cast<unsigned>(GetRValue(c))<<16)|(static_cast<unsigned>(GetGValue(c))<<8)|GetBValue(c);
        changed_dock();
    }
    void play_chime() {
        if(prayer.chime==2)return;
        wchar_t folder[MAX_PATH]{};GetWindowsDirectoryW(folder,MAX_PATH);
        PlaySoundW((std::wstring(folder)+chime_file(prayer.chime)).c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT);
    }
    // ---- Window ----
    D2D1_POINT_2F point(LPARAM l) const{const float s=static_cast<float>(dpi)/96;return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(l))/s,static_cast<float>(GET_Y_LPARAM(l))/s);}
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));self->hwnd=window;}
        if(!self)return DefWindowProcW(window,message,w,l);
        switch(message) {
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window,&p);
            const float s=static_cast<float>(self->dpi)/96,x=static_cast<float>(p.x)/s,y=static_cast<float>(p.y)/s;
            // Drag by the top strip (and the brand area), except over the close button.
            if(y>=margin_top&&y<margin_top+44&&x<margin_x+window_width-50)return HTCAPTION;
            if(y>=margin_top&&y<margin_top+64&&x<margin_x+side_width)return HTCAPTION;
            return HTCLIENT;
        }
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);self->render();EndPaint(window,&ps);self->frames();return 0;}
        case WM_MOUSEMOVE: {
            const auto p=self->point(l);self->px=p.x;self->py=p.y;
            if(self->slider_drag){for(const auto& h:self->hits)if(h.slider&&h.drag){h.drag(p.x);break;}}
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);
            self->redraw();return 0;
        }
        case WM_MOUSELEAVE:self->px=self->py=-1;self->mouse_down=false;self->redraw();return 0;
        case WM_LBUTTONDOWN: {
            const auto p=self->point(l);bool on_field=false;self->mouse_down=true;
            for(auto it=self->hits.rbegin();it!=self->hits.rend();++it)if(p.x>=it->r.left&&p.x<it->r.right&&p.y>=it->r.top&&p.y<it->r.bottom) {
                if(it->slider){self->slider_drag=true;SetCapture(window);}
                const auto click=it->click;
                const int before=self->editing;click(p.x,p.y);on_field=self->editing!=0&&self->editing!=before;if(self->editing&&self->editing==before)on_field=true;
                break;
            }
            if(!on_field)self->commit_edit();
            self->redraw();return 0;
        }
        case WM_LBUTTONUP:self->mouse_down=false;if(self->slider_drag){self->slider_drag=false;ReleaseCapture();}self->redraw();return 0;
        case WM_MOUSEWHEEL:self->scroll_target-=static_cast<float>(GET_WHEEL_DELTA_WPARAM(w))/120*72;self->clamp_scroll();self->redraw();return 0;
        case WM_CHAR:self->key_char(static_cast<wchar_t>(w));return 0;
        case WM_KEYDOWN:
            if(w==VK_ESCAPE&&!self->editing){PostMessageW(window,WM_CLOSE,0,0);return 0;}
            if(w==VK_TAB&&!self->editing) { // Tab / Shift+Tab: next or previous section.
                const int count=static_cast<int>(std::size(pages));int index=static_cast<int>(self->page);
                index=(index+((GetKeyState(VK_SHIFT)&0x8000)?count-1:1))%count;self->switch_page(pages[index]);return 0;
            }
            return 0;
        case WM_TIMER:if(w==1||w==2)self->redraw();return 0;
        case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)self->commit_edit();return 0;
        case WM_CLOSE:self->commit_edit();DestroyWindow(window);return 0;
        case WM_DPICHANGED:{self->dpi=HIWORD(w);const auto* r=reinterpret_cast<const RECT*>(l);self->size_to_dpi();SetWindowPos(window,nullptr,r->left,r->top,static_cast<int>(self->pixel_width),static_cast<int>(self->pixel_height),SWP_NOZORDER|SWP_NOACTIVATE);self->hwnd_target.reset();self->redraw();return 0;}
        case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);self->host.detach();self->hwnd=nullptr;self->hwnd_target.reset();self->brush.reset();self->painter_bound=false;break;
        default:break;
        }
        return DefWindowProcW(window,message,w,l);
    }
    void size_to_dpi() {
        const float s=static_cast<float>(dpi)/96;
        pixel_width=static_cast<UINT>(std::ceil((window_width+2*margin_x)*s));pixel_height=static_cast<UINT>(std::ceil((window_height+margin_top+margin_bottom)*s));
        if(hwnd) { // Clicks and drawing stay on the card (plus its soft shadow).
            if(auto region=CreateRoundRectRgn(static_cast<int>((margin_x-2)*s),static_cast<int>((margin_top-2)*s),static_cast<int>((margin_x+window_width+3)*s),static_cast<int>((margin_top+window_height+6)*s),static_cast<int>(30*s),static_cast<int>(30*s)))
                if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
        }
    }
    void open(HWND owner_window,const DockSettings& d,const PrayerSettings& p,CustomizeHooks h,bool reduced_motion) {
        (void)owner_window;
        dock=d;prayer=p;hooks=std::move(h);reduced=reduced_motion;pal=dock_palette(dock);live=dock_transparency(dock);
        if(hwnd){ShowWindow(hwnd,SW_SHOWNORMAL);SetForegroundWindow(hwnd);redraw();return;}
        if(!resources())return;
        composed=GlassHost::available();
        WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.Customize";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        // On the monitor with the pointer, centred.
        POINT cursor{};GetCursorPos(&cursor);const auto monitor=MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};info.cbSize=sizeof(info);GetMonitorInfoW(monitor,&info);
        UINT x_dpi=96,y_dpi=96;dpi=96;
        if(const auto shcore=GetModuleHandleW(L"shcore.dll"))if(const auto get=reinterpret_cast<HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*)>(reinterpret_cast<void*>(GetProcAddress(shcore,"GetDpiForMonitor"))))if(SUCCEEDED(get(monitor,0,&x_dpi,&y_dpi)))dpi=x_dpi;
        size_to_dpi();
        const int x=(info.rcWork.left+info.rcWork.right-static_cast<int>(pixel_width))/2,y=(info.rcWork.top+info.rcWork.bottom-static_cast<int>(pixel_height))/2;
        hwnd=CreateWindowExW((composed?WS_EX_NOREDIRECTIONBITMAP:0)|WS_EX_APPWINDOW,klass.lpszClassName,L"MinimaDock settings",WS_POPUP|WS_MINIMIZEBOX,
            x,y,static_cast<int>(pixel_width),static_cast<int>(pixel_height),nullptr,nullptr,klass.hInstance,this);
        if(!hwnd)return;
        dpi=GetDpiForWindow(hwnd);size_to_dpi();
        SetWindowPos(hwnd,nullptr,0,0,static_cast<int>(pixel_width),static_cast<int>(pixel_height),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        opened=now();page_changed=now();
        ShowWindow(hwnd,SW_SHOWNORMAL);SetForegroundWindow(hwnd);redraw();
    }
};

CustomizeWindow::CustomizeWindow():impl_(std::make_unique<Impl>()){}
CustomizeWindow::~CustomizeWindow(){close();}
void CustomizeWindow::open(HWND owner,const DockSettings& dock,const PrayerSettings& prayer,CustomizeHooks hooks,bool reduced){impl_->open(owner,dock,prayer,std::move(hooks),reduced);}
void CustomizeWindow::close(){if(impl_->hwnd)DestroyWindow(impl_->hwnd);}
bool CustomizeWindow::visible() const{return impl_->hwnd!=nullptr;}
void CustomizeWindow::prayer_updated(){impl_->redraw();}
void CustomizeWindow::refresh(const DockSettings& dock,const PrayerSettings& prayer,bool reduced) {
    auto& i=*impl_;if(!i.hwnd)return;
    i.dock=dock;i.prayer=prayer;i.reduced=reduced;i.pal=dock_palette(dock);i.live=dock_transparency(dock);i.redraw();
}

int save_customize_snapshot(const std::wstring& path,int page) {
    CustomizeWindow::Impl ui;
    if(!ui.resources())return 1;
    ComPtr<IWICImagingFactory> wic;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put()))))return 1;
    ui.dock=load_dock_settings();ui.prayer=PrayerSettings{};ui.pal=dock_palette(ui.dock);ui.live=false;ui.reduced=true;ui.composed=false;
    ui.page=pages[std::clamp(page,0,4)];ui.hooks.version=app_version;
    ui.hooks.windows_key=[]{return true;};ui.hooks.return_to_windows=[]{};ui.hooks.start_with_windows=[]{return true;};
    ui.hooks.prayer_status=[]{
        PrayerSnapshot s;s.have=true;s.updated=L"9:02 AM";s.method=L"GalaxyStream widget";
        const int t[][3]={{385,385,400},{452,452,-1},{810,810,825},{1020,1020,1030},{1117,1117,1120},{1230,1230,1245}};
        const Salah order[]={Salah::Fajr,Salah::Sunrise,Salah::Zuhr,Salah::Asr,Salah::Maghrib,Salah::Isha};
        for(int i=0;i<6;++i)s.day.times[static_cast<size_t>(order[i])]={t[i][0],t[i][1],t[i][2]};
        return s;
    };
    const float scale=2;const UINT w=static_cast<UINT>((window_width+2*margin_x)*scale),h=static_cast<UINT>((window_height+margin_top+margin_bottom)*scale);
    ComPtr<IWICBitmap> bitmap;ComPtr<ID2D1RenderTarget> target;
    if(FAILED(wic->CreateBitmap(w,h,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()))||
       FAILED(ui.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(),192,192),target.put())))return 1;
    target->CreateSolidColorBrush(ui.pal.ink,ui.brush.put());ui.t=target.get();
    target->BeginDraw();target->Clear(D2D1::ColorF(0x2a4f7a));ui.draw(D2D1::Matrix3x2F::Identity());
    if(FAILED(target->EndDraw()))return 1;
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    if(FAILED(wic->CreateStream(stream.put()))||FAILED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))||
       FAILED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()))||FAILED(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(frame.put(),nullptr))||FAILED(frame->Initialize(nullptr))||FAILED(frame->SetSize(w,h)))return 1;
    auto format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetPixelFormat(&format))||FAILED(frame->WriteSource(bitmap.get(),nullptr))||FAILED(frame->Commit())||FAILED(encoder->Commit()))return 1;
    return 0;
}
}
