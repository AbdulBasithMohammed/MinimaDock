#pragma once
#include <inspectable.h>
#include <winstring.h>

// Minimal Windows.Media.Control ABI declarations for toolchains which lack
// windows.media.control.h. Method order and interface identifiers follow the
// public Windows SDK metadata: https://github.com/microsoft/windows-rs/blob/master/crates/libs/windows/src/Windows/Media/Control/mod.rs
// Only declared methods are used; Windows supplies every implementation.
namespace minima::media_abi {
template<class T> struct Async : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*)=0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T*)=0;
};
struct Event : IUnknown {virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*)=0;};
struct Properties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Title(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE Subtitle(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE AlbumArtist(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE Artist(HSTRING*)=0;
};
struct Controls : IInspectable {
#define MEDIA_FLAG(name) virtual HRESULT STDMETHODCALLTYPE name(boolean*)=0;
    MEDIA_FLAG(Play) MEDIA_FLAG(Pause) MEDIA_FLAG(Stop) MEDIA_FLAG(Record)
    MEDIA_FLAG(FastForward) MEDIA_FLAG(Rewind) MEDIA_FLAG(Next) MEDIA_FLAG(Previous)
    MEDIA_FLAG(ChannelUp) MEDIA_FLAG(ChannelDown) MEDIA_FLAG(Toggle)
#undef MEDIA_FLAG
};
struct Playback : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE ControlsInfo(Controls**)=0;
    virtual HRESULT STDMETHODCALLTYPE Status(INT32*)=0;
};
struct Session : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Source(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE MediaProperties(Async<Properties*>**)=0;
    virtual HRESULT STDMETHODCALLTYPE Timeline(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE PlaybackInfo(Playback**)=0;
#define MEDIA_OPERATION(name) virtual HRESULT STDMETHODCALLTYPE name(Async<boolean>**)=0;
    MEDIA_OPERATION(Play) MEDIA_OPERATION(Pause) MEDIA_OPERATION(Stop) MEDIA_OPERATION(Record)
    MEDIA_OPERATION(FastForward) MEDIA_OPERATION(Rewind) MEDIA_OPERATION(Next) MEDIA_OPERATION(Previous)
    MEDIA_OPERATION(ChannelUp) MEDIA_OPERATION(ChannelDown) MEDIA_OPERATION(Toggle)
#undef MEDIA_OPERATION
    virtual HRESULT STDMETHODCALLTYPE Repeat(INT32,Async<boolean>**)=0;
    virtual HRESULT STDMETHODCALLTYPE Rate(double,Async<boolean>**)=0;
    virtual HRESULT STDMETHODCALLTYPE Shuffle(boolean,Async<boolean>**)=0;
    virtual HRESULT STDMETHODCALLTYPE Position(INT64,Async<boolean>**)=0;
    virtual HRESULT STDMETHODCALLTYPE TimelineChanged(Event*,INT64*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveTimelineChanged(INT64)=0;
    virtual HRESULT STDMETHODCALLTYPE PlaybackChanged(Event*,INT64*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemovePlaybackChanged(INT64)=0;
    virtual HRESULT STDMETHODCALLTYPE PropertiesChanged(Event*,INT64*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemovePropertiesChanged(INT64)=0;
};
struct Manager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Current(Session**)=0;
    virtual HRESULT STDMETHODCALLTYPE Sessions(IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE CurrentChanged(Event*,INT64*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveCurrentChanged(INT64)=0;
    virtual HRESULT STDMETHODCALLTYPE SessionsChanged(Event*,INT64*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemoveSessionsChanged(INT64)=0;
};
struct Statics : IInspectable {virtual HRESULT STDMETHODCALLTYPE Request(Async<Manager*>**)=0;};
}
