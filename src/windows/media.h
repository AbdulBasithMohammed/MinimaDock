#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <cstdint>
#include <string_view>

namespace minima {
inline constexpr UINT media_changed_message=WM_APP+14;
enum class MediaCommand { Previous, Toggle, Next, Play, Pause };
inline std::uint32_t media_label_hash(std::wstring_view text){std::uint32_t hash=2166136261u;for(auto character:text){hash^=static_cast<std::uint32_t>(character);hash*=16777619u;}return hash;}
struct MediaSnapshot {
    bool available=false,playing=false,previous=false,toggle=false,next=false,unavailable=false;
    std::wstring title,artist,source;
    std::uint64_t session=0;
    bool operator==(const MediaSnapshot&) const=default;
};
class MediaController {
public:
    MediaController();
    ~MediaController();
    MediaController(const MediaController&)=delete;
    MediaController& operator=(const MediaController&)=delete;
    void start(HWND notify);
    void stop();
    MediaSnapshot snapshot() const;
    bool command(MediaCommand command,const MediaSnapshot& expected);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
