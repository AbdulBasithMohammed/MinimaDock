// Exercise real DirectWrite measurement, retained text, clipping and hit geometry.
#include "../../src/rendering/preview.cpp"
#include <iostream>
#include <stdexcept>
using namespace minima;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(apartment))return 1;int result=0;
try {
    Scene scene;scene.initialize();scene.desktop=true;scene.width=1000;scene.height=88;
    scene.machine.reduced_motion(true);scene.machine.hold(Hold::Keyboard,true,0);
    ComPtr<IWICBitmap> bitmap;check(scene.wic->CreateBitmap(1000,88,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
    ComPtr<ID2D1RenderTarget> target;check(scene.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),target.put()));scene.bind(target.get());
    scene.media.available=scene.media.playing=scene.media.toggle=scene.media.next=scene.media.previous=true;
    scene.media.title=L"A very long track title with enough words to scroll smoothly and reveal its last characters";scene.media.artist=L"An artist";
    scene.measure_media_title();expect(scene.media_marquee.distance()>0,"Native title width did not detect overflow");
    const auto retained=scene.media_title.get();const auto mark=scene.launcher_mark.get();const auto pill=scene.dock();const auto area=scene.media_rect();
    scene.media_marquee.visible(true,false,0);
    for(int t=0;t<10000;t+=33){scene.media_marquee.tick(t);scene.draw();check(target->EndDraw());expect(scene.media_title.get()==retained,"Scroll recreated its text layout");expect(scene.launcher_mark.get()==mark,"Scroll recreated dock geometry");expect(scene.dock().top==pill.top&&scene.dock().bottom==pill.bottom,"Scroll changed pill height");}
    const auto pixels=[&](size_t index){const auto r=scene.media_button(index);WICRect rect{static_cast<INT>(r.left),static_cast<INT>(r.top),static_cast<INT>(r.right-r.left),static_cast<INT>(r.bottom-r.top)};ComPtr<IWICBitmapLock> lock;check(bitmap->Lock(&rect,WICBitmapLockRead,lock.put()));UINT bytes=0,stride=0;BYTE* data=nullptr;check(lock->GetDataPointer(&bytes,&data));check(lock->GetStride(&stride));std::vector<BYTE> copy;for(INT y=0;y<rect.Height;++y)copy.insert(copy.end(),data+y*stride,data+y*stride+rect.Width*4);return copy;};
    const auto pause=pixels(1),previous=pixels(0),next=pixels(2);
    scene.media.playing=false;scene.draw();check(target->EndDraw());
    expect(pixels(1)!=pause,"Paused media did not replace pause bars with the play triangle");
    expect(pixels(0)==previous&&pixels(2)==next,"Play/pause switch changed adjacent controls");
    scene.media.playing=true;scene.draw();check(target->EndDraw());expect(pixels(1)==pause,"Playing media did not restore the pause bars");
    for(size_t i=0;i<3;++i){const auto button=scene.media_button(i);expect(button.right-button.left==32,"Media button hit area is too narrow");expect(button.left>=area.left&&button.right<=area.right&&button.bottom<=pill.bottom,"Media button escapes its wing");if(i)expect(scene.media_button(i-1).right<button.left,"Media hit targets overlap");}
    scene.media.artist=L"Another artist";scene.measure_media_title();expect(scene.media_title.get()==retained,"Artist update discarded retained title");
    const auto short_tip=scene.tooltip_rect(L"JOHNPAL TV",area),long_tip=scene.tooltip_rect(std::wstring(200,L'W'),area);
    expect(short_tip.right-short_tip.left<130,"Short artist tooltip still reserves an oversized empty rectangle");
    expect(long_tip.right-long_tip.left<=300&&long_tip.left>=8&&long_tip.right<=scene.width-8,"Long tooltip escapes its bounded canvas");
    expect(short_tip.bottom==scene.dock().top-6&&short_tip.bottom-short_tip.top==26,"Tooltip overlaps its title or changes height with label length");
    scene.media.title=L"Short";scene.measure_media_title();expect(scene.media_marquee.distance()==0&&!scene.media_marquee.active(),"A new fitting title kept scrolling");
    scene.media.title=L"A long title which must stay still when reduced motion is enabled even though it does not fit";scene.measure_media_title();scene.reduced=true;scene.media_marquee.visible(true,true,0);expect(!scene.media_marquee.active(),"Reduced motion enabled scrolling");scene.draw();check(target->EndDraw());
    scene.media={};scene.measure_media_title();expect(scene.media_title_text==L"No media"&&scene.media_marquee.distance()==0,"Empty media kept the old title");
    scene.reduced=false;scene.media.available=scene.media.toggle=scene.media.previous=scene.media.next=true;
    const auto frame=[&](){scene.draw();check(target->EndDraw());std::vector<BYTE> data(1000*88*4);check(bitmap->CopyPixels(nullptr,4000,static_cast<UINT>(data.size()),data.data()));return data;};
    const auto baseline=frame();const auto count=static_cast<int>(scene.shown_groups());
    const D2D1_RECT_F controls[]={scene.tray_rect(),scene.quick_settings_rect(),scene.clock_rect(),scene.desktop_button_rect(),scene.media_button(0),scene.media_button(1),scene.media_button(2)};
    ClickPulse* pulses[]={&scene.tray_pulse,&scene.settings_pulse,&scene.clock_pulse,&scene.desktop_pulse,&scene.media_pulses[0],&scene.media_pulses[1],&scene.media_pulses[2]};
    for(size_t i=0;i<7;++i){
        const auto r=controls[i];scene.press_at((r.left+r.right)/2,(r.top+r.bottom)/2);
        expect(scene.control_pressed==count+1+static_cast<int>(i),"Control press hit geometry does not match its action");
        expect(frame()!=baseline,"Pressed control does not show the common press animation");
        scene.clear_press();pulses[i]->start(0);expect(frame()!=baseline,"Control does not render the common click bubble");
        scene.tick_feedback(240);expect(frame()==baseline,"Control click left residual pixels after settling");
        expect(scene.launcher_mark.get()==mark,"Control animation rebuilt retained geometry");
    }
    scene.machine.hold(Hold::Keyboard,false,300);const auto compact_frame=frame();
    scene.desktop_pulse.start(300);scene.focus=count+4;
    expect(frame()==compact_frame,"Compact pill still renders Show Desktop feedback or tooltip");
    scene.stop_feedback();scene.focus=-1;
    // Exercise the real layered flyout lifecycle while fullscreen suppression
    // remains covered by the separate live-dock checks.
    Preview dock;dock.scene.desktop=true;
    WNDCLASSW klass{};klass.lpfnWndProc=Preview::proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.FeedbackFixture";
    expect(RegisterClassW(&klass)!=0,"Could not register layered feedback fixture");
    const auto window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,klass.lpszClassName,L"MinimaDock feedback test",WS_POPUP,0,0,1000,440,nullptr,nullptr,klass.hInstance,&dock);
    expect(window!=nullptr,"Could not create layered feedback fixture");
    struct WindowGuard {HWND hwnd;~WindowGuard(){if(IsWindow(hwnd))DestroyWindow(hwnd);}} guard{window};
    const auto pump=[&](){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}};
    const auto await=[&](auto ready){for(int i=0;i<250;++i){pump();if(ready())return;Sleep(10);}throw std::runtime_error("Layered feedback fixture did not settle");};
    await([&](){return dock.inventory_loaded;});dock.scene.reduced=false;dock.scene.machine.reduced_motion(true);dock.scene.machine.hold(Hold::Keyboard,true,dock.now());dock.desktop_position();dock.paint_layer();
    const auto allocations=dock.layer_allocations;const auto center=dock.presented_center_y;
    for(const auto panel:{Panel::Settings,Panel::Launcher}){
        dock.set_panel(panel);expect(dock.scene.panel_reveal.active(),"Native flyout did not begin opening");
        await([&](){return !dock.scene.panel_reveal.active();});expect(dock.scene.panel_reveal.opacity()==1,"Native flyout did not reach full opacity");
        dock.set_panel(Panel::None);expect(dock.scene.panel==Panel::None&&dock.scene.drawn_panel()==panel,"Closing removed the pixels before exit or kept stale input");
        Sleep(40);pump();expect(dock.scene.panel_reveal.opacity()>0&&dock.scene.panel_reveal.opacity()<1,"Native close did not display an intermediate frame");
        const auto alpha=dock.scene.panel_reveal.opacity();dock.set_panel(panel);expect(dock.scene.panel_reveal.opacity()>=alpha&&dock.scene.panel_reveal.opacity()<1,"Reopening jumped instead of reversing its current frame");
        await([&](){return !dock.scene.panel_reveal.active();});dock.set_panel(Panel::None);
        await([&](){return dock.scene.exiting_panel==Panel::None&&!dock.animation_timer;});
        expect(dock.scene.height==88&&dock.layer_allocations==allocations,"Flyout exit did not restore compact canvas with retained resources");
        expect(dock.presented_center_y==center&&dock.scene.layout().height(1)==44,"Flyout motion changed pill height or anchor");
    }
    for(const auto panel:{Panel::Settings,Panel::Launcher,Panel::Calendar}) {
        dock.set_panel(panel);expect(dock.popup_watching,"Flyout did not arm outside-input dismissal");
        await([&](){return panel==Panel::Calendar?SendMessageW(dock.calendar.window(),WM_APP+44,0,0)==0:!dock.scene.panel_reveal.active();});
        POINT inside{};
        if(panel==Panel::Calendar){RECT r{};GetWindowRect(dock.calendar.window(),&r);inside={(r.left+r.right)/2,(r.top+r.bottom)/2};}
        else {const auto r=dock.scene.panel_rect();inside={static_cast<LONG>((r.left+r.right)*dock.dpi/192),static_cast<LONG>((r.top+r.bottom)*dock.dpi/192)};ClientToScreen(window,&inside);}
        dock.popup_keys=0;dock.poll_popup_input(1,inside);expect(dock.scene.panel==panel,"Click inside flyout incorrectly dismissed it");
        RECT bounds{};GetWindowRect(window,&bounds);POINT outside{bounds.left-50,bounds.top-50};
        dock.poll_popup_input(0,outside);dock.poll_popup_input(1,outside);
        expect(dock.scene.panel==Panel::None&&!dock.popup_watching,"Outside click did not dismiss flyout and disarm polling");
        expect(dock.calendar.window()!=nullptr||dock.scene.exiting_panel==panel,"Outside dismissal skipped its exit animation");
        await([&](){return !dock.calendar.window()&&dock.scene.exiting_panel==Panel::None;});
        dock.set_panel(panel);dock.popup_keys=0;dock.poll_popup_input(8,outside);
        expect(dock.scene.panel==Panel::None&&!dock.popup_watching,"Escape did not dismiss a nonactivating flyout");
        await([&](){return !dock.calendar.window()&&dock.scene.exiting_panel==Panel::None;});
    }
    dock.set_panel(Panel::Settings);dock.scene.machine.hold(Hold::ContextMenu,true,dock.now());dock.popup_keys=0;
    dock.poll_popup_input(2,POINT{-500,-500});expect(dock.scene.panel==Panel::Settings,"Native context menu interaction dismissed its parent flyout");
    dock.scene.machine.hold(Hold::ContextMenu,false,dock.now());
    SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);expect(dock.scene.panel==Panel::None,"Focused flyout did not dismiss on Escape");
    await([&](){return dock.scene.exiting_panel==Panel::None;});
    if(const auto shell=GetShellWindow()) {
        dock.set_panel(Panel::Settings);
        SendMessageW(window,foreground_changed,reinterpret_cast<WPARAM>(shell),dock.panel_opened-1);
        expect(dock.scene.panel==Panel::Settings,"Queued stale foreground event dismissed a newly opened flyout");
        SendMessageW(window,foreground_changed,reinterpret_cast<WPARAM>(shell),dock.panel_opened);
        expect(dock.scene.panel==Panel::None,"External foreground change did not dismiss quick settings");
        await([&](){return dock.scene.exiting_panel==Panel::None;});
    }
    dock.set_panel(Panel::Calendar);const auto calendar=dock.calendar.window();expect(calendar!=nullptr,"Native calendar flyout did not open");
    const auto read_calendar=[&](UINT offset){return SendMessageW(calendar,WM_APP+40+offset,0,0);};
    await([&](){return read_calendar(4)==0;});const auto calendar_allocations=read_calendar(6);
    const auto month=read_calendar(2);SendMessageW(calendar,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);
    expect(read_calendar(2)==(month==12?1:month+1)&&read_calendar(8)>0,"Calendar wheel did not immediately begin next month from below");
    await([&](){return read_calendar(4)==0;});dock.set_panel(Panel::None);Sleep(40);pump();
    expect(dock.calendar.window()==calendar&&read_calendar(11)==1&&read_calendar(5)>0&&read_calendar(5)<1000,"Calendar did not retain its window for the exit animation");
    dock.set_panel(Panel::Calendar);expect(dock.calendar.window()==calendar&&read_calendar(11)==0,"Calendar reopening recreated its window instead of reversing");
    await([&](){return read_calendar(4)==0;});expect(read_calendar(6)==calendar_allocations,"Calendar reversal rebuilt render resources");
    dock.set_panel(Panel::None);await([&](){return dock.calendar.window()==nullptr&&!dock.animation_timer;});
    DestroyWindow(window);
    std::cout<<"Media UI passed: retained marquee, pixel-verified controls, compact sliver exclusion, bounded measured tooltips, animated outside-click/Escape dismissal for launcher/settings/calendar, context-menu protection, stale foreground protection and stopped popup timers.\n";
}catch(HRESULT hr){std::cerr<<"Media UI HRESULT="<<std::hex<<static_cast<unsigned long>(hr)<<'\n';result=1;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;}
