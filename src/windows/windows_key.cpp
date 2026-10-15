#include "windows/windows_key.h"
#include <atomic>
#include <cwchar>
#include <thread>

namespace minima {
namespace {
constexpr wchar_t settings_key[]=L"Software\\MinimaDock\\Launcher";
constexpr WORD mask_key=0xE8; // Unassigned; tells Windows the Windows key was used in a shortcut.

// A full-screen window (game, video) on its monitor keeps Windows' own Start behaviour.
bool fullscreen_foreground() {
    const auto foreground=GetForegroundWindow();if(!foreground)return false;
    DWORD owner=0;GetWindowThreadProcessId(foreground,&owner);
    if(owner==GetCurrentProcessId()||IsIconic(foreground)||IsZoomed(foreground))return false;
    wchar_t klass[64]{};GetClassNameW(foreground,klass,64);
    if(wcscmp(klass,L"Progman")==0||wcscmp(klass,L"WorkerW")==0)return false;
    RECT bounds{};MONITORINFO info{};info.cbSize=sizeof(info);
    if(!GetWindowRect(foreground,&bounds)||!GetMonitorInfoW(MonitorFromWindow(foreground,MONITOR_DEFAULTTONEAREST),&info))return false;
    return bounds.left<=info.rcMonitor.left&&bounds.top<=info.rcMonitor.top&&bounds.right>=info.rcMonitor.right&&bounds.bottom>=info.rcMonitor.bottom;
}
}

struct WindowsKey::Impl {
    static inline Impl* current=nullptr; // The hook procedure has no context argument.
    std::thread worker;
    std::atomic<DWORD> thread_id=0;
    std::atomic<HWND> notify=nullptr;
    std::atomic<bool> on=true;
    // Touched only on the hook thread.
    bool held=false,combo=false;
    static LRESULT CALLBACK hook(int code,WPARAM message,LPARAM data) {
        if(code==HC_ACTION&&current)current->key(message,*reinterpret_cast<const KBDLLHOOKSTRUCT*>(data));
        return CallNextHookEx(nullptr,code,message,data);
    }
    void key(WPARAM message,const KBDLLHOOKSTRUCT& key) {
        if(key.flags&LLKHF_INJECTED)return; // Our own mask key, and other tools' input.
        const bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
        const bool windows=key.vkCode==VK_LWIN||key.vkCode==VK_RWIN;
        if(!windows){if(down&&held)combo=true;return;}
        if(down) {
            if(!held){held=true;combo=!on.load()||!notify.load()||fullscreen_foreground();} // Key repeat keeps the first decision.
            return;
        }
        if(!held)return;
        held=false;
        if(combo)return;
        // Tapped alone: keep Start closed and open the launcher.
        INPUT mask[2]{};
        for(auto& input:mask){input.type=INPUT_KEYBOARD;input.ki.wVk=mask_key;}
        mask[1].ki.dwFlags=KEYEVENTF_KEYUP;
        SendInput(2,mask,sizeof(INPUT));
        if(const auto window=notify.load())PostMessageW(window,windows_key_message,0,0);
    }
    void run() {
        thread_id=GetCurrentThreadId();
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE); // Creates the queue for WM_QUIT.
        const auto handle=SetWindowsHookExW(WH_KEYBOARD_LL,hook,GetModuleHandleW(nullptr),0);
        while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        if(handle)UnhookWindowsHookEx(handle);
    }
};
WindowsKey::WindowsKey():impl_(std::make_unique<Impl>()){}
WindowsKey::~WindowsKey(){stop();}
void WindowsKey::start(HWND notify) {
    stop();
    impl_->notify=notify;Impl::current=impl_.get();
    impl_->worker=std::thread([i=impl_.get()]{i->run();});
}
void WindowsKey::stop() {
    auto& i=*impl_;i.notify=nullptr;
    if(!i.worker.joinable())return;
    for(int tries=0;tries<200&&!i.thread_id.load();++tries)Sleep(5);
    if(const auto id=i.thread_id.load())PostThreadMessageW(id,WM_QUIT,0,0);
    i.worker.join();i.thread_id=0;
    if(Impl::current==&i)Impl::current=nullptr;
}
void WindowsKey::enable(bool on){impl_->on=on;}
bool WindowsKey::enabled() const{return impl_->on.load();}

bool load_windows_key_setting() {
    DWORD value=1,size=sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,settings_key,L"WindowsKey",RRF_RT_REG_DWORD,nullptr,&value,&size);
    return value!=0;
}
void save_windows_key_setting(bool on) {
    const DWORD value=on?1:0;
    RegSetKeyValueW(HKEY_CURRENT_USER,settings_key,L"WindowsKey",REG_DWORD,&value,sizeof(value));
}
bool open_start_menu() {
    INPUT keys[4]{};
    for(auto& input:keys)input.type=INPUT_KEYBOARD;
    keys[0].ki.wVk=VK_CONTROL;keys[1].ki.wVk=VK_ESCAPE;
    keys[2].ki.wVk=VK_ESCAPE;keys[2].ki.dwFlags=KEYEVENTF_KEYUP;
    keys[3].ki.wVk=VK_CONTROL;keys[3].ki.dwFlags=KEYEVENTF_KEYUP;
    return SendInput(4,keys,sizeof(INPUT))==4;
}
}
