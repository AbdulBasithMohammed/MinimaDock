#include "rendering/calendar.h"
#include "rendering/theme.h"
#include "rendering/glass.h"
#include "rendering/glass_style.h"
#include "rendering/dock_settings.h"
#include "platform/com_ptr.h"
#include "taskbar/calendar_model.h"
#include "taskbar/state_machine.h"
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace minima {
namespace {
constexpr float canvas_width=360,calendar_height=448,prayer_height=244;
constexpr UINT_PTR motion_timer=1,day_timer=2;
void require(HRESULT hr){if(FAILED(hr))throw hr;}
CalendarDate local_date(){SYSTEMTIME time{};GetLocalTime(&time);return {time.wYear,time.wMonth,time.wDay};}
SYSTEMTIME system_date(CalendarDate d){SYSTEMTIME time{};time.wYear=static_cast<WORD>(d.year);time.wMonth=static_cast<WORD>(d.month);time.wDay=static_cast<WORD>(d.day);return time;}
std::wstring date_label(CalendarDate d,const wchar_t* pattern){auto time=system_date(d);wchar_t value[128]{};GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,&time,pattern,value,128,nullptr);return value;}
bool contains(D2D1_RECT_F r,float x,float y){return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;}
D2D1_RECT_F previous_button(){return D2D1::RectF(268,72,298,104);}
D2D1_RECT_F next_button(){return D2D1::RectF(304,72,334,104);}
D2D1_RECT_F today_button(){return D2D1::RectF(24,402,114,430);}
D2D1_RECT_F close_button(){return D2D1::RectF(310,22,338,50);}
D2D1_RECT_F website_button(){return D2D1::RectF(300,448,334,476);}
constexpr float grid_left=24,grid_top=144,column_width=312.0f/7,row_height=42;
struct CalendarPainter {
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> material,sheen;
    std::array<ComPtr<IDWriteTextFormat>,3> fonts;
    CalendarModel model{local_date()};
    CalendarSlide slide;
    int old_year=model.year(),focus=-1;
    unsigned old_month=model.month();
    float pointer_x=-1,pointer_y=-1;
    bool reduced=false,transparency=true,high_contrast=false;
    // Same smoked-crystal recipe and palette as the dock.
    theme::Palette pal=theme::palette(false);
    GlassPainter glass;
    bool live=false,external=false;
    D2D1_MATRIX_3X2_F origin=D2D1::Matrix3x2F::Identity();
    PrayerView prayer;
    float height() const{return calendar_height+(prayer.visible?prayer_height:0);}
    static D2D1_COLOR_F fade(D2D1_COLOR_F c,float a){c.a*=a;return c;}
    void appearance() {
        HIGHCONTRASTW contrast{};contrast.cbSize=sizeof(contrast);SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);high_contrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        const auto settings=load_dock_settings();transparency=dock_transparency(settings);pal=dock_palette(settings);
    }
    void initialize() {
        require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
        require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write.put())));
        require(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IDWriteFontCollection> collection;require(write->GetSystemFontCollection(collection.put()));const auto family=theme::text_family(collection.get());
        const float sizes[]={12,13,15};
        for(size_t i=0;i<fonts.size();++i){require(write->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_LIGHT,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,sizes[i],L"",fonts[i].put()));fonts[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);fonts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);}
        appearance();
    }
    void bind(ID2D1RenderTarget* value){
        target.reset();value->AddRef();*target.put()=value;require(value->CreateSolidColorBrush(theme::primary(),brush.put()));value->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ComPtr<ID2D1GradientStopCollection> collection;const auto stops=theme::material_stops();require(value->CreateGradientStopCollection(stops.data(),static_cast<UINT32>(stops.size()),collection.put()));
        require(value->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(10,10),D2D1::Point2F(10,438)),collection.get(),material.put()));
        const auto highlights=theme::sheen_stops();require(value->CreateGradientStopCollection(highlights.data(),static_cast<UINT32>(highlights.size()),collection.put()));
        require(value->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(10,10),D2D1::Point2F(10,10+428*.54f)),collection.get(),sheen.put()));
        glass.palette=pal;glass.live=live;glass.bind(value,factory.get());
    }
    void retarget(ID2D1RenderTarget* value){target.reset();value->AddRef();*target.put()=value;glass.retarget(value);}
    void color(D2D1_COLOR_F c){brush->SetColor(c);}
    void rect(D2D1_RECT_F r,D2D1_COLOR_F c,float radius=0){color(c);target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush.get());}
    void outline(D2D1_RECT_F r,D2D1_COLOR_F c,float radius=0){color(c);target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush.get(),1);}
    void text(const std::wstring& value,D2D1_RECT_F r,int font,D2D1_COLOR_F c,bool center=false){fonts[font]->SetTextAlignment(center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);color(c);target->DrawTextW(value.c_str(),static_cast<UINT32>(value.size()),fonts[font].get(),r,brush.get());}
    void navigate(int delta,std::int64_t time){old_year=model.year();old_month=model.month();if(model.navigate(delta))slide.start(delta,time,reduced);}
    void select(CalendarDate date,std::int64_t time){old_year=model.year();old_month=model.month();if(model.select(date)&&(old_year!=model.year()||old_month!=model.month()))slide.start(model.year()*12+static_cast<int>(model.month())>old_year*12+static_cast<int>(old_month)?1:-1,time,reduced);}
    void button(D2D1_RECT_F r,const wchar_t* label,int index){if(contains(r,pointer_x,pointer_y))rect(r,pal.hover,9);if(focus==index)outline(r,pal.accent,9);text(label,r,2,pal.ink,true);}
    void draw_grid(int year,unsigned month,float offset,float opacity,bool hit) {
        const auto days=calendar_grid(year,month);
        for(size_t i=0;i<days.size();++i) {
            const auto d=days[i];const float x=grid_left+(static_cast<float>(i%7)+.5f)*column_width,y=grid_top+(static_cast<float>(i/7)+.5f)*row_height+offset;
            const auto cell=D2D1::RectF(x-20,y-20,x+20,y+20);
            const bool selected=d==model.selected(),today=d==model.current_date(),valid=valid_calendar_date(d);
            if(selected){color(fade(pal.accent,opacity));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),19,19),brush.get());}
            else if(hit&&valid&&contains(cell,pointer_x,pointer_y))rect(cell,fade(pal.hover,opacity),20);
            if(today&&!selected){color(fade(pal.accent,opacity));target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),19,19),brush.get(),1.25f);}
            if(selected&&focus==4)outline(D2D1::RectF(x-21,y-21,x+21,y+21),fade(pal.ink,.7f*opacity),21);
            text(std::to_wstring(d.day),cell,0,selected?fade(pal.on_accent,opacity):fade(d.month==month&&valid?pal.ink:pal.ink_muted,opacity),true);
        }
    }
    void draw(float scale=1) {
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,D2D1::Point2F(180,height()-10))*(*D2D1::Matrix3x2F::ReinterpretBaseType(&origin)));
        if(!external)target->BeginDraw();
        target->Clear(D2D1::ColorF(0,0,0,0));
        glass.palette=pal;glass.live=live;glass.high_contrast=high_contrast;glass.origin=origin;glass.begin_frame();
        glass.paint(D2D1::RectF(10,10,350,height()-12),22,GlassKind::Panel);
        rect(D2D1::RectF(26,59,334,60),pal.separator);
        text(date_label(model.selected(),L"dddd, MMMM d"),D2D1::RectF(26,20,303,52),0,pal.ink);
        button(close_button(),L"×",3);
        text(date_label({model.year(),model.month(),1},L"MMMM yyyy"),D2D1::RectF(26,72,260,104),1,pal.ink);
        button(previous_button(),L"↑",0);button(next_button(),L"↓",1);
        for(unsigned i=0;i<7;++i){wchar_t name[32]{};GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_SABBREVDAYNAME1+(i?i-1:6),name,32);text(name,D2D1::RectF(grid_left+i*column_width,112,grid_left+(i+1)*column_width,138),0,pal.ink_secondary,true);}
        target->PushAxisAlignedClip(D2D1::RectF(20,140,340,398),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if(slide.active())draw_grid(old_year,old_month,slide.old_offset(),1,false);
        draw_grid(model.year(),model.month(),slide.new_offset(),1,true);
        target->PopAxisAlignedClip();
        rect(D2D1::RectF(26,397,334,398),pal.separator);
        button(today_button(),L"Today",2);
        text(L"Scroll to change month",D2D1::RectF(126,402,335,430),0,pal.ink_secondary,true);
        if(prayer.visible)draw_prayers();
    }
    // Today's Adhan and Iqama times; the next prayer is highlighted.
    void draw_prayers() {
        rect(D2D1::RectF(26,440,334,441),pal.separator);
        text(L"Prayer times",D2D1::RectF(26,448,170,476),1,pal.ink);
        text(prayer.status,D2D1::RectF(150,448,294,476),0,pal.ink_secondary);
        if(!prayer.website.empty()){const auto b=website_button();if(contains(b,pointer_x,pointer_y))rect(b,pal.hover,9);if(focus==5)outline(b,pal.accent,9);text(L"\x2197",b,2,pal.ink,true);}
        text(L"Salah",D2D1::RectF(34,480,150,502),0,pal.ink_secondary);
        text(L"Adhan",D2D1::RectF(150,480,242,502),0,pal.ink_secondary,true);
        text(L"Iqama",D2D1::RectF(242,480,334,502),0,pal.ink_secondary,true);
        for(size_t i=0;i<prayer.rows.size();++i) {
            const auto& row=prayer.rows[i];const float y=504+static_cast<float>(i)*28;
            if(row.state==1)rect(D2D1::RectF(22,y+1,338,y+27),fade(pal.accent,.22f),8);
            const auto ink=row.state==0?pal.ink_muted:pal.ink;
            text(row.name,D2D1::RectF(34,y,150,y+28),1,ink);
            text(row.adhan,D2D1::RectF(150,y,242,y+28),1,ink,true);
            text(row.iqama,D2D1::RectF(242,y,334,y+28),1,ink,true);
        }
        if(prayer.rows.empty())text(L"Loading prayer times\x2026",D2D1::RectF(34,504,334,532),1,pal.ink_secondary);
    }
};
}
struct CalendarPopup::Impl {
    HWND hwnd=nullptr,owner=nullptr;
    CalendarPainter painter;
    PanelReveal reveal;
    HDC dc=nullptr;HBITMAP dib=nullptr;HGDIOBJ previous=nullptr;void* pixels=nullptr;
    GlassHost host;
    bool glass_mode=false,glass_bound=false;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> render;
    float dpi=96;
    POINT position{};
    UINT width=0,height=0,allocations=0;
    int wheel_delta=0;
    bool timer=false,closing=false,tracking=false,opening=false,return_focus=false;
    static std::int64_t now(){return static_cast<std::int64_t>(GetTickCount64());}
    void destroy(){return_focus=GetForegroundWindow()==hwnd;DestroyWindow(hwnd);}
    ~Impl(){close(false);release();}
    void release(){host.detach();glass_bound=false;painter.brush.reset();painter.material.reset();painter.sheen.reset();painter.target.reset();render.reset();bitmap.reset();if(dc&&previous)SelectObject(dc,previous);if(dib)DeleteObject(dib);if(dc)DeleteDC(dc);dc=nullptr;dib=nullptr;previous=nullptr;pixels=nullptr;}
    float alpha() const{return reveal.opacity();}
    float offset() const{return reveal.offset();}
    D2D1_POINT_2F content_point(LPARAM l) const {
        const auto scale=reveal.scale();const float pivot=painter.height()-10;return D2D1::Point2F(180+(GET_X_LPARAM(l)*96.0f/dpi-180)/scale,pivot+(GET_Y_LPARAM(l)*96.0f/dpi-pivot)/scale);
    }
    int hover_target(float x,float y) const {
        if(x<0||y<0)return -1;
        const D2D1_RECT_F buttons[]={previous_button(),next_button(),today_button(),close_button()};
        for(int i=0;i<4;++i)if(contains(buttons[i],x,y))return i;
        if(painter.prayer.visible&&!painter.prayer.website.empty()&&contains(website_button(),x,y))return 5;
        const auto local_x=x-grid_left,local_y=y-painter.slide.new_offset()-grid_top;
        if(local_x<0||local_x>=7*column_width||local_y<0||local_y>=6*row_height)return -1;
        // Cell hover is drawn within the 40 DIP circle around each day's centre.
        const auto column=static_cast<int>(local_x/column_width),row=static_cast<int>(local_y/row_height);
        const auto cx=(static_cast<float>(column)+.5f)*column_width,cy=(static_cast<float>(row)+.5f)*row_height;
        if(std::abs(local_x-cx)>20||std::abs(local_y-cy)>20)return -2;
        return 10+row*7+column;
    }
    void schedule(){const bool moving=closing||reveal.active()||painter.slide.active();if(moving&&!timer)timer=SetTimer(hwnd,motion_timer,10,nullptr)!=0;else if(!moving&&timer){KillTimer(hwnd,motion_timer);timer=false;}if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);}
    // Composition path: blurred backdrop masked to the card, content above it,
    // fade and lift applied by the compositor.
    void present_glass() {
        width=static_cast<UINT>(std::ceil(canvas_width*dpi/96));height=static_cast<UINT>(std::ceil(painter.height()*dpi/96));
        RECT current{};GetWindowRect(hwnd,&current);
        if(current.left!=position.x||current.top!=position.y||current.right-current.left!=static_cast<LONG>(width)||current.bottom-current.top!=static_cast<LONG>(height))
            SetWindowPos(hwnd,nullptr,position.x,position.y,static_cast<int>(width),static_cast<int>(height),SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER);
        if(!host.attached()) {
            host.attach(hwnd,painter.factory.get());
            // Clicks outside the card and its shadow reach the windows below.
            const float scale=dpi/96;
            if(auto region=CreateRoundRectRgn(0,0,static_cast<int>(width)+1,static_cast<int>(height)+1,static_cast<int>(48*scale),static_cast<int>(48*scale)))
                if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
        }
        D2D1_MATRIX_3X2_F origin{};auto context=host.begin(width,height,dpi,origin);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        painter.live=painter.transparency&&!painter.high_contrast;
        if(!glass_bound){painter.bind(context);glass_bound=true;++allocations;}else painter.retarget(context);
        painter.external=true;painter.origin=origin;painter.draw(reveal.scale());painter.external=false;
        host.end();host.backdrops(painter.glass.shapes,dpi);
        host.present(alpha(),offset()*dpi/96,painter.live);
    }
    void present() {
        if(!hwnd)return;
        if(glass_mode){present_glass();return;}
        if(!render){
            width=static_cast<UINT>(std::ceil(canvas_width*dpi/96));height=static_cast<UINT>(std::ceil(painter.height()*dpi/96));
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=static_cast<LONG>(width);info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            dc=CreateCompatibleDC(nullptr);dib=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);if(!dc||!dib||!pixels)throw E_OUTOFMEMORY;previous=SelectObject(dc,dib);
            require(painter.wic->CreateBitmap(width,height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));require(painter.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),render.put()));render->SetDpi(dpi,dpi);painter.bind(render.get());++allocations;
        }
        painter.draw(reveal.scale());require(render->EndDraw());require(bitmap->CopyPixels(nullptr,width*4,width*height*4,static_cast<BYTE*>(pixels)));
        POINT destination{position.x,position.y+static_cast<LONG>(std::lround(offset()*dpi/96))},source{};SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};BLENDFUNCTION blend{AC_SRC_OVER,0,static_cast<BYTE>(std::lround(alpha()*255)),AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(hwnd,nullptr,&destination,&size,dc,&source,0,&blend,ULW_ALPHA))throw E_FAIL;
    }
    void clamp_position(){MONITORINFO info{sizeof(info),{},{},0};if(GetMonitorInfoW(MonitorFromPoint(position,MONITOR_DEFAULTTONEAREST),&info)){position.x=std::clamp(position.x,info.rcWork.left,std::max(info.rcWork.left,info.rcWork.right-static_cast<LONG>(width)));position.y=std::clamp(position.y,info.rcWork.top,std::max(info.rcWork.top,info.rcWork.bottom-static_cast<LONG>(height)));}}
    void open(HWND parent,RECT anchor,bool reduced) {
        if(hwnd&&closing){owner=parent;closing=false;painter.reduced=reduced;reveal.open(now(),reduced);SetForegroundWindow(hwnd);SetFocus(hwnd);schedule();return;}
        close(false);owner=parent;opening=true;closing=false;wheel_delta=0;
        if(!painter.factory)painter.initialize();painter.appearance();painter.model.reset(local_date());painter.slide.settle();painter.pointer_x=painter.pointer_y=-1;painter.focus=-1;painter.reduced=reduced;
        WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.Calendar";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw E_FAIL;
        dpi=static_cast<float>(GetDpiForWindow(parent));width=static_cast<UINT>(std::ceil(canvas_width*dpi/96));height=static_cast<UINT>(std::ceil(painter.height()*dpi/96));
        // Center the visible card over its clock button, with the same 12 DIP
        // gap above the pill as the other flyouts. Clamp only at screen edges.
        position={(anchor.left+anchor.right-static_cast<LONG>(width))/2,anchor.top-static_cast<LONG>(std::lround((painter.height()-10+15)*dpi/96))};clamp_position();
        glass_mode=GlassHost::available();
        hwnd=CreateWindowExW((glass_mode?WS_EX_NOREDIRECTIONBITMAP:WS_EX_LAYERED)|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,klass.lpszClassName,L"MinimaDock Calendar",WS_POPUP,position.x,position.y,width,height,parent,nullptr,klass.hInstance,this);if(!hwnd)throw E_FAIL;
        reveal.start(now(),reduced);present();ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);SetFocus(hwnd);opening=false;arm_day_timer();schedule();
    }
    // Re-arm just after the next minute boundary so midnight is observed promptly.
    void arm_day_timer(){SYSTEMTIME time{};GetLocalTime(&time);SetTimer(hwnd,day_timer,std::clamp((60-time.wSecond)*1000u-time.wMilliseconds+50u,50u,60050u),nullptr);}
    void close(bool animated) {
        if(!hwnd)return;
        if(closing&&animated)return;
        if(animated&&!painter.reduced){reveal.close(now());closing=true;schedule();return;}
        destroy();
    }
    void update_preferences(bool reduced){painter.reduced=reduced;painter.appearance();if(reduced){reveal.settle();painter.slide.settle();if(closing){destroy();return;}}if(hwnd)schedule();}
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l){auto self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(message==WM_NCDESTROY){SetWindowLongPtrW(window,GWLP_USERDATA,0);return DefWindowProcW(window,message,w,l);}if(!self)return DefWindowProcW(window,message,w,l);try{return self->message(message,w,l);}catch(...){PostMessageW(window,WM_CLOSE,0,0);return 0;}}
    LRESULT message(UINT msg,WPARAM w,LPARAM l) {
        switch(msg){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);try{present();}catch(...){EndPaint(hwnd,&ps);throw;}EndPaint(hwnd,&ps);return 0;}
        case WM_TIMER:if(w==motion_timer){reveal.tick(now());if(closing&&reveal.hidden()){destroy();return 0;}painter.slide.tick(now());schedule();}else if(w==day_timer){painter.model.update_today(local_date());arm_day_timer();schedule();}return 0;
        case WM_CLOSE:close(true);return 0;
        case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE&&!opening&&reinterpret_cast<HWND>(l)!=owner)close(true);return 0;
        case WM_ACTIVATEAPP:if(!w&&!opening)close(true);return 0;
        case WM_MOUSEMOVE:{
            const auto point=content_point(l);const auto before=hover_target(painter.pointer_x,painter.pointer_y);
            painter.pointer_x=point.x;painter.pointer_y=point.y;
            const bool fresh=!tracking;
            if(!tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0};TrackMouseEvent(&t);tracking=true;}
            // Redraw only when the hovered button or day changes.
            if(fresh||before!=hover_target(point.x,point.y))schedule();
            return 0;}
        case WM_MOUSELEAVE:tracking=false;painter.pointer_x=painter.pointer_y=-1;schedule();return 0;
        case WM_LBUTTONDOWN:return 0;
        case WM_LBUTTONUP:{
            if(closing)return 0;
            const auto point=content_point(l);const float x=point.x,y=point.y;
            if(contains(close_button(),x,y)){close(true);return 0;}
            if(painter.prayer.visible&&!painter.prayer.website.empty()&&contains(website_button(),x,y)){ShellExecuteW(hwnd,L"open",painter.prayer.website.c_str(),nullptr,nullptr,SW_SHOWNORMAL);close(true);return 0;}
            if(contains(previous_button(),x,y))painter.navigate(-1,now());
            else if(contains(next_button(),x,y))painter.navigate(1,now());
            else if(contains(today_button(),x,y))painter.select(painter.model.current_date(),now());
            else {const auto local_x=x-grid_left,local_y=y-painter.slide.new_offset()-grid_top;if(local_x>=0&&local_x<7*column_width&&local_y>=0&&local_y<6*row_height){const auto index=static_cast<size_t>(local_x/column_width)+7*static_cast<size_t>(local_y/row_height);painter.select(painter.model.grid()[index],now());}}
            painter.focus=-1;schedule();return 0;}
        case WM_MOUSEWHEEL:if(closing)return 0;wheel_delta+=GET_WHEEL_DELTA_WPARAM(w);while(std::abs(wheel_delta)>=WHEEL_DELTA){const int direction=wheel_delta>0?1:-1;painter.navigate(-direction,now());wheel_delta-=direction*WHEEL_DELTA;}schedule();return 0;
        case WM_KEYDOWN:{
            if(w==VK_ESCAPE){close(true);return 0;}
            if(closing)return 0;
            if(w==VK_TAB)painter.focus=(painter.focus+((GetKeyState(VK_SHIFT)&0x8000)?4:1)+5)%5;
            else if(w==VK_PRIOR||w==VK_NEXT)painter.navigate(w==VK_PRIOR?-1:1,now());
            else if(w==VK_HOME)painter.select(painter.model.current_date(),now());
            else if(w==VK_LEFT||w==VK_RIGHT||w==VK_UP||w==VK_DOWN){const auto date=calendar_date(calendar_days(painter.model.selected())+std::chrono::days(w==VK_LEFT?-1:w==VK_RIGHT?1:w==VK_UP?-7:7));painter.select(date,now());painter.focus=4;}
            else if(w==VK_RETURN||w==VK_SPACE){if(painter.focus==0||painter.focus==1)painter.navigate(painter.focus==0?-1:1,now());else if(painter.focus==2)painter.select(painter.model.current_date(),now());else if(painter.focus==3)close(true);}
            schedule();return 0;}
        case WM_DPICHANGED:{dpi=static_cast<float>(HIWORD(w));const auto r=reinterpret_cast<RECT*>(l);position={r->left,r->top};release();width=static_cast<UINT>(std::ceil(canvas_width*dpi/96));height=static_cast<UINT>(std::ceil(painter.height()*dpi/96));clamp_position();schedule();return 0;}
        case WM_DISPLAYCHANGE:clamp_position();schedule();return 0;
        case WM_APP+40:return render?1:0;
        case WM_APP+41:return painter.model.year();
        case WM_APP+42:return painter.model.month();
        case WM_APP+43:return painter.model.selected().day;
        case WM_APP+44:return timer?1:0;
        case WM_APP+45:return static_cast<LRESULT>(alpha()*1000);
        case WM_APP+46:return allocations;
        case WM_APP+48:return static_cast<LRESULT>(painter.slide.new_offset()*1000);
        case WM_APP+49:return painter.model.selected().year;
        case WM_APP+50:return painter.model.selected().month;
        case WM_APP+51:return closing?1:0;
        case WM_APP+52:return GetForegroundWindow()==hwnd?1:0;
        case WM_APP+53:return static_cast<LRESULT>(reveal.scale()*1000);
        case WM_APP+54:return static_cast<LRESULT>(offset()*1000);
        case WM_APP+55:return 2; // Calendar months move on the vertical axis.
        case WM_DESTROY:{const auto window=hwnd;KillTimer(hwnd,motion_timer);KillTimer(hwnd,day_timer);timer=closing=false;hwnd=nullptr;release();if(IsWindow(owner))PostMessageW(owner,calendar_closed_message,reinterpret_cast<WPARAM>(window),return_focus?1:0);return_focus=false;return 0;}
        default:return DefWindowProcW(hwnd,msg,w,l);
        }
    }
};
CalendarPopup::CalendarPopup():impl_(std::make_unique<Impl>()){}
CalendarPopup::~CalendarPopup()=default;
void CalendarPopup::open(HWND owner,RECT anchor,bool reduced){impl_->open(owner,anchor,reduced);}
void CalendarPopup::close(bool animated){impl_->close(animated);}
void CalendarPopup::preferences(bool reduced){impl_->update_preferences(reduced);}
void CalendarPopup::prayers(const PrayerView& view) {
    auto& i=*impl_;
    if(i.hwnd&&view.visible!=i.painter.prayer.visible)return; // Height changes wait for the next open.
    i.painter.prayer=view;if(i.hwnd)i.schedule();
}
HWND CalendarPopup::window() const{return impl_->hwnd;}
int save_calendar_snapshot(const std::wstring& path,bool moving,const PrayerView* prayers){
    try{CalendarPainter painter;painter.initialize();if(prayers)painter.prayer=*prayers;const auto tall=static_cast<UINT>(painter.height());ComPtr<IWICBitmap> bitmap;require(painter.wic->CreateBitmap(360,tall,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));ComPtr<ID2D1RenderTarget> rt;require(painter.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),rt.put()));painter.bind(rt.get());if(moving){painter.navigate(1,0);painter.slide.tick(60);}painter.draw();require(rt->EndDraw());ComPtr<IWICStream> stream;require(painter.wic->CreateStream(stream.put()));require(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));ComPtr<IWICBitmapEncoder> encoder;require(painter.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));require(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));ComPtr<IWICBitmapFrameEncode> frame;require(encoder->CreateNewFrame(frame.put(),nullptr));require(frame->Initialize(nullptr));require(frame->SetSize(360,tall));auto format=GUID_WICPixelFormat32bppBGRA;require(frame->SetPixelFormat(&format));require(frame->WriteSource(bitmap.get(),nullptr));require(frame->Commit());require(encoder->Commit());return 0;}catch(...){return 1;}
}
}
