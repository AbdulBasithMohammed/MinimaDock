// Exercises our tray receiver using actual Shell_NotifyIcon calls and owned icons.
// No commands are sent to installed applications and no tray preferences change.
#include "windows/tray.h"
#include <shellapi.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr UINT legacy_message=WM_APP+101,modern_message=WM_APP+102,cancel_menu=WM_APP+103;
GUID legacy_guid{},modern_guid{};
std::atomic_uint legacy_left=0,legacy_right=0,modern_left=0,modern_right=0,menu_depth=0;
std::atomic_bool bad_identity=false;
std::atomic_uint legacy_event=0,modern_event=0;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==legacy_message||message==modern_message) {
        const bool modern=message==modern_message;
        const UINT event=modern?LOWORD(l):static_cast<UINT>(l),id=modern?HIWORD(l):static_cast<UINT>(w);
        (modern?modern_event:legacy_event)=event;
        if(id!=(modern?42u:41u))bad_identity=true;
        if(event==WM_LBUTTONUP||event==NIN_SELECT||event==NIN_KEYSELECT){if(modern)++modern_left;else ++legacy_left;}
        if(event==(modern?WM_CONTEXTMENU:WM_RBUTTONUP)) {
            if(modern)++modern_right;else ++legacy_right;
            auto menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,modern?L"Modern fixture options":L"Legacy fixture options");
            POINT point{};GetCursorPos(&point);SetForegroundWindow(window);++menu_depth;
            TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window,nullptr);
            --menu_depth;DestroyMenu(menu);
        }
        return 0;
    }
    if(message==cancel_menu){EndMenu();return 0;}
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
NOTIFYICONDATAW icon(HWND fixture,bool modern) {
    NOTIFYICONDATAW data{};data.cbSize=sizeof(data);data.hWnd=fixture;data.uID=modern?42:41;
    data.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_GUID;data.uCallbackMessage=modern?modern_message:legacy_message;
    data.guidItem=modern?modern_guid:legacy_guid;data.hIcon=LoadIconW(nullptr,modern?IDI_INFORMATION:IDI_APPLICATION);
    wcscpy_s(data.szTip,modern?L"MinimaDock modern tray fixture":L"MinimaDock legacy tray fixture");return data;
}
template<class Check> void await(Check check,const char* message){for(unsigned i=0;i<150;++i){if(check())return;std::this_thread::sleep_for(std::chrono::milliseconds(20));}throw std::runtime_error(message);}

}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--receiver"){
        WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"MinimaDock.TrayCrashFixture";RegisterClassW(&c);
        const auto w=CreateWindowExW(WS_EX_TOOLWINDOW,c.lpszClassName,L"Owned crash fixture",WS_POPUP,0,0,0,0,nullptr,nullptr,c.hInstance,nullptr);minima::TrayPopup tray;tray.prepare(w);
        MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}tray.stop();return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(CoCreateGuid(&legacy_guid))||FAILED(CoCreateGuid(&modern_guid)))return 1;
    const auto previous=GetForegroundWindow();POINT cursor{};GetCursorPos(&cursor);
    WNDCLASSW klass{};klass.lpfnWndProc=proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.TrayFixture";RegisterClassW(&klass);
    const auto fixture=CreateWindowExW(WS_EX_TOOLWINDOW,klass.lpszClassName,L"MinimaDock tray fixture",WS_OVERLAPPEDWINDOW,100,100,280,140,nullptr,nullptr,klass.hInstance,nullptr);
    if(!fixture)return 1;ShowWindow(fixture,SW_SHOWNOACTIVATE);
    auto legacy=icon(fixture,false),modern=icon(fixture,true);int result=0;
    std::jthread test([&]{
        minima::TrayPopup tray;
        try {
            tray.prepare(fixture);await([&]{return tray.ready();},"Tray preparation timed out");expect(tray.available(),"Custom tray receiver unavailable");
            expect(FindWindowW(L"Shell_TrayWnd",nullptr)==tray.receiver(),"Custom receiver was not selected");
            expect(Shell_NotifyIconW(NIM_ADD,&legacy)&&Shell_NotifyIconW(NIM_ADD,&modern),"Native notification add failed");
            modern.uVersion=NOTIFYICON_VERSION_4;expect(Shell_NotifyIconW(NIM_SETVERSION,&modern),"Native version negotiation failed");
            unsigned long long legacy_key=0,modern_key=0;
            await([&]{for(const auto& i:tray.snapshot()){if(i.title==L"MinimaDock legacy tray fixture")legacy_key=i.key;if(i.title==L"MinimaDock modern tray fixture")modern_key=i.key;}return legacy_key&&modern_key;},"Own icons not captured");
            expect(tray.open({},96)&&tray.tick(),"Custom panel did not open");
            const POINT point{180,160};const RECT icon_bounds{160,140,204,184};tray.set_bounds(legacy_key,icon_bounds);
            NOTIFYICONIDENTIFIER rect_id{};rect_id.cbSize=sizeof(rect_id);rect_id.hWnd=fixture;rect_id.uID=41;rect_id.guidItem=legacy_guid;RECT rect{};
            const auto rect_result=Shell_NotifyIconGetRect(&rect_id,&rect);if(FAILED(rect_result)||!EqualRect(&rect,&icon_bounds))std::cerr<<"Rectangle result="<<rect_result<<" coords="<<rect.left<<","<<rect.top<<","<<rect.right<<","<<rect.bottom<<"\n";expect(SUCCEEDED(rect_result)&&EqualRect(&rect,&icon_bounds),"App icon rectangle did not use custom panel coordinates");
            for(bool modern_icon:{false,true}) {
                const auto key=modern_icon?modern_key:legacy_key;const auto left=modern_icon?modern_left.load():legacy_left.load();
                expect(tray.activate(key,false,point),"Original activation dispatch failed");
                await([&]{return (modern_icon?modern_left.load():legacy_left.load())>left;},"Original activation callback missing");
                const auto right=modern_icon?modern_right.load():legacy_right.load();expect(tray.activate(key,true,point),"Original menu dispatch failed");
                await([&]{return (modern_icon?modern_right.load():legacy_right.load())==right+1&&menu_depth>0;},"Original app menu missing or duplicated");
                expect(tray.tick(),"App menu lost tray hold");PostMessageW(fixture,cancel_menu,0,0);await([]{return menu_depth==0;},"Owned menu did not close");
            }
            expect(!bad_identity,"Legacy/version-4 callback identity altered");
            wcscpy_s(legacy.szTip,L"MinimaDock updated tray fixture");const auto saved_owner=legacy.hWnd;legacy.hWnd=nullptr;legacy.uFlags=NIF_GUID|NIF_TIP;expect(Shell_NotifyIconW(NIM_MODIFY,&legacy),"GUID-only native tooltip update failed");legacy.hWnd=saved_owner;
            await([&]{for(const auto& i:tray.snapshot())if(i.key==legacy_key&&i.title==L"MinimaDock updated tray fixture")return true;return false;},"Custom tooltip did not update");
            legacy.uFlags=NIF_GUID|NIF_STATE;legacy.dwStateMask=NIS_HIDDEN;legacy.dwState=NIS_HIDDEN;Shell_NotifyIconW(NIM_MODIFY,&legacy);
            await([&]{for(const auto& i:tray.snapshot())if(i.key==legacy_key)return false;return true;},"Hidden icon remained visible");
            legacy.dwState=0;Shell_NotifyIconW(NIM_MODIFY,&legacy);
            await([&]{for(const auto& i:tray.snapshot())if(i.key==legacy_key)return true;return false;},"Unhidden icon was lost");
            BYTE malformed[12]{};COPYDATASTRUCT invalid{1,sizeof(malformed),malformed};expect(SendMessageW(tray.receiver(),WM_COPYDATA,reinterpret_cast<WPARAM>(fixture),reinterpret_cast<LPARAM>(&invalid))==0,"Malformed notification accepted");
            expect(Shell_NotifyIconW(NIM_DELETE,&legacy)&&Shell_NotifyIconW(NIM_DELETE,&modern),"Native deletion failed");
            await([&]{for(const auto& i:tray.snapshot())if(i.key==legacy_key||i.key==modern_key)return false;return true;},"Deleted icon remained captured");
            expect(!tray.activate(legacy_key,false,point),"Deleted icon callback was still dispatchable");
            tray.close();expect(!tray.tick()&&!tray.window(),"Custom panel hold did not close");tray.stop();
            expect(FindWindowW(L"Shell_TrayWnd",nullptr)!=tray.receiver(),"Receiver was not removed");
            // Startup cancellation must never wait forever for the receiver loop.
            for(unsigned i=0;i<3;++i){tray.prepare(fixture);tray.stop();}
            wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);std::wstring command=L"\""+std::wstring(executable)+L"\" --receiver";
            STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION child{};
            expect(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)!=FALSE,"Owned crash receiver did not start");CloseHandle(child.hThread);
            try {
                await([&]{DWORD pid=0;GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd",nullptr),&pid);return pid==child.dwProcessId;},"Owned crash receiver was not selected");
                legacy.uFlags=NIF_GUID|NIF_ICON|NIF_MESSAGE|NIF_TIP;expect(Shell_NotifyIconW(NIM_ADD,&legacy),"Crash fixture icon was not added");
                expect(TerminateProcess(child.hProcess,0)&&WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0,"Owned crash receiver did not stop");
                wcscpy_s(legacy.szTip,L"MinimaDock crash recovery fixture");expect(Shell_NotifyIconW(NIM_MODIFY,&legacy),"Icon update did not return to Explorer after crash");
                RECT recovered{};expect(SUCCEEDED(Shell_NotifyIconGetRect(&rect_id,&recovered))&&recovered.right>recovered.left,"Explorer's mirrored icon was lost after crash");
                Shell_NotifyIconW(NIM_DELETE,&legacy);CloseHandle(child.hProcess);
            }catch(...){TerminateProcess(child.hProcess,0);WaitForSingleObject(child.hProcess,3000);CloseHandle(child.hProcess);throw;}
            std::cout<<"Custom tray passed: actual notification capture, icon bounds, legacy/version-4 activation and original menus, updates, hidden state, malformed data, deletion, startup cancellation and crash mirror recovery\n";
        }catch(const std::exception& error){result=1;PostMessageW(fixture,cancel_menu,0,0);std::cerr<<error.what()<<"; own last events="<<legacy_event<<','<<modern_event<<" bad id="<<bad_identity<<'\n';}
        Shell_NotifyIconW(NIM_DELETE,&legacy);Shell_NotifyIconW(NIM_DELETE,&modern);tray.stop();PostMessageW(fixture,WM_CLOSE,0,0);
    });
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    test.join();Shell_NotifyIconW(NIM_DELETE,&legacy);Shell_NotifyIconW(NIM_DELETE,&modern);
    SetCursorPos(cursor.x,cursor.y);if(IsWindow(previous))SetForegroundWindow(previous);return result;
}
