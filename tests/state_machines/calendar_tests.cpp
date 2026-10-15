#include "taskbar/calendar_model.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
using namespace minima;
int checks=0;
void expect(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
int main(){try{
    const auto october=calendar_grid(2026,10);
    expect(october.front()==CalendarDate{2026,9,27},"October 2026 begins with Sunday September 27");
    expect(october[14]==CalendarDate{2026,10,11},"October 11 belongs in the third Sunday cell");
    expect(october.back()==CalendarDate{2026,11,7},"Six-week grid includes following-month dates");
    for(const int year:{1601,1700,1900,2000,2024,2026,2100,2400,9999})for(unsigned month=1;month<=12;++month){
        const auto grid=calendar_grid(year,month);unsigned in_month=0;
        for(size_t i=0;i<grid.size();++i){if(grid[i].year==year&&grid[i].month==month)++in_month;if(i)expect(calendar_days(grid[i])-calendar_days(grid[i-1])==std::chrono::days(1),"Every cell is the next civil day");expect(std::chrono::weekday(calendar_days(grid[i])).c_encoding()==i%7,"Column matches Sunday-first weekday");}
        if(month==2)expect(in_month==static_cast<unsigned>(year%4==0&&(year%100!=0||year%400==0)?29:28),"Gregorian leap-century rules");
        else expect(in_month==((month==4||month==6||month==9||month==11)?30u:31u),"Each month contains exactly its civil days");
    }
    CalendarModel model({2026,12,31});expect(model.navigate(1)&&model.year()==2027&&model.month()==1,"Next month crosses into January");expect(model.selected()==CalendarDate{2026,12,31},"Browsing months preserves selected date");expect(model.navigate(-1)&&model.year()==2026&&model.month()==12,"Previous month crosses year correctly");
    expect(model.select({2024,2,29}),"Leap day is selectable");model.move_selection(1);expect(model.selected()==CalendarDate{2024,3,1},"Day keyboard navigation crosses leap February");
    expect(!model.select({1900,2,29})&&model.selected()==CalendarDate{2024,3,1},"Invalid civil dates cannot alter selection");
    model.today();expect(model.selected()==CalendarDate{2026,12,31}&&model.year()==2026&&model.month()==12,"Today restores date and visible month");model.update_today({2027,1,1});model.today();expect(model.selected()==CalendarDate{2027,1,1},"Clock rollover updates Today without rebuilding calendar");
    model.select({1601,1,1});expect(!model.navigate(-1),"Lower supported month does not wrap");model.move_selection(-1);expect(model.selected()==CalendarDate{1601,1,1},"Lower selected-day bound does not wrap");model.select({9999,12,31});expect(!model.navigate(1),"Upper month bound does not wrap");model.move_selection(1);expect(model.selected()==CalendarDate{9999,12,31},"Upper day bound does not wrap");
    CalendarSlide slide;slide.start(1,100);expect(slide.active()&&slide.blend()==0&&slide.new_offset()==CalendarSlide::distance&&slide.old_offset()==0,"Next month starts below the fixed grid");float last=0;
    for(int t=100;t<=300;++t){slide.tick(t);expect(slide.blend()>=last&&slide.blend()<=1&&std::abs(slide.new_offset())<=CalendarSlide::distance&&std::abs(slide.old_offset())<=CalendarSlide::distance,"Slide stays bounded and monotonic");last=slide.blend();}
    expect(!slide.active()&&slide.new_offset()==0&&slide.blend()==1,"Month animation settles exactly and stops");slide.start(-1,400);expect(slide.new_offset()==-CalendarSlide::distance,"Previous month enters from above");slide.tick(440);slide.start(1,441);slide.tick(641);expect(!slide.active()&&slide.new_offset()==0,"Rapid reversal ends at the requested month");slide.start(-1,700,true);expect(!slide.active()&&slide.new_offset()==0,"Reduced motion changes month immediately");slide.start(1,800);slide.settle();expect(!slide.active(),"Cancelling animation leaves no idle motion");
    std::cout<<checks<<" calendar checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
