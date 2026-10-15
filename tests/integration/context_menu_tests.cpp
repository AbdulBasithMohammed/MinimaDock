#include "windows/app_menu.h"
#include "windows/pin_preferences.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace minima;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main() {
    const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(apartment))return 1;
    int result=0;HWND window=nullptr;
    const auto root=std::wstring(L"Software\\MinimaDock\\Tests\\Pins-")+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    wchar_t temporary[MAX_PATH]{};GetTempPathW(MAX_PATH,temporary);
    const auto file=std::filesystem::path(temporary)/(L"MinimaDock-context-fixture-"+std::to_wstring(GetCurrentProcessId())+L".txt");
    try {
        const Shortcut imported{L"Imported app",L"C:\\Fixture\\App.lnk",false,L"C:\\Fixture\\App.exe",{},L"fixture.app"};
        const Shortcut added{L"Other app",L"C:\\Fixture\\Other.exe",false,L"C:\\Fixture\\Other.exe",{},L"fixture.other"};
        Inventory inventory;inventory.pinned={imported};
        expect(save_pin_preference(imported,false,root.c_str()),"Could not save isolated unpin preference");
        apply_pin_preferences(inventory,root.c_str());expect(inventory.pinned.empty(),"Imported pin was not suppressed in dock preferences");
        expect(save_pin_preference(imported,true,root.c_str())&&save_pin_preference(added,true,root.c_str()),"Could not persist isolated pins");
        inventory.pinned={imported};apply_pin_preferences(inventory,root.c_str());apply_pin_preferences(inventory,root.c_str());
        expect(inventory.pinned.size()==2&&inventory.pinned.front().app_id==imported.app_id,"Preferences duplicated/reordered existing native pins");
        expect(read_pin_preferences(root.c_str()).size()==2,"Pin/unpin updates duplicated persisted identities");
        const Shortcut mixed{L"App",L"c:\\fixture\\app.exe",false,L"c:\\FIXTURE\\APP.EXE",{},L""};
        auto same=mixed;same.target=L"C:\\fixture\\app.exe";
        expect(pin_identity(mixed)==pin_identity(same),"Executable pin identity is case-sensitive");
        expect(!save_pin_preference(Shortcut{},true,root.c_str()),"Invalid app created a useless pin");
        WNDCLASSW klass{};klass.lpfnWndProc=DefWindowProcW;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.ContextFixture";
        expect(RegisterClassW(&klass)!=0,"Could not register blank context fixture");
        window=CreateWindowW(klass.lpszClassName,L"Fixture & window",WS_OVERLAPPEDWINDOW,0,0,300,200,nullptr,nullptr,klass.hInstance,nullptr);
        expect(window!=nullptr,"Could not create blank menu fixture");
        EnableMenuItem(GetSystemMenu(window,FALSE),SC_MAXIMIZE,MF_BYCOMMAND|MF_GRAYED);
        AppContextMenu running(window,L"Fixture app",L"",L"",nullptr,{window},false);
        expect(GetMenuState(running.menu,AppContextMenu::launch_id,MF_BYCOMMAND)&MF_GRAYED,"Unlaunchable app offers a broken Open action");
        expect(GetMenuState(running.menu,AppContextMenu::close_id,MF_BYCOMMAND)!=static_cast<UINT>(-1),"Running app omitted Close window");
        expect(running.commands.empty()&&!running.window_menu&&running.launch_position==0,"Jump list mixed in per-window commands");
        AppContextMenu system(window,L"Fixture app",L"",L"",nullptr,{window},false,true);
        bool close=false,maximize=false;
        for(const auto& action:system.commands){if(action.command==SC_CLOSE)close=true;if(action.command==SC_MAXIMIZE){maximize=true;expect(GetMenuState(system.menu,action.id,MF_BYCOMMAND)&MF_GRAYED,"Window menu lost native window command availability");}}
        expect(system.window_menu&&close&&maximize,"Shift window menu did not preserve the native system commands");
        AppContextMenu stacked(window,L"Fixture app",L"",L"",nullptr,{window,window},false,true);
        expect(GetMenuState(stacked.menu,AppContextMenu::minimize_all_id,MF_BYCOMMAND)!=static_cast<UINT>(-1)&&GetMenuState(stacked.menu,AppContextMenu::close_id,MF_BYCOMMAND)!=static_cast<UINT>(-1),"Grouped window menu lacks group commands");
        AppContextMenu closed(window,L"Pinned app",L"C:\\Fixture\\App.exe",L"",nullptr,{},true,true);
        wchar_t label[128]{};GetMenuStringW(closed.menu,AppContextMenu::pin_id,label,128,MF_BYCOMMAND);
        expect(!closed.window_menu&&std::wstring(label)==L"Unpin from taskbar"&&GetMenuState(closed.menu,AppContextMenu::close_id,MF_BYCOMMAND)==static_cast<UINT>(-1),"Pinned-only menu has incorrect pin/close actions");
        expect(menu_label(L"A&B\nC")==L"A&&B C","External labels can create unintended menu mnemonics");
        {std::ofstream stream(file);stream<<"Owned shell context-menu fixture";}
        ShellItemMenu shell;
        expect(shell.load(window,file.wstring()),"Native Shell options were not loaded for an actual fixture file");
        expect(shell.menu&&shell.popup&&GetMenuItemCount(shell.popup)>0,"Native Shell menu contains no commands");
        expect(shell.invoke(window,0,POINT{})==E_INVALIDARG,"Out-of-range selection invoked a Shell command");
        std::cout<<"Context menus passed: isolated persistent pin/unpin, identity merging, jump-list layout, Shift window menu with native disabled state, running-only Close, real Shell extension menu and escaped labels.\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}
    if(window)DestroyWindow(window);RegDeleteTreeW(HKEY_CURRENT_USER,root.c_str());
    std::error_code ignored;std::filesystem::remove(file,ignored);CoUninitialize();return result;
}
