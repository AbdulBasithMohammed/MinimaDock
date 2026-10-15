#include "taskbar/state_machine.h"
#include "taskbar/layout.h"
#include "windows/pin_stream.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace minima;
static int checks = 0;
void expect(bool result, const char* message) { ++checks; if (!result) throw std::runtime_error(message); }
int main() {
    try {
        ClickPulse pulse;
        expect(!pulse.active()&&pulse.opacity()==0,"click feedback is idle initially");
        pulse.start(100);expect(pulse.active()&&pulse.radius()==10&&pulse.opacity()>0,"click bubble begins immediately");
        float radius=pulse.radius(),alpha=pulse.opacity();
        for(int time=101;time<=340;++time) {
            pulse.tick(time);
            expect(pulse.radius()>=radius&&pulse.radius()<=18&&pulse.opacity()<=alpha&&pulse.opacity()>=0,"bubble expands and fades within its icon slot");
            expect(pulse.scale()>=.94f&&pulse.scale()<=1.01f,"click press and rebound stay inside the icon slot");
            radius=pulse.radius();alpha=pulse.opacity();
        }
        expect(!pulse.active()&&pulse.opacity()==0&&pulse.scale()==1,"click effect ends at 240 ms without idle frames or residual scaling");
        pulse.start(400);pulse.tick(430);pulse.start(431);pulse.tick(671);
        expect(!pulse.active(),"rapid clicks restart a bounded effect");
        pulse.start(700,true);expect(!pulse.active(),"reduced motion skips the click animation");
        PanelReveal reveal;reveal.start(100);
        expect(reveal.active()&&reveal.opacity()==0&&reveal.offset()==8,"panel reveal starts faded and eight DIP below its final position");
        float opacity=reveal.opacity(),offset=reveal.offset();
        for(int time=101;time<=280;++time) {
            reveal.tick(time);
            expect(reveal.opacity()>=opacity&&reveal.opacity()<=1&&reveal.offset()<=offset&&reveal.offset()>=0,"panel opacity and position converge monotonically");
            opacity=reveal.opacity();offset=reveal.offset();
        }
        expect(!reveal.active()&&reveal.opacity()==1&&reveal.offset()==0,"panel settles at 180 ms");
        reveal.start(300);reveal.tick(340);reveal.settle();expect(!reveal.active()&&reveal.offset()==0,"closing cancels a reveal immediately");
        reveal.start(400,true);expect(!reveal.active()&&reveal.opacity()==1,"reduced motion shows the complete panel immediately");
        reveal.close(500);reveal.tick(550);const auto closing=reveal.opacity();
        expect(closing>0&&closing<1&&reveal.scale()>.98f&&reveal.scale()<1,"closing fades, lowers and contracts the flyout");
        reveal.open(550);expect(reveal.opacity()==closing,"reopening preserves the currently displayed frame");
        reveal.tick(730);expect(!reveal.active()&&reveal.opacity()==1&&reveal.scale()==1,"reopening returns to full size and stops");
        reveal.close(800);float last_alpha=1;
        for(int t=800;t<=950;++t){reveal.tick(t);expect(reveal.opacity()<=last_alpha&&reveal.opacity()>=0,"close converges monotonically");last_alpha=reveal.opacity();}
        expect(reveal.hidden()&&!reveal.active()&&reveal.offset()==8,"close reaches zero and releases finite animation");
        reveal.open(1000,true);reveal.close(1001,true);expect(reveal.hidden(),"reduced motion closes immediately");
        DockVisibility visibility;
        visibility.update(true,false,false,0);
        expect(!visibility.wanted()&&visibility.needs_tick(),"maximized apps immediately begin hiding");
        visibility.tick(30);const auto hiding=visibility.progress();
        expect(hiding>0&&hiding<1&&visibility.offset()>0&&visibility.opacity()<1,"hide slides and fades without changing scale");
        visibility.update(true,false,true,30);
        expect(visibility.progress()==hiding&&visibility.wanted(),"edge reveal reverses continuously with no dwell delay");
        visibility.tick(500);expect(visibility.progress()==1&&!visibility.needs_tick(),"reveal settles and stops scheduling");
        visibility.update(true,false,false,501);visibility.tick(1000);
        expect(visibility.progress()==0&&!visibility.needs_tick(),"hidden dock has no idle animation");
        visibility.update(false,false,false,1001);visibility.tick(1500);
        expect(visibility.progress()==1,"desktop restores dock visibility");
        visibility.update(true,false,true,1501);expect(visibility.wanted(),"interaction holds prevent auto-hide");
        visibility.update(true,true,true,1502);expect(visibility.progress()==0&&!visibility.needs_tick()&&!visibility.wanted(),"fullscreen overrides holds and never animates over games");
        visibility.update(false,false,false,1600);visibility.reduced_motion(true);
        expect(visibility.progress()==1&&!visibility.needs_tick(),"reduced motion finishes visibility transition");
        visibility.update(true,false,false,1700);expect(visibility.progress()==0&&!visibility.needs_tick(),"reduced motion hides without animation");
        DockVisibility reversing;
        for(int t=0;t<2000;t+=7){reversing.update(true,false,t%91<42,t);reversing.tick(t+7);expect(reversing.progress()>=0&&reversing.progress()<=1,"rapid reversals remain within opacity and position bounds");}
        DockMachine d;
        expect(!d.needs_tick(), "idle does not schedule frames");
        d.hold(Hold::Pointer, true, 0);
        expect(d.state() == DockState::Expanding, "hover starts expansion on input with no delay");
        d.tick(1); expect(d.progress()>0, "first tick immediately advances expansion");
        d.tick(315); expect(d.progress() == 1 && !d.needs_tick(), "terminal frame stops scheduler");
        d.hold(Hold::Flyout, true, 320); d.hold(Hold::Pointer, false, 330); d.tick(1000);
        expect(d.state() == DockState::Expanded && !d.needs_tick(), "flyout keeps dock open");
        d.hold(Hold::Flyout, false, 1000);
        expect(d.state() == DockState::Collapsing, "leaving starts collapse immediately");
        d.tick(1030); const auto middle = d.progress();
        expect(middle > 0 && middle < 1, "collapse animates");
        d.hold(Hold::Pointer, true, 1030);
        expect(std::abs(d.progress() - middle) < .0001f, "reversal preserves position");
        d.tick(1700); expect(d.progress() == 1, "reversal completes");
        d.suppress(true, 1701); d.hold(Hold::Pointer, true, 1702);
        expect(!d.visible() && !d.needs_tick() && d.holds() == 0, "fullscreen cancels all interaction and animation");
        d.suppress(false, 1800); expect(d.visible() && d.progress() == 0, "return collapsed after fullscreen");
        d.reduced_motion(true); d.hold(Hold::Keyboard, true, 1801);
        expect(d.progress() == 1 && !d.needs_tick(), "keyboard and reduced motion");
        d.recover(2000); d.suppress(false, 2100); d.hold(Hold::Pointer, true, 2200);
        expect(d.state() == DockState::Recovering && !d.visible(), "recovery is terminal");
        DockMachine brief; brief.hold(Hold::Pointer, true, 0); brief.hold(Hold::Pointer, false, 60); brief.tick(600);
        expect(brief.progress() == 0 && !brief.needs_tick(), "brief pointer crossing reverses and settles immediately");
        DockMachine preference; preference.hold(Hold::Pointer,true,0); preference.tick(20); preference.reduced_motion(true);
        expect(preference.progress()==1 && !preference.needs_tick(), "changing motion preference finishes active animation");
        DockLayout layout{580,700,2400,7};
        expect(layout.launcher_x()==layout.left()+DockLayout::edge+22,"launcher starts the centered app group");
        expect(layout.width(0)<layout.width(1),"hover reveals utility wings on both sides");
        for(size_t count=0;count<=40;++count) {
            DockLayout growing{580,700,2400,count};
            expect(growing.expanded_width()==DockLayout::natural_expanded_width(count),"width follows contents without an arbitrary tile limit");
            expect(DockLayout::capacity(growing.expanded_width())>=count,"natural width fits every app");
            for(float progress:{0.0f,.25f,.5f,.75f,1.0f}) {
                expect(growing.height(progress)==44&&growing.center_y-22==678,"horizontal reveal keeps accepted height and bottom anchor");
                for(size_t i=0;i<count;++i) {
                    expect(growing.app_x(i)-growing.launcher_x()==layout.app_x(i)-layout.launcher_x(),"adding apps retains every ordered slot relative to the launcher");
                    expect((growing.center-growing.width(progress)/2+growing.center+growing.width(progress)/2)/2==580,"hover keeps the pill centered");
                    expect(growing.app_x(i)-18>=growing.left()+DockLayout::edge+DockLayout::button,"apps follow the launcher");
                    expect(growing.app_x(i)+18<=growing.center+growing.width(progress)/2-4,"compact app targets fit without a desktop sliver");
                    expect(growing.app_x(i)+18<=growing.center+growing.expanded_width()/2-4-DockLayout::desktop_width,"expanded desktop sliver cannot overlap app targets");
                    if(i)expect(growing.app_x(i)-growing.app_x(i-1)==40,"ordered apps retain their spacing");
                }
            }
        }
        DockLayout empty{20,700,1120,0};
        expect(empty.collapsed_width()==DockLayout::button+2*DockLayout::edge&&empty.launcher_x()==empty.center,"empty compact pill centers its launcher without desktop space");
        for(size_t count=0;count<=40;++count){const DockLayout compact{580,700,2400,count};const auto right=compact.center+compact.collapsed_width()/2;const auto last=count?compact.app_x(count-1):compact.launcher_x();expect(compact.launcher_x()-compact.left()==right-last,"compact pill has identical end padding for first and last icon centers");}
        DockLayout monitor_limited{20,700,1920,80};
        expect(monitor_limited.expanded_width()==1920&&DockLayout::capacity(1920)>DockLayout::capacity(960),"only monitor width limits app capacity");
        DockMachine fast;fast.hold(Hold::Pointer,true,0);fast.tick(90);
        expect(fast.progress()==1&&!fast.needs_tick(),"expansion finishes in 90 ms");
        fast.hold(Hold::Pointer,false,91);fast.tick(181);
        expect(fast.progress()==0&&!fast.needs_tick(),"collapse finishes in 90 ms");
        const std::vector<std::uint8_t> pin_bytes{0,6,0,0,0,4,0,1,2,0,0,255};
        expect(parse_pin_stream(pin_bytes).size()==1,"bounded pin stream accepts a terminated PIDL");
        for(size_t length=0;length<pin_bytes.size();++length)
            expect(parse_pin_stream(std::span(pin_bytes.data(),length)).empty(),"truncated pin streams rejected before Shell access");
        auto corrupt=pin_bytes;corrupt[1]=255;
        expect(parse_pin_stream(corrupt).empty(),"oversized pin record rejected");
        corrupt=pin_bytes;corrupt[5]=255;
        expect(parse_pin_stream(corrupt).empty(),"out-of-bounds SHITEMID rejected");
        corrupt=pin_bytes;corrupt[5]=1;
        expect(parse_pin_stream(corrupt).empty(),"non-progressing SHITEMID rejected");
        corrupt=pin_bytes;corrupt[10]=1;
        expect(parse_pin_stream(corrupt).empty(),"unterminated PIDL rejected");
        corrupt=pin_bytes;corrupt.back()=0;
        expect(parse_pin_stream(corrupt).empty(),"unknown stream terminator rejected");
        const std::vector<std::uint8_t> empty_pidl{0,2,0,0,0,0,0,255};
        expect(parse_pin_stream(empty_pidl).empty(),"empty desktop PIDL rejected as an app pin");
        std::cout << checks << " state machine checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
