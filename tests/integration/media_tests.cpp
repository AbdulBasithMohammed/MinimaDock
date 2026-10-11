#include "windows/media.h"
#include "platform/com_ptr.h"
#ifdef __MINGW32__
// WIDL currently emits both BYTE and boolean as unsigned char template
// specializations. This fixture never uses IReference<BYTE>.
#define ____FIReference_1_BYTE_INTERFACE_DEFINED__
#endif
#include <windows.media.h>
#include <winstring.h>
#include <shobjidl.h>
#include <systemmediatransportcontrolsinterop.h>
#include <roapi.h>
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>

using namespace minima;
using namespace ABI::Windows::Media;
using Buttons=ABI::Windows::Foundation::ITypedEventHandler<SystemMediaTransportControls*,SystemMediaTransportControlsButtonPressedEventArgs*>;
namespace {
struct HandoffChecked {};
struct CompetingPlayer {};
HWND fixture=nullptr;
ComPtr<ISystemMediaTransportControls> transport;
int track=1;
std::atomic<int> received{0},last_button{-1};
void require(HRESULT hr){if(FAILED(hr))throw hr;}
void label(IMusicDisplayProperties* properties,const std::wstring& value,bool artist=false){HSTRING text=nullptr;require(WindowsCreateString(value.c_str(),static_cast<UINT32>(value.size()),&text));const auto hr=artist?properties->put_Artist(text):properties->put_Title(text);WindowsDeleteString(text);require(hr);}
void metadata(){ComPtr<ISystemMediaTransportControlsDisplayUpdater> updater;require(transport->get_DisplayUpdater(updater.put()));require(updater->put_Type(MediaPlaybackType_Music));ComPtr<IMusicDisplayProperties> properties;require(updater->get_MusicProperties(properties.put()));label(properties.get(),L"MinimaDock isolated media fixture "+std::to_wstring(track));label(properties.get(),L"Fixture artist",true);require(updater->Update());}
LRESULT CALLBACK proc(HWND window,UINT msg,WPARAM w,LPARAM l){if(msg==WM_APP+1){try{const auto button=static_cast<SystemMediaTransportControlsButton>(w);if(button==SystemMediaTransportControlsButton_Play)require(transport->put_PlaybackStatus(MediaPlaybackStatus_Playing));else if(button==SystemMediaTransportControlsButton_Pause)require(transport->put_PlaybackStatus(MediaPlaybackStatus_Paused));else if(button==SystemMediaTransportControlsButton_Next){++track;metadata();}else if(button==SystemMediaTransportControlsButton_Previous){--track;metadata();}++received;}catch(...){received=-100;}return 0;}return DefWindowProcW(window,msg,w,l);}
class Pressed final : public Buttons {
    std::atomic<ULONG> references{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override{if(!result)return E_POINTER;*result=nullptr;if(iid!=IID_IUnknown&&iid!=IID_IAgileObject&&iid!=__uuidof(Buttons))return E_NOINTERFACE;*result=static_cast<Buttons*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Invoke(ISystemMediaTransportControls*,ISystemMediaTransportControlsButtonPressedEventArgs* args) override{SystemMediaTransportControlsButton button{};if(FAILED(args->get_Button(&button)))return E_FAIL;last_button=button;PostMessageW(fixture,WM_APP+1,button,0);return S_OK;}
};
template<class F> void await(F check,const char* message){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);do{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(check())return;Sleep(10);}while(std::chrono::steady_clock::now()<deadline);throw std::runtime_error(message);}
struct Dock {
    PROCESS_INFORMATION process{};HWND window=nullptr;
    ~Dock(){if(process.hProcess){if(window)PostMessageW(window,WM_CLOSE,0,0);if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);CloseHandle(process.hProcess);}}
    LRESULT read(UINT offset){DWORD_PTR value=0;if(!SendMessageTimeoutW(window,WM_APP+40+offset,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&value))throw std::runtime_error("Own dock did not respond.");return static_cast<LRESULT>(value);}
    void open(const std::filesystem::path& path){auto command=L"\""+std::filesystem::absolute(path).wstring()+L"\" --dock";STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))throw std::runtime_error("Own media dock failed to start.");
        await([&]{EnumWindows([](HWND candidate,LPARAM parameter)->BOOL{auto self=reinterpret_cast<Dock*>(parameter);DWORD pid=0;GetWindowThreadProcessId(candidate,&pid);wchar_t name[128]{};GetClassNameW(candidate,name,128);if(pid==self->process.dwProcessId&&wcscmp(name,L"MinimaDock.DesktopDock")==0){self->window=candidate;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(this));return window&&read(0)==1;},"Own media dock did not become ready.");
        SendMessageW(window,WM_KEYDOWN,VK_TAB,0);await([&]{return read(1)==2&&read(9)==1000;},"Own media dock did not expand.");}
    void title(int index){const auto label=L"MinimaDock isolated media fixture "+std::to_wstring(index);await([&]{return static_cast<std::uint32_t>(read(32))==media_label_hash(label);},"Dock did not show the own fixture track title.");}
    void click(UINT control){title(track);const auto scale=GetDpiForWindow(window)/96.0;const int x=static_cast<int>(std::lround(read(28+control)*scale/1000)),y=static_cast<int>(std::lround(read(31)*scale/1000));const auto point=MAKELPARAM(x,y);SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(window,WM_LBUTTONUP,0,point);}
};
}
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--inspect") {
        MediaController reader;reader.start(nullptr);
        for(int i=0;i<150;++i){const auto state=reader.snapshot();if(state.available||state.unavailable)break;Sleep(20);}
        const auto state=reader.snapshot();Sleep(5300);const auto later=reader.snapshot();reader.stop();
        std::wcout<<L"Current player="<<state.source<<L" available="<<state.available<<L" playing="<<state.playing<<L" previous="<<state.previous<<L" toggle="<<state.toggle<<L" next="<<state.next<<L" generation="<<state.session<<L" later="<<later.session<<L'\n';
        return state.unavailable?1:0;
    }
    const auto previous=GetForegroundWindow();
    const auto apartment=RoInitialize(RO_INIT_MULTITHREADED);if(FAILED(apartment))return 1;
    int exit=0;EventRegistrationToken token{};bool subscribed=false;MediaController controller;Dock dock;
    try {
        require(SetCurrentProcessExplicitAppUserModelID(L"MinimaDock.MediaFixture"));
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MinimaDock.MediaFixture";RegisterClassW(&cls);
        fixture=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"MinimaDock isolated media fixture",WS_OVERLAPPEDWINDOW,60,60,280,120,nullptr,nullptr,cls.hInstance,nullptr);if(!fixture)throw E_FAIL;
        ShowWindow(fixture,SW_SHOWNOACTIVATE);
        SetForegroundWindow(fixture);
        HSTRING name=nullptr;constexpr wchar_t runtime[]=L"Windows.Media.SystemMediaTransportControls";require(WindowsCreateString(runtime,static_cast<UINT32>(std::size(runtime)-1),&name));
        ComPtr<ISystemMediaTransportControlsInterop> interop;const auto hr=RoGetActivationFactory(name,__uuidof(ISystemMediaTransportControlsInterop),reinterpret_cast<void**>(interop.put()));WindowsDeleteString(name);require(hr);
        require(interop->GetForWindow(fixture,__uuidof(ISystemMediaTransportControls),reinterpret_cast<void**>(transport.put())));
        ComPtr<Buttons> handler;*handler.put()=new Pressed;require(transport->add_ButtonPressed(handler.get(),&token));subscribed=true;
        require(transport->put_IsPlayEnabled(true));require(transport->put_IsPauseEnabled(true));require(transport->put_IsNextEnabled(true));require(transport->put_IsPreviousEnabled(true));
        metadata();require(transport->put_IsEnabled(true));require(transport->put_PlaybackStatus(MediaPlaybackStatus_Playing));controller.start(nullptr);
        try{await([&]{return controller.snapshot().title==L"MinimaDock isolated media fixture 1";},"Own SMTC session was not selected/read.");}
        catch(const std::exception&){if(controller.snapshot().available&&controller.snapshot().title!=L"MinimaDock isolated media fixture 1")throw CompetingPlayer{};throw;}
        auto snapshot=controller.snapshot();if(!snapshot.available||!snapshot.playing||!snapshot.toggle||!snapshot.next||!snapshot.previous||snapshot.artist!=L"Fixture artist")throw std::runtime_error("Playback/capability/artist mapping failed.");
        if(controller.command(MediaCommand::Next,MediaSnapshot{}))throw std::runtime_error("Stale session command was accepted.");
        if(!controller.command(MediaCommand::Next,snapshot))throw std::runtime_error("Next command was rejected.");
        await([&]{return received==1&&controller.snapshot().title==L"MinimaDock isolated media fixture 2";},"Next/title event failed.");
        if(!controller.command(MediaCommand::Previous,snapshot))throw std::runtime_error("Previous command was rejected.");
        await([&]{return received==2&&controller.snapshot().title==L"MinimaDock isolated media fixture 1";},"Previous/title event failed.");
        if(!controller.command(MediaCommand::Pause,snapshot))throw std::runtime_error("Pause command was rejected.");
        await([&]{MediaPlaybackStatus status{};transport->get_PlaybackStatus(&status);const auto current=controller.snapshot();return received==3&&status==MediaPlaybackStatus_Paused&&(!current.playing||current.source!=snapshot.source);},"Pause did not reach own SMTC/event update.");
        if(controller.snapshot().source!=snapshot.source){
            if(controller.command(MediaCommand::Toggle,snapshot))throw std::runtime_error("Handoff accepted a stale player command.");
            // The remaining fixture actions require ours to be current. Do not
            // pause another player's audio to force that environmental condition.
            throw HandoffChecked{};
        }
        if(!controller.command(MediaCommand::Play,snapshot))throw std::runtime_error("Play command was rejected.");
        await([&]{return received==4&&controller.snapshot().playing;},"Play did not reach own SMTC/event update.");
        if(argc==2){dock.open(argv[1]);dock.title(1);const auto anchor=dock.read(16),allocations=dock.read(17);
            if(dock.read(27)!=31)throw std::runtime_error("Dock did not reflect media capabilities/playing state.");
            dock.click(1);await([&]{return received==5&&((dock.read(27)&2)==0||static_cast<std::uint32_t>(dock.read(32))!=media_label_hash(L"MinimaDock isolated media fixture 1"));},"Dock pause hit/action/status update failed.");
            if(static_cast<std::uint32_t>(dock.read(32))!=media_label_hash(L"MinimaDock isolated media fixture 1")){require(transport->put_PlaybackStatus(MediaPlaybackStatus_Playing));dock.title(1);require(transport->put_PlaybackStatus(MediaPlaybackStatus_Changing));await([&]{return static_cast<std::uint32_t>(dock.read(32))==media_label_hash(L"MinimaDock isolated media fixture 1")&&(dock.read(27)&2)==0;},"Dock did not show the own changing media session.");}
            dock.click(1);await([&]{return received==6&&(dock.read(27)&2)!=0;},"Dock play hit/action/status update failed.");
            dock.click(2);await([&]{return received==7;},"Dock next action failed.");dock.title(2);
            dock.click(0);await([&]{return received==8;},"Dock previous action failed.");dock.title(1);
            await([&]{return dock.read(20)==0&&dock.read(11)==0;},"Media click animation did not settle.");
            if(dock.read(16)!=anchor||dock.read(17)!=allocations||dock.read(5)!=44000)throw std::runtime_error("Media updates changed taskbar geometry/resources.");
            std::cout<<"Dock media passed: actual previous/play/pause/next clicks, title/status events, finite click feedback, fixed pill scale/anchor and retained resources.\n";
        }
        require(transport->put_IsNextEnabled(false));await([&]{return !controller.snapshot().next;},"Disabled capability event failed.");
        if(controller.command(MediaCommand::Next,snapshot))throw std::runtime_error("Disabled next command was accepted.");
        if(dock.window){await([&]{return (dock.read(27)&16)==0;},"Dock next stayed enabled.");const auto before=received.load();dock.click(2);Sleep(100);if(received!=before)throw std::runtime_error("Disabled dock button dispatched a command.");}
        require(transport->put_PlaybackStatus(MediaPlaybackStatus_Closed));require(transport->put_IsEnabled(false));
        await([&]{const auto s=controller.snapshot();return !s.available||s.source!=snapshot.source;},"Closed fixture stayed selected as active media.");
        if(controller.command(MediaCommand::Toggle,snapshot))throw std::runtime_error("Closed/stale media session accepted a command.");
        if(dock.window)await([&]{return static_cast<std::uint32_t>(dock.read(32))!=media_label_hash(L"MinimaDock isolated media fixture 1");},"Dock retained a closed track title.");
        const auto stopped=std::chrono::steady_clock::now();controller.stop();if(std::chrono::steady_clock::now()-stopped>std::chrono::seconds(2))throw std::runtime_error("Media shutdown was not bounded.");
        if(controller.command(MediaCommand::Toggle,snapshot))throw std::runtime_error("Stopped controller accepted a command.");
        std::cout<<"Media passed: own native session, title/artist/status/capabilities, event updates, play/pause/next/previous delivery, stale-source and disabled-command rejection, bounded shutdown. Other players were not controlled.\n";
    }catch(CompetingPlayer){std::cout<<"Media fixture skipped: Windows kept a competing player current. Read-only media startup succeeded; no transport commands were sent.\n";exit=77;}catch(HandoffChecked){std::cout<<"Media handoff passed: own title/artist/status/capabilities, next/previous delivery and title changes, pause delivery, current-player change and stale-command rejection. Remaining fixture actions skipped because Windows selected a competing player; other players were not controlled.\n";}catch(HRESULT hr){std::cerr<<"Media fixture HRESULT 0x"<<std::hex<<static_cast<unsigned long>(hr)<<'\n';exit=1;}catch(const std::exception& error){const auto s=controller.snapshot();MediaPlaybackStatus status{};if(transport)transport->get_PlaybackStatus(&status);std::cerr<<error.what()<<" received="<<received<<" button="<<last_button<<" playing="<<s.playing<<" available="<<s.available<<" toggle="<<s.toggle<<" own-title="<<(s.title.find(L"MinimaDock isolated media fixture")==0)<<" fixture-status="<<status<<'\n';exit=1;}
    controller.stop();if(transport){transport->put_IsEnabled(false);if(subscribed)transport->remove_ButtonPressed(token);}transport.reset();if(fixture)DestroyWindow(fixture);if(previous)SetForegroundWindow(previous);RoUninitialize();return exit;
}
