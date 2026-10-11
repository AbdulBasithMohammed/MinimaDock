#include "taskbar/media_marquee.h"
#include <iostream>
#include <stdexcept>
using namespace minima;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    MediaMarquee scroll;scroll.configure(96);scroll.visible(true,false,100);
    expect(scroll.active(),"Visible overflow must scroll");scroll.tick(1300);expect(scroll.offset()==0,"Title needs a reading pause");
    scroll.tick(3300);expect(std::abs(scroll.offset()-48)<.01f,"Forward scroll should reveal the middle");
    scroll.tick(5300);expect(std::abs(scroll.offset()-96)<.01f,"Scroll must reveal the final characters");
    scroll.tick(6300);expect(std::abs(scroll.offset()-96)<.01f,"Far end needs a reading pause");
    scroll.tick(8500);expect(std::abs(scroll.offset()-48)<.01f,"Return scroll should be smooth");
    scroll.tick(10500);expect(scroll.offset()==0,"Scroll must loop without jumping");
    for(int t=100;t<40000;t+=11){scroll.tick(t);expect(scroll.offset()>=0&&scroll.offset()<=96,"Title offset must stay inside measured overflow");}
    scroll.visible(false,false,40000);expect(!scroll.active()&&scroll.offset()==0,"Hiding must stop the timer and reset");
    scroll.visible(true,false,41000);scroll.tick(41200);expect(scroll.offset()==0,"Revealing must restore a reading pause");
    scroll.visible(true,true,42000);expect(!scroll.active()&&scroll.offset()==0,"Reduced motion must stay still");
    scroll.configure(0);scroll.visible(true,false,43000);expect(!scroll.active(),"Fitting titles must remain idle");
    scroll.configure(-40);scroll.visible(true,false,44000);expect(!scroll.active(),"Short titles must not scroll");
    scroll.configure(30);scroll.visible(true,false,45000);scroll.tick(47000);scroll.configure(60);expect(!scroll.active()&&scroll.offset()==0,"Track changes must reset the old scroll");
    std::cout<<"Media marquee passed: pauses, smooth forward/return motion, bounds, reset, hidden idle, short titles and reduced motion.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
