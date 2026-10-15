#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace minima {
// Protect immediate click feedback from stale provider notifications. Access is
// serialized by MediaController's shared mutex; every intent has a finite life.
class MediaIntent {
public:
    using Clock=std::chrono::steady_clock;
    std::uint64_t begin(std::wstring_view source,std::uint64_t session,bool playing,Clock::time_point now) {
        source_=source;session_=session;playing_=playing;active_=true;accepted_=false;
        deadline_=now+std::chrono::milliseconds(1500);return ++sequence_;
    }
    void complete(std::uint64_t sequence,bool accepted) {
        if(!active_||sequence!=sequence_)return;
        accepted_=accepted;if(!accepted)active_=false;
    }
    bool reconcile(std::wstring_view source,std::uint64_t session,bool available,bool playing,Clock::time_point now) {
        if(active_&&(!available||source!=source_||session!=session_||now>=deadline_||(accepted_&&playing==playing_)))active_=false;
        return active_?playing_:playing;
    }
    bool pending() const{return active_;}
    void clear(){active_=false;}
private:
    std::wstring source_;
    std::uint64_t session_=0,sequence_=0;
    bool playing_=false,active_=false,accepted_=false;
    Clock::time_point deadline_{};
};
}
