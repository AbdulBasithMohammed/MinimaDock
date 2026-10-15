#pragma once
#include <windows.h>
#include <memory>

namespace minima {
// Posted to the notify window when the default output's level/mute changes
// (from any app or the keyboard) or the default output device changes.
inline constexpr UINT volume_changed_message=WM_APP+16;
// Master volume of the default playback device and mute of the default
// microphone (Core Audio endpoint volume).
// GUI-thread object; change callbacks only post a message.
class VolumeControl {
public:
    VolumeControl();
    ~VolumeControl();
    VolumeControl(const VolumeControl&)=delete;
    VolumeControl& operator=(const VolumeControl&)=delete;
    void start(HWND notify);
    void stop();
    // Re-binds after a default-device change notification.
    void refresh();
    bool available() const;
    float level() const;
    bool muted() const;
    void set_level(float value);
    // For dragging: applies the latest value on a background thread so the
    // GUI never waits on the audio service; older pending values are dropped.
    void set_level_async(float value);
    void set_muted(bool value);
    // Default recording device (quick settings' mic mute).
    bool mic_available() const;
    bool mic_muted() const;
    void set_mic_muted(bool value); // Also the default communications mic.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
