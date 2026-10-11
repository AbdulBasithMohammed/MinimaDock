#include "windows/media.h"
#include "windows/media_abi.h"
#include "platform/com_ptr.h"
#include <roapi.h>
#include <asyncinfo.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace minima {
namespace {
using namespace media_abi;
GUID guid(const wchar_t* value){GUID result{};IIDFromString(value,&result);return result;}
struct String {
    HSTRING value=nullptr;
    ~String(){WindowsDeleteString(value);}
    std::wstring text() const {UINT32 length=0;const auto data=WindowsGetStringRawBuffer(value,&length);return data?std::wstring(data,length):std::wstring{};}
    std::wstring label(UINT32 limit) const {
        UINT32 length=0;const auto data=WindowsGetStringRawBuffer(value,&length);if(!data)return {};
        auto text=std::wstring(data,std::min(length,limit));
        if(!text.empty()&&text.back()>=0xd800&&text.back()<=0xdbff)text.pop_back();
        for(auto& character:text)if(character<32)character=L' ';
        return text;
    }
};
struct PendingCommand {MediaCommand command;std::wstring source;std::uint64_t session;};
struct Shared {
    std::mutex mutex;
    std::condition_variable_any wake;
    bool dirty=true,running=false;
    HWND notify=nullptr;
    MediaSnapshot snapshot;
    std::deque<PendingCommand> commands;
};
class Changed final : public Event {
    std::atomic<ULONG> references{1};
    std::shared_ptr<Shared> state;
    GUID iid;
public:
    Changed(std::shared_ptr<Shared> state,const wchar_t* id):state(std::move(state)),iid(guid(id)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID requested,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(requested!=IID_IUnknown&&requested!=IID_IAgileObject&&requested!=iid)return E_NOINTERFACE;
        *result=static_cast<Event*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) override {
        {std::lock_guard lock(state->mutex);state->dirty=true;}state->wake.notify_all();return S_OK;
    }
};
template<class T> HRESULT result(Async<T>* operation,T* value,std::stop_token stop,const std::shared_ptr<Shared>& state) {
    if(!operation)return E_POINTER;
    ComPtr<IAsyncInfo> info;auto hr=operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(info.put()));if(FAILED(hr))return hr;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1500);
    AsyncStatus status=Started;
    while(SUCCEEDED(hr=info->get_Status(&status))&&status==Started) {
        if(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline){info->Cancel();return stop.stop_requested()?E_ABORT:HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        std::unique_lock lock(state->mutex);state->wake.wait_for(lock,stop,std::chrono::milliseconds(10),[]{return false;});
    }
    if(FAILED(hr))return hr;
    if(status==Completed)return operation->GetResults(value);
    if(status==Error){info->get_ErrorCode(&hr);return FAILED(hr)?hr:E_FAIL;}
    return E_ABORT;
}
struct Subscriptions {
    ComPtr<Manager> manager;
    ComPtr<Session> session;
    INT64 current_token=0,sessions_token=0,playback_token=0,properties_token=0;
    bool current=false,sessions=false,playback=false,properties=false;
    std::uint64_t generation=0;
    ~Subscriptions(){clear_manager();}
    void clear_manager(){clear_session();if(manager){if(current)manager->RemoveCurrentChanged(current_token);if(sessions)manager->RemoveSessionsChanged(sessions_token);}manager.reset();current=sessions=false;}
    void clear_session(){if(session){if(playback)session->RemovePlaybackChanged(playback_token);if(properties)session->RemovePropertiesChanged(properties_token);}session.reset();playback=properties=false;}
    void bind_manager(const std::shared_ptr<Shared>& state) {
        ComPtr<Event> changed;*changed.put()=new Changed(state,L"{228bd0ed-1fa2-5e9b-a6ec-42566173103b}");current=SUCCEEDED(manager->CurrentChanged(changed.get(),&current_token));
        *changed.put()=new Changed(state,L"{2e2a8630-dc8c-530a-9746-bc984d4b029e}");sessions=SUCCEEDED(manager->SessionsChanged(changed.get(),&sessions_token));
    }
    void bind_session(Session* value,const std::shared_ptr<Shared>& state) {
        if(session.get()==value)return;++generation;clear_session();if(!value)return;
        value->AddRef();*session.put()=value;
        ComPtr<Event> changed;*changed.put()=new Changed(state,L"{2bdf1426-d41f-5896-897f-efc0b0fa7392}");playback=SUCCEEDED(session->PlaybackChanged(changed.get(),&playback_token));
        *changed.put()=new Changed(state,L"{0f2ce2b7-afa7-5ed0-8cb6-8c40cf9b3a5f}");properties=SUCCEEDED(session->PropertiesChanged(changed.get(),&properties_token));
    }
};
MediaSnapshot read_playback(Session* session) {
    MediaSnapshot snapshot;if(!session)return snapshot;
    String source;if(FAILED(session->Source(&source.value)))return snapshot;snapshot.source=source.text();
    ComPtr<Playback> playback;if(FAILED(session->PlaybackInfo(playback.put()))||!playback)return snapshot;
    INT32 status=0;if(FAILED(playback->Status(&status))||status==0)return {};snapshot.available=true;snapshot.playing=status==4;
    ComPtr<Controls> controls;if(SUCCEEDED(playback->ControlsInfo(controls.put()))&&controls){boolean previous=false,toggle=false,next=false,play=false,pause=false;controls->Previous(&previous);controls->Toggle(&toggle);controls->Next(&next);controls->Play(&play);controls->Pause(&pause);snapshot.previous=previous;snapshot.toggle=toggle||(snapshot.playing?pause:play);snapshot.next=next;}
    return snapshot;
}
void read_metadata(Session* session,MediaSnapshot& snapshot,std::stop_token stop,const std::shared_ptr<Shared>& state) {
    if(!session||!snapshot.available)return;
    ComPtr<Async<Properties*>> operation;ComPtr<Properties> properties;
    if(SUCCEEDED(session->MediaProperties(operation.put()))&&SUCCEEDED(result(operation.get(),properties.put(),stop,state))&&properties){String title,artist;properties->Title(&title.value);properties->Artist(&artist.value);snapshot.title=title.label(512);snapshot.artist=artist.label(128);}
    if(snapshot.title.empty())snapshot.title=L"Current media";
}
void publish(const std::shared_ptr<Shared>& state,MediaSnapshot value,std::stop_token stop) {
    std::lock_guard lock(state->mutex);if(stop.stop_requested()||state->snapshot==value)return;
    state->snapshot=std::move(value);if(state->notify)PostMessageW(state->notify,media_changed_message,0,0);
}
void run(const std::shared_ptr<Shared>& state,std::stop_token stop) {
    struct Apartment {HRESULT initialized=RoInitialize(RO_INIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(initialized))RoUninitialize();}} apartment;
    if(FAILED(apartment.initialized)){MediaSnapshot value;value.unavailable=true;publish(state,value,stop);return;}
    {
        Subscriptions subscriptions;
        while(!stop.stop_requested()) {
            if(!subscriptions.manager){
                String name;constexpr wchar_t runtime[]=L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
                WindowsCreateString(runtime,static_cast<UINT32>(std::size(runtime)-1),&name.value);
                ComPtr<Statics> factory;ComPtr<Async<Manager*>> operation;
                auto hr=RoGetActivationFactory(name.value,guid(L"{2050c4ee-11a0-57de-aed7-c97c70338245}"),reinterpret_cast<void**>(factory.put()));
                if(SUCCEEDED(hr))hr=factory->Request(operation.put());
                if(SUCCEEDED(hr))hr=result(operation.get(),subscriptions.manager.put(),stop,state);
                if(SUCCEEDED(hr)&&!subscriptions.manager)hr=E_FAIL;
                if(FAILED(hr)){MediaSnapshot value;value.unavailable=true;publish(state,value,stop);std::unique_lock lock(state->mutex);state->commands.clear();state->wake.wait_for(lock,stop,std::chrono::seconds(5),[]{return false;});continue;}
                subscriptions.bind_manager(state);
            }
            std::deque<PendingCommand> commands;{std::lock_guard lock(state->mutex);state->dirty=false;commands.swap(state->commands);}
            ComPtr<Session> current;
            if(FAILED(subscriptions.manager->Current(current.put()))){subscriptions.clear_manager();MediaSnapshot value;value.unavailable=true;publish(state,value,stop);}
            else {
                subscriptions.bind_session(current.get(),state);
                if(current){String source;current->Source(&source.value);for(const auto& request:commands){if(request.source!=source.text()||request.session!=subscriptions.generation||stop.stop_requested())continue;
                    ComPtr<Playback> playback;ComPtr<Controls> controls;ComPtr<Async<boolean>> operation;HRESULT hr=E_FAIL;
                    if(FAILED(current->PlaybackInfo(playback.put()))||!playback||FAILED(playback->ControlsInfo(controls.put()))||!controls)continue;
                    boolean enabled=false;
                    if(request.command==MediaCommand::Previous){controls->Previous(&enabled);if(enabled)hr=current->Previous(operation.put());}
                    else if(request.command==MediaCommand::Next){controls->Next(&enabled);if(enabled)hr=current->Next(operation.put());}
                    else {
                        INT32 status=0;if(FAILED(playback->Status(&status)))continue;
                        const bool pause=request.command==MediaCommand::Pause||(request.command==MediaCommand::Toggle&&status==4);
                        // The action shown by the icon is an explicit intent.
                        // An intervening playback change must not reverse it.
                        if(pause!=(status==4))continue;
                        if(pause){controls->Pause(&enabled);if(enabled)hr=current->Pause(operation.put());}
                        else {controls->Play(&enabled);if(enabled)hr=current->Play(operation.put());}
                        if(!enabled){controls->Toggle(&enabled);if(enabled)hr=current->Toggle(operation.put());}
                    }
                    boolean accepted=false;if(SUCCEEDED(hr))result(operation.get(),&accepted,stop,state);
                }}
                auto snapshot=read_playback(current.get());snapshot.session=subscriptions.generation;
                {std::lock_guard lock(state->mutex);const auto& previous=state->snapshot;if(previous.source==snapshot.source&&previous.session==snapshot.session){snapshot.title=previous.title;snapshot.artist=previous.artist;}}
                if(snapshot.available&&snapshot.title.empty())snapshot.title=L"Current media";
                // Play/pause state reaches the GUI before a slow metadata query.
                publish(state,snapshot,stop);
                read_metadata(current.get(),snapshot,stop,state);publish(state,std::move(snapshot),stop);
            }
            std::unique_lock lock(state->mutex);const auto ready=[&]{return state->dirty||!state->commands.empty();};
            // Providers can acknowledge before applying a transport request, or
            // omit a change notification. Verify a click once after 150 ms;
            // keep a slow reconciliation fallback without repainting idle UI.
            state->wake.wait_for(lock,stop,commands.empty()?std::chrono::milliseconds(5000):std::chrono::milliseconds(150),ready);
        }
    }
}
}
struct MediaController::Impl {
    std::shared_ptr<Shared> state=std::make_shared<Shared>();
    std::jthread worker;
    void stop(){{std::lock_guard lock(state->mutex);state->running=false;}if(worker.joinable()){worker.request_stop();state->wake.notify_all();worker.join();}std::lock_guard lock(state->mutex);state->notify=nullptr;state->commands.clear();}
    ~Impl(){stop();}
};
MediaController::MediaController():impl_(std::make_unique<Impl>()){}
MediaController::~MediaController()=default;
void MediaController::start(HWND notify){impl_->stop();{std::lock_guard lock(impl_->state->mutex);impl_->state->notify=notify;impl_->state->dirty=impl_->state->running=true;impl_->state->snapshot={};}const auto state=impl_->state;impl_->worker=std::jthread([state](std::stop_token stop){try{run(state,stop);}catch(...){MediaSnapshot value;value.unavailable=true;publish(state,value,stop);}});}
void MediaController::stop(){impl_->stop();}
MediaSnapshot MediaController::snapshot() const{std::lock_guard lock(impl_->state->mutex);return impl_->state->snapshot;}
bool MediaController::command(MediaCommand command,const MediaSnapshot& expected){std::lock_guard lock(impl_->state->mutex);const auto& s=impl_->state->snapshot;const bool enabled=command==MediaCommand::Previous?s.previous:command==MediaCommand::Next?s.next:s.toggle;if(!impl_->state->running||!s.available||!enabled||s.source!=expected.source||s.session!=expected.session||impl_->state->commands.size()>=8)return false;impl_->state->commands.push_back({command,s.source,s.session});impl_->state->wake.notify_all();return true;}
}
