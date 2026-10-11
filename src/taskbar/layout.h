#pragma once
#include <algorithm>
#include <cstddef>
#include <array>
#include <vector>
#include <cmath>

namespace minima {
struct DockApp { bool pinned, running; };
struct AppPlacement { std::size_t side=0, expanded_rank=0, collapsed_rank=0; };
struct BalancedApps {
    std::vector<AppPlacement> apps;
    std::array<std::size_t,2> expanded{}, running{}, pinned{};
};
inline BalancedApps balance_apps(const std::vector<DockApp>& apps,const std::vector<std::size_t>& previous_sides={}) {
    BalancedApps result;result.apps.resize(apps.size());
    const auto put=[&](std::size_t i,std::size_t side) {
        result.apps[i].side=side;++result.expanded[side];
        if(apps[i].pinned)++result.pinned[side];
        if(apps[i].running)++result.running[side];
    };
    // Assign one side per app for both views. Balance the overlapping running-pin
    // subset first, then idle pins, then the remaining running apps.
    for(std::size_t i=0;i<apps.size();++i)if(apps[i].pinned&&apps[i].running)
        put(i,result.running[0]<=result.running[1]?0:1);
    for(std::size_t i=0;i<apps.size();++i)if(apps[i].pinned&&!apps[i].running)
        put(i,result.pinned[0]<=result.pinned[1]?0:1);
    for(std::size_t i=0;i<apps.size();++i)if(!apps[i].pinned) {
        const auto left=result.expanded[0]+result.running[0];
        const auto right=result.expanded[1]+result.running[1];
        put(i,left<=right?0:1);
    }
    if(std::any_of(previous_sides.begin(),previous_sides.end(),[](auto side){return side<2;})) {
        // Find balanced category counts with the fewest existing icons moving.
        // Categories: idle unpinned, idle pinned, running unpinned, running pinned.
        // New icons have no established side and cost nothing to place.
        std::array<std::size_t,4> count{},old_left{},old_right{},best_left{};
        const auto category=[&](std::size_t i){return (apps[i].pinned?1u:0u)|(apps[i].running?2u:0u);};
        const auto previous=[&](std::size_t i){return i<previous_sides.size()?previous_sides[i]:2u;};
        for(std::size_t i=0;i<apps.size();++i) {
            const auto c=category(i);++count[c];
            if(previous(i)==0)++old_left[c];else if(previous(i)==1)++old_right[c];
        }
        const auto pins=count[1]+count[3],running=count[2]+count[3];
        std::size_t best_cost=static_cast<std::size_t>(-1);
        for(std::size_t pr=0;pr<=count[3];++pr)
        for(std::size_t pin_left=pins/2;pin_left<=(pins+1)/2;++pin_left)
        for(std::size_t run_left=running/2;run_left<=(running+1)/2;++run_left)
        for(std::size_t all_left=apps.size()/2;all_left<=(apps.size()+1)/2;++all_left) {
            if(pin_left<pr||run_left<pr||pin_left-pr>count[1]||run_left-pr>count[2])continue;
            const auto assigned=pin_left+run_left-pr;
            if(all_left<assigned||all_left-assigned>count[0])continue;
            const std::array<std::size_t,4> left{all_left-assigned,pin_left-pr,run_left-pr,pr};
            std::size_t moves=0,live_moves=0;
            for(std::size_t c=0;c<4;++c) {
                const auto excess_left=old_left[c]>left[c]?old_left[c]-left[c]:0;
                const auto right=count[c]-left[c];
                const auto excess_right=old_right[c]>right?old_right[c]-right:0;
                moves+=excess_left+excess_right;
                if(c&2u)live_moves+=excess_left+excess_right;
            }
            // First minimize all movement, then prefer moving idle pins.
            const auto cost=moves*(apps.size()+1)+live_moves;
            if(cost<best_cost){best_cost=cost;best_left=left;}
        }
        if(best_cost!=static_cast<std::size_t>(-1)) {
            result.expanded={};result.running={};result.pinned={};
            std::array<std::size_t,4> kept_left{},kept_right{};
            std::vector<std::size_t> sides(apps.size(),2);
            // Keep inner/earlier icons when a category must shed an outer icon.
            for(std::size_t i=0;i<apps.size();++i) {
                const auto c=category(i);const auto side=previous(i);
                if(side==0&&kept_left[c]<best_left[c]){sides[i]=0;++kept_left[c];}
                else if(side==1&&kept_right[c]<count[c]-best_left[c]){sides[i]=1;++kept_right[c];}
            }
            for(std::size_t i=0;i<apps.size();++i) {
                const auto c=category(i);
                if(sides[i]==2) {
                    sides[i]=kept_left[c]<best_left[c]?0:1;
                    if(sides[i]==0)++kept_left[c];else ++kept_right[c];
                }
                put(i,sides[i]);
            }
        }
    }
    std::array<std::size_t,2> expanded_rank{},running_rank{};
    for(std::size_t i=0;i<apps.size();++i) {
        auto& placement=result.apps[i];
        placement.expanded_rank=expanded_rank[placement.side]++;
        placement.collapsed_rank=running_rank[placement.side];
        if(apps[i].running)++running_rank[placement.side];
    }
    return result;
}
// Logical pixels (DIPs). The launcher is the invariant origin for both sides.
struct DockLayout {
    float center, center_y, available_width;
    std::size_t expanded_apps, running_left, running_right;
    static constexpr float button = 44, expanded_height = button, pitch = 40, utility_width = 156, desktop_width = 14;
    static float natural_expanded_width(std::size_t apps) { return button+2*(static_cast<float>((apps+1)/2)*pitch+utility_width+20+desktop_width); }
    static std::size_t capacity(float available) {
        const auto space=available-natural_expanded_width(0);
        return static_cast<std::size_t>(std::max(1.0f,std::floor(space/(2*pitch))))*2;
    }
    static float offset(std::size_t index) { return (index % 2 == 0 ? -1.0f : 1.0f) * (button/2 + 14 + static_cast<float>(index/2)*pitch); }
    float app_x(const AppPlacement& placement, bool running, float p) const {
        const auto expanded = offset(placement.expanded_rank*2+placement.side);
        const auto collapsed = running ? offset(placement.collapsed_rank*2+placement.side) : expanded;
        return center + collapsed + (expanded-collapsed)*p;
    }
    float collapsed_width() const { const auto wings=std::max(running_left,running_right);return wings?button+2*(static_cast<float>(wings)*pitch+desktop_width-2):button+2*(desktop_width+4); }
    float expanded_width() const { return std::min(available_width,natural_expanded_width(expanded_apps)); }
    float width(float p) const { return collapsed_width()+(expanded_width()-collapsed_width())*p; }
    float height(float) const { return button; }
};
}
