#include "windows/media.h"
#include "windows/media_abi.h"
#include "windows/media_intent.h"
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
struct PendingCommand {MediaCommand command;std::wstring source;std::uint64_t session,sequence=0;};
struct Shared {
    std::mutex mutex;
    std::condition_variable_any wake;
    bool dirty=true,metadata_dirty=true,running=false;
    HWND notify=nullptr;
    MediaSnapshot snapshot;
    MediaIntent intent;
    std::deque<PendingCommand> commands;
};
class Changed final : public Event {
    std::atomic<ULONG> references{1};
    std::shared_ptr<Shared> state;
    GUID iid;
    bool metadata;
public:
    Changed(std::shared_ptr<Shared> state,const wchar_t* id,bool metadata=true):state(std::move(state)),iid(guid(id)),metadata(metadata){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID requested,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(requested!=IID_IUnknown&&requested!=IID_IAgileObject&&requested!=iid)return E_NOINTERFACE;
        *result=static_cast<Event*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) override {
        {std::lock_guard lock(state->mutex);state->dirty=true;state->metadata_dirty|=metadata;}state->wake.notify_all();return S_OK;
    }
};
// IAsyncOperationCompletedHandler<T>: wakes the worker instead of a 10 ms poll.
// The operation only calls Invoke through this vtable; it needs no typed IID.
struct CompletedHandler : IUnknown {virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown*,AsyncStatus)=0;};
class Completion final : public CompletedHandler {
    std::atomic<ULONG> references{1};
    std::shared_ptr<Shared> state;
    std::shared_ptr<bool> done;
public:
    Completion(std::shared_ptr<Shared> state,std::shared_ptr<bool> done):state(std::move(state)),done(std::move(done)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID requested,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(requested!=IID_IUnknown&&requested!=IID_IAgileObject)return E_NOINTERFACE;
        *result=static_cast<CompletedHandler*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Invoke(IUnknown*,AsyncStatus) override {
        {std::lock_guard lock(state->mutex);*done=true;}state->wake.notify_all();return S_OK;
    }
};
template<class T> HRESULT result(Async<T>* operation,T* value,std::stop_token stop,const std::shared_ptr<Shared>& state,bool interrupt_for_command=false) {
    if(!operation)return E_POINTER;
    ComPtr<IAsyncInfo> info;auto hr=operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(info.put()));if(FAILED(hr))return hr;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1500);
    // Guarded by state->mutex. A refused handler falls back to the 10 ms poll.
    const auto done=std::make_shared<bool>(false);
    ComPtr<CompletedHandler> handler;*handler.put()=new Completion(state,done);
    const bool notified=SUCCEEDED(operation->put_Completed(handler.get()));
    AsyncStatus status=Started;
    while(SUCCEEDED(hr=info->get_Status(&status))&&status==Started) {
        const auto now=std::chrono::steady_clock::now();
        if(stop.stop_requested()||now>=deadline){info->Cancel();return stop.stop_requested()?E_ABORT:HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        std::unique_lock lock(state->mutex);
        if(interrupt_for_command&&!state->commands.empty()){lock.unlock();info->Cancel();return E_ABORT;}
        const auto wait=notified?std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now)+std::chrono::milliseconds(1):std::chrono::milliseconds(10);
        state->wake.wait_for(lock,stop,wait,[&]{return *done||(interrupt_for_command&&!state->commands.empty());});
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
        ComPtr<Event> changed;*changed.put()=new Changed(state,L"{2bdf1426-d41f-5896-897f-efc0b0fa7392}",false);playback=SUCCEEDED(session->PlaybackChanged(changed.get(),&playback_token));
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
bool read_metadata(Session* session,MediaSnapshot& snapshot,std::stop_token stop,const std::shared_ptr<Shared>& state) {
    if(!session||!snapshot.available)return true;
    ComPtr<Async<Properties*>> operation;ComPtr<Properties> properties;
    auto hr=session->MediaProperties(operation.put());
    if(SUCCEEDED(hr))hr=result(operation.get(),properties.put(),stop,state,true);
    if(SUCCEEDED(hr)&&properties){String title,artist;properties->Title(&title.value);properties->Artist(&artist.value);snapshot.title=title.label(512);snapshot.artist=artist.label(128);}
    if(snapshot.title.empty())snapshot.title=L"Current media";
    return hr!=E_ABORT;
}
void publish(const std::shared_ptr<Shared>& state,MediaSnapshot value,std::stop_token stop) {
    std::lock_guard lock(state->mutex);if(stop.stop_requested())return;
    value.playing=state->intent.reconcile(value.source,value.session,value.available,value.playing,MediaIntent::Clock::now());
    if(state->snapshot==value)return;
    state->snapshot=std::move(value);if(state->notify)PostMessageW(state->notify,media_changed_message,0,0);
}
void run(const std::shared_ptr<Shared>& state,std::stop_token stop) {
    struct Apartment {HRESULT initialized=RoInitialize(RO_INIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(initialized))RoUninitialize();}} apartment;
    if(FAILED(apartment.initialized)){MediaSnapshot value;value.unavailable=true;publish(state,value,stop);return;}
    {
        Subscriptions subscriptions;
        auto next_metadata=MediaIntent::Clock::now();
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
            bool metadata_dirty=false;
            std::deque<PendingCommand> commands;{std::lock_guard lock(state->mutex);state->dirty=false;metadata_dirty=state->metadata_dirty;state->metadata_dirty=false;commands.swap(state->commands);}
            ComPtr<Session> current;
            if(FAILED(subscriptions.manager->Current(current.put()))){subscriptions.clear_manager();MediaSnapshot value;value.unavailable=true;publish(state,value,stop);}
            else {
                const auto generation=subscriptions.generation;subscriptions.bind_session(current.get(),state);
                metadata_dirty|=generation!=subscriptions.generation;
                if(current){String source;current->Source(&source.value);for(const auto& request:commands){if(request.source!=source.text()||request.session!=subscriptions.generation||stop.stop_requested())continue;
                    ComPtr<Playback> playback;ComPtr<Controls> controls;ComPtr<Async<boolean>> operation;HRESULT hr=E_FAIL;
                    if(FAILED(current->PlaybackInfo(playback.put()))||!playback||FAILED(playback->ControlsInfo(controls.put()))||!controls)continue;
                    boolean enabled=false;
                    if(request.command==MediaCommand::Previous){controls->Previous(&enabled);if(enabled)hr=current->Previous(operation.put());}
                    else if(request.command==MediaCommand::Next){controls->Next(&enabled);if(enabled)hr=current->Next(operation.put());}
                    else {
                        INT32 status=0;if(FAILED(playback->Status(&status)))continue;
                        const bool pause=request.command==MediaCommand::Pause||(request.command==MediaCommand::Toggle&&status==4);
                        // Explicit play/pause stays ordered even if the previous
                        // request has not appeared in PlaybackInfo yet.
                        if(pause){controls->Pause(&enabled);if(enabled)hr=current->Pause(operation.put());}
                        else {controls->Play(&enabled);if(enabled)hr=current->Play(operation.put());}
                        if(!enabled){controls->Toggle(&enabled);if(enabled){if(pause==(status==4))hr=current->Toggle(operation.put());else hr=S_FALSE;}}
                    }
                    boolean accepted=hr==S_FALSE;if(SUCCEEDED(hr)&&operation)hr=result(operation.get(),&accepted,stop,state);
                    if(request.sequence){std::lock_guard lock(state->mutex);state->intent.complete(request.sequence,SUCCEEDED(hr)&&accepted);}
                }}
                auto snapshot=read_playback(current.get());snapshot.session=subscriptions.generation;
                {std::lock_guard lock(state->mutex);const auto& previous=state->snapshot;if(previous.source==snapshot.source&&previous.session==snapshot.session){snapshot.title=previous.title;snapshot.artist=previous.artist;}}
                if(snapshot.available&&snapshot.title.empty())snapshot.title=L"Current media";
                // Play/pause state reaches the GUI before a slow metadata query.
                publish(state,snapshot,stop);
                if(metadata_dirty||MediaIntent::Clock::now()>=next_metadata){
                    if(!read_metadata(current.get(),snapshot,stop,state)){std::lock_guard lock(state->mutex);state->metadata_dirty=true;}
                    next_metadata=MediaIntent::Clock::now()+std::chrono::seconds(5);
                    publish(state,std::move(snapshot),stop);
                }
            }
            std::unique_lock lock(state->mutex);const auto ready=[&]{return state->dirty||!state->commands.empty();};
            // Verify pending intent promptly and finitely, including providers
            // that acknowledge before applying or omit change notifications.
            const auto delay=state->intent.pending()?std::chrono::milliseconds(40):std::chrono::milliseconds(5000);
            state->wake.wait_for(lock,stop,delay,ready);
        }
    }
}

}
struct MediaController::Impl {
    std::shared_ptr<Shared> state=std::make_shared<Shared>();
    std::jthread worker;
    void stop(){{std::lock_guard lock(state->mutex);state->running=false;}if(worker.joinable()){worker.request_stop();state->wake.notify_all();worker.join();}std::lock_guard lock(state->mutex);state->notify=nullptr;state->commands.clear();state->intent.clear();}
    ~Impl(){stop();}
};
MediaController::MediaController():impl_(std::make_unique<Impl>()){}
MediaController::~MediaController()=default;
void MediaController::start(HWND notify){impl_->stop();{std::lock_guard lock(impl_->state->mutex);impl_->state->notify=notify;impl_->state->dirty=impl_->state->metadata_dirty=impl_->state->running=true;impl_->state->snapshot={};}const auto state=impl_->state;impl_->worker=std::jthread([state](std::stop_token stop){try{run(state,stop);}catch(...){MediaSnapshot value;value.unavailable=true;publish(state,value,stop);}});}
void MediaController::stop(){impl_->stop();}
MediaSnapshot MediaController::snapshot() const{std::lock_guard lock(impl_->state->mutex);return impl_->state->snapshot;}
bool MediaController::command(MediaCommand command,const MediaSnapshot& expected){
    auto& state=*impl_->state;std::lock_guard lock(state.mutex);auto& s=state.snapshot;
    const bool enabled=command==MediaCommand::Previous?s.previous:command==MediaCommand::Next?s.next:s.toggle;
    if(!state.running||!s.available||!enabled||s.source!=expected.source||s.session!=expected.session||state.commands.size()>=8)return false;
    const bool transport=command==MediaCommand::Play||command==MediaCommand::Pause||command==MediaCommand::Toggle;
    const bool playing=command==MediaCommand::Play||(command==MediaCommand::Toggle&&!s.playing);
    const auto sequence=transport?state.intent.begin(s.source,s.session,playing,MediaIntent::Clock::now()):0;
    if(transport)command=playing?MediaCommand::Play:MediaCommand::Pause;
    state.commands.push_back({command,s.source,s.session,sequence});
    if(transport){s.playing=playing;if(state.notify)PostMessageW(state.notify,media_changed_message,0,0);}
    state.wake.notify_all();return true;
}
}
