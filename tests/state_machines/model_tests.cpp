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
            radius=pulse.radius();alpha=pulse.opacity();
        }
        expect(!pulse.active()&&pulse.opacity()==0,"click effect ends at 240 ms without idle frames");
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
        DockLayout layout{580,700,1120,7};
        expect(DockLayout::offset(0)==-DockLayout::offset(1),"first apps balance across center");
        expect(DockLayout::offset(2)==-DockLayout::offset(3),"outer apps balance across center");
        expect(layout.width(0)<layout.width(1),"only utility wings expand outward");
        expect(layout.app_x({0,2})==580+DockLayout::offset(4),"all apps use their fixed balanced rank");
        for(float p:{0.0f,.25f,.5f,.75f,1.0f}) {
            expect(std::abs(((layout.center-layout.width(p)/2)+(layout.center+layout.width(p)/2))/2-580)<.001f,"launcher remains geometrically centered throughout expansion");
        }
        for(size_t index=0;index<8;++index)for(float p:{0.0f,.25f,.5f,.75f,1.0f}) {
            const auto offset=layout.app_x({index%2,index/2})-layout.center;
            expect(index%2==0?offset<0:offset>0,"every app preserves its side throughout expansion and collapse");
            expect(std::abs(offset)+18<=layout.width(p)/2,"all pinned and running app targets fit throughout hover motion");
        }
        expect(layout.collapsed_width()>=2*std::abs(layout.app_x({0,3})-580)+36,"all app slots fit inside collapsed pill");
        DockLayout empty{580,700,1120,0};
        expect(empty.width(0)==80,"no apps leaves room for centered button and separate desktop sliver");
        expect(DockLayout::expanded_height==44,"expanded pill keeps the accepted collapsed height");
        for(float progress:{0.0f,.1f,.25f,.5f,.75f,.9f,1.0f}) {
            expect(layout.height(progress)==44,"pill height stays fixed through the entire animation");
            expect(layout.center_y-layout.height(progress)/2==678&&layout.center_y+layout.height(progress)/2==722,"top and bottom stay anchored during horizontal expansion");
        }
        float previous_width=0;
        for(size_t count=0;count<=40;++count) {
            DockLayout growing{580,700,2400,count};
            expect(growing.expanded_width()==DockLayout::natural_expanded_width(count)&&growing.expanded_width()>=previous_width,"expanded width follows contents rather than fixed tile limit");
            previous_width=growing.expanded_width();
            expect(DockLayout::capacity(growing.expanded_width())>=count,"all app tiles fit at the natural content width");
            if(count>0) {
                const auto right=growing.collapsed_width()/2;
                expect(right-4-DockLayout::desktop_width>=DockLayout::offset((count/2?count/2-1:0)*2+1)+18,"desktop sliver stays outside app hit targets");
            }
        }
        expect(empty.collapsed_width()/2-4-DockLayout::desktop_width>=DockLayout::button/2,"desktop sliver does not overlap launcher when no apps are open");
        DockLayout monitor_limited{580,700,1920,80};
        expect(monitor_limited.expanded_width()==1920&&DockLayout::capacity(1920)>DockLayout::capacity(960),"only physical monitor width imposes overflow; larger screens fit more apps");
        DockMachine fast;fast.hold(Hold::Pointer,true,0);fast.tick(90);
        expect(fast.progress()==1&&!fast.needs_tick(),"expansion finishes in 90 ms");
        fast.hold(Hold::Pointer,false,91);fast.tick(181);
        expect(fast.progress()==0&&!fast.needs_tick(),"immediate collapse finishes in 90 ms");
        const auto balanced=[](const auto& counts){return counts[0]>counts[1]?counts[0]-counts[1]<=1:counts[1]-counts[0]<=1;};
        // Retain every established slot when updated running counts are balanced;
        // otherwise make the minimum necessary side changes.
        for(size_t pins=1;pins<=8;++pins)for(size_t extra=0;extra<=4;++extra) {
            std::vector<DockApp> idle;
            for(size_t i=0;i<pins;++i)idle.push_back({true,false});
            for(size_t i=0;i<extra;++i)idle.push_back({false,true});
            const auto baseline=balance_apps(idle);
            std::vector<size_t> previous_sides;
            for(const auto& app:baseline.apps)previous_sides.push_back(app.side);
            for(size_t mask=0;mask<(size_t{1}<<pins);++mask) {
                auto opened=idle;
                for(size_t i=0;i<pins;++i)opened[i].running=(mask&(size_t{1}<<i))!=0;
                const auto updated=balance_apps(opened,previous_sides);
                std::array<size_t,2> old_running{};
                for(size_t i=0;i<opened.size();++i)if(opened[i].running)++old_running[previous_sides[i]];
                const bool keep=balanced(old_running);
                expect(balanced(updated.expanded)&&balanced(updated.pinned)&&balanced(updated.running),
                    "stateful assignments balance expanded, pinned and running subsets");
                for(size_t i=0;i<opened.size();++i) {
                    if(keep) {
                        expect(updated.apps[i].side==baseline.apps[i].side&&
                            updated.apps[i].rank==baseline.apps[i].rank,
                            "balanced pin updates preserve every expanded slot");
                        expect(layout.app_x(updated.apps[i])==
                            layout.app_x(baseline.apps[i]),
                            "balanced pin state updates cannot move an expanded icon");
                    }
                    if(opened[i].running)expect((layout.app_x(updated.apps[i])<layout.center)==(updated.apps[i].side==0),
                        "stateful placements keep the assigned app side");
                }
                if(opened.size()<=7) {
                    const auto cost=[&](const auto& sides) {
                        size_t moves=0,live=0;
                        for(size_t i=0;i<opened.size();++i)if(sides[i]!=previous_sides[i]){++moves;if(opened[i].running)++live;}
                        return moves*(opened.size()+1)+live;
                    };
                    size_t optimum=static_cast<size_t>(-1);
                    for(size_t sides_mask=0;sides_mask<(size_t{1}<<opened.size());++sides_mask) {
                        std::vector<size_t> sides;std::array<size_t,2> pins_count{},running_count{},all_count{};
                        for(size_t i=0;i<opened.size();++i) {
                            const auto side=(sides_mask>>i)&1;sides.push_back(side);++all_count[side];
                            if(opened[i].pinned)++pins_count[side];if(opened[i].running)++running_count[side];
                        }
                        if(balanced(pins_count)&&balanced(running_count)&&balanced(all_count))optimum=std::min(optimum,cost(sides));
                    }
                    std::vector<size_t> actual;for(const auto& app:updated.apps)actual.push_back(app.side);
                    expect(cost(actual)==optimum,"required rebalancing moves the fewest icons, preferring idle pins");
                }
            }
        }
        for(size_t pins=0;pins<=8;++pins)for(size_t mask=0;mask<(size_t{1}<<pins);++mask)for(size_t extra=0;extra<=8;++extra) {
            std::vector<DockApp> apps;
            for(size_t i=0;i<pins;++i)apps.push_back({true,(mask&(size_t{1}<<i))!=0});
            for(size_t i=0;i<extra;++i)apps.push_back({false,true});
            const auto placed=balance_apps(apps);
            expect(balanced(placed.pinned)&&balanced(placed.expanded)&&balanced(placed.running),"all pin/running combinations balance both views and pinned subset");
            std::array<size_t,2> ranks{};
            for(size_t i=0;i<apps.size();++i) {
                const auto& slot=placed.apps[i];
                expect(slot.rank==ranks[slot.side]++,"fixed ranks stay contiguous with pins inside unpinned running apps");
                if(apps[i].running) {
                    expect((layout.app_x(slot)<layout.center)==(slot.side==0),"balanced layouts preserve assigned running app sides");
                }
            }
            expect(placed.expanded[0]+placed.expanded[1]==apps.size(),"running pins occupy only one expanded slot");
        }
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
