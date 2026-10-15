#include "windows/volume.h"
#include "platform/com_ptr.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace minima {
namespace {
// Receives endpoint-volume and device notifications on audio threads; both
// only post to the GUI window, which re-reads state on its own thread.
class Notifier final : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
    std::atomic<ULONG> references{1};
    std::atomic<HWND> window;
public:
    explicit Notifier(HWND window):window(window){}
    void detach(){window=nullptr;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result) override {
        if(!result)return E_POINTER;
        if(id==__uuidof(IUnknown)||id==__uuidof(IAudioEndpointVolumeCallback))*result=static_cast<IAudioEndpointVolumeCallback*>(this);
        else if(id==__uuidof(IMMNotificationClient))*result=static_cast<IMMNotificationClient*>(this);
        else {*result=nullptr;return E_NOINTERFACE;}
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        if(const auto w=window.load())PostMessageW(w,volume_changed_message,0,0);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,ERole role,LPCWSTR) override {
        if((flow==eRender&&role==eMultimedia)||(flow==eCapture&&role==eConsole))if(const auto w=window.load())PostMessageW(w,volume_changed_message,1,0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY) override{return S_OK;}
};
}
// Background writer for slider drags. It owns its own (MTA) endpoint, rebinds
// when the default device changes, and applies only the newest level.
struct LevelWriter {
    std::mutex mutex;std::condition_variable wake;std::thread worker;
    float pending=0;bool has=false,stopping=false;std::atomic<unsigned> device_generation{0};
    void set(float value) {
        {std::lock_guard lock(mutex);pending=value;has=true;if(!worker.joinable()){stopping=false;worker=std::thread([this]{run();});}}
        wake.notify_one();
    }
    void stop() {
        {std::lock_guard lock(mutex);stopping=true;}
        wake.notify_one();if(worker.joinable())worker.join();
        std::lock_guard lock(mutex);has=false;stopping=false;
    }
    void run() {
        const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        {
            ComPtr<IMMDeviceEnumerator> devices;ComPtr<IAudioEndpointVolume> endpoint;unsigned bound=~0u;
            CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(devices.put()));
            for(;;) {
                float value=0;
                {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||has;});if(stopping)break;value=pending;has=false;}
                if(!devices)continue;
                if(const auto generation=device_generation.load();!endpoint||bound!=generation) {
                    endpoint.reset();ComPtr<IMMDevice> device;bound=generation;
                    if(SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender,eMultimedia,device.put()))&&device)
                        device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(endpoint.put()));
                }
                if(!endpoint)continue;
                endpoint->SetMasterVolumeLevelScalar(value,nullptr);
                // Raising the level is an explicit request to hear audio again.
                BOOL muted=FALSE;if(value>0&&SUCCEEDED(endpoint->GetMute(&muted))&&muted)endpoint->SetMute(FALSE,nullptr);
            }
        }
        if(SUCCEEDED(apartment))CoUninitialize();
    }
};
struct VolumeControl::Impl {
    LevelWriter writer;
    HWND notify=nullptr;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IAudioEndpointVolume> endpoint,mic;
    Notifier* notifier=nullptr;
    bool device_registered=false,volume_registered=false,mic_registered=false;
    void unbind() {
        if(endpoint&&volume_registered)endpoint->UnregisterControlChangeNotify(notifier);
        if(mic&&mic_registered)mic->UnregisterControlChangeNotify(notifier);
        volume_registered=mic_registered=false;endpoint.reset();mic.reset();
    }
    ComPtr<IAudioEndpointVolume> capture(ERole role) const {
        ComPtr<IMMDevice> device;ComPtr<IAudioEndpointVolume> result;
        if(devices&&SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture,role,device.put()))&&device&&
           FAILED(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(result.put()))))result.reset();
        return result;
    }
    void bind() {
        unbind();++writer.device_generation;if(!devices)return;
        mic=capture(eConsole);if(mic)mic_registered=SUCCEEDED(mic->RegisterControlChangeNotify(notifier));
        ComPtr<IMMDevice> device;
        if(FAILED(devices->GetDefaultAudioEndpoint(eRender,eMultimedia,device.put()))||!device)return;
        if(FAILED(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(endpoint.put()))))return;
        volume_registered=SUCCEEDED(endpoint->RegisterControlChangeNotify(notifier));
    }
};
VolumeControl::VolumeControl():impl_(std::make_unique<Impl>()){}
VolumeControl::~VolumeControl(){stop();}
void VolumeControl::start(HWND notify) {
    stop();auto& i=*impl_;i.notify=notify;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(i.devices.put()))))return;
    i.notifier=new Notifier(notify);
    i.device_registered=SUCCEEDED(i.devices->RegisterEndpointNotificationCallback(i.notifier));
    i.bind();
}
void VolumeControl::stop() {
    auto& i=*impl_;i.writer.stop();i.unbind();
    if(i.devices&&i.device_registered)i.devices->UnregisterEndpointNotificationCallback(i.notifier);
    i.device_registered=false;i.devices.reset();
    if(i.notifier){i.notifier->detach();i.notifier->Release();i.notifier=nullptr;}
}
void VolumeControl::refresh(){impl_->bind();}
bool VolumeControl::available() const{return static_cast<bool>(impl_->endpoint);}
float VolumeControl::level() const{float value=0;if(impl_->endpoint)impl_->endpoint->GetMasterVolumeLevelScalar(&value);return value;}
bool VolumeControl::muted() const{BOOL value=FALSE;if(impl_->endpoint)impl_->endpoint->GetMute(&value);return value!=FALSE;}
void VolumeControl::set_level(float value) {
    if(!impl_->endpoint)return;
    value=std::clamp(value,0.0f,1.0f);impl_->endpoint->SetMasterVolumeLevelScalar(value,nullptr);
    // Raising the level is an explicit request to hear audio again.
    if(value>0&&muted())impl_->endpoint->SetMute(FALSE,nullptr);
}
void VolumeControl::set_level_async(float value){impl_->writer.set(std::clamp(value,0.0f,1.0f));}
void VolumeControl::set_muted(bool value){if(impl_->endpoint)impl_->endpoint->SetMute(value,nullptr);}
bool VolumeControl::mic_available() const{return static_cast<bool>(impl_->mic);}
bool VolumeControl::mic_muted() const{BOOL value=FALSE;if(impl_->mic)impl_->mic->GetMute(&value);return value!=FALSE;}
void VolumeControl::set_mic_muted(bool value) {
    if(!impl_->mic)return;
    impl_->mic->SetMute(value,nullptr);
    // Calls use the communications default, which may be another microphone.
    if(const auto calls=impl_->capture(eCommunications))calls->SetMute(value,nullptr);
}
}
