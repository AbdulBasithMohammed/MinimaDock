#include "windows/media_intent.h"
#include <iostream>
#include <stdexcept>

using namespace minima;
using namespace std::chrono_literals;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){
    try {
        MediaIntent intent;const auto now=MediaIntent::Clock::time_point{};
        auto sequence=intent.begin(L"player",1,false,now);
        check(!intent.reconcile(L"player",1,true,true,now),"Pause feedback was not immediate.");
        check(!intent.reconcile(L"player",1,true,true,now+250ms),"Stale provider state reversed feedback.");
        check(!intent.reconcile(L"player",1,true,false,now+300ms)&&intent.pending(),"Unprocessed intent settled prematurely.");
        intent.complete(sequence,true);
        check(!intent.reconcile(L"player",1,true,false,now+350ms)&&!intent.pending(),"Confirmed pause did not settle.");
        const auto older=intent.begin(L"player",1,true,now+400ms);
        const auto newer=intent.begin(L"player",1,false,now+410ms);
        intent.complete(older,false);
        check(!intent.reconcile(L"player",1,true,true,now+450ms)&&intent.pending(),"Older rejection erased the latest click.");
        intent.complete(newer,true);
        check(!intent.reconcile(L"player",1,true,false,now+500ms)&&!intent.pending(),"Latest rapid-click state did not settle.");
        sequence=intent.begin(L"player",1,true,now+600ms);intent.complete(sequence,false);
        check(!intent.reconcile(L"player",1,true,false,now+610ms)&&!intent.pending(),"Rejected play did not roll back.");
        intent.begin(L"player",1,true,now+700ms);
        check(!intent.reconcile(L"player",1,true,false,now+2200ms)&&!intent.pending(),"Unconfirmed intent never expired.");
        intent.begin(L"player",1,false,now+2300ms);
        check(intent.reconcile(L"other",1,true,true,now+2310ms)&&!intent.pending(),"Player handoff retained old intent.");
        intent.begin(L"player",1,false,now+2400ms);
        check(intent.reconcile(L"player",2,true,true,now+2410ms)&&!intent.pending(),"New session inherited old intent.");
        intent.begin(L"player",2,true,now+2500ms);
        check(!intent.reconcile(L"player",2,false,false,now+2510ms)&&!intent.pending(),"Closed player retained intent.");
        intent.begin(L"player",2,true,now+2600ms);intent.clear();check(!intent.pending(),"Shutdown retained intent.");
        std::cout<<"Media intent passed: immediate feedback, stale notifications, confirmation, rapid clicks, rejection, timeout, player/session handoff and shutdown.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
