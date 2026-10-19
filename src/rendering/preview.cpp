#include "rendering/preview.h"
#include "rendering/calendar.h"
#include "rendering/theme.h"
#include "rendering/launcher_logo.h"
#include "rendering/glass_style.h"
#include "rendering/glass.h"
#include "rendering/previews.h"
#include "rendering/jump_list.h"
#include "rendering/prayer_toast.h"
#include "rendering/customize.h"
#include "rendering/dock_settings.h"
#include "windows/jump_lists.h"
#include "windows/volume.h"
#include "rendering/icon_paths.h"
#include "taskbar/launcher_model.h"
#include "taskbar/emoji_data.h"
#include "windows/launcher_index.h"
#include "windows/file_search.h"
#include "windows/windows_key.h"
#include <map>
#include "windows/connectivity.h"
#include "windows/updates.h"
#include "windows/prayer_times.h"
#include "windows/app_update.h"
#include "windows/quick_actions.h"
#include "windows/brightness.h"
#include <tlhelp32.h>
#include <mmsystem.h>
#include "taskbar/state_machine.h"
#include "taskbar/layout.h"
#include "taskbar/media_marquee.h"
#include "taskbar/motion.h"
#include "windows/inventory.h"
#include "windows/window_state.h"
#include "windows/media.h"
#include "windows/tray.h"
#include "windows/desktop.h"
#include "windows/app_menu.h"
#include "windows/pin_preferences.h"
#include "windows/pin_watch.h"
#include "platform/com_ptr.h"
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <string_view>
#include <fstream>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace minima {
namespace {
constexpr UINT inventory_ready = WM_APP + 1, windows_changed = WM_APP + 2, visibility_changed=WM_APP+3;
constexpr UINT foreground_changed=WM_APP+4,names_changed=WM_APP+5;
constexpr UINT_PTR frame_timer = 1, reconcile_timer = 2, clock_timer = 3, media_title_timer=4, tray_timer=5, desktop_timer=6, popup_input_timer=7, render_retry_timer=8, hover_timer=9, previews_timer=10, preview_delay_timer=16,
    settings_timer=11, countdown_timer=12, alarm_timer=13, capture_timer=14, pins_timer=17, caret_timer=18, return_focus_timer=19, hide_delay_timer=20;
// Timer alarm card events (WM_APP+20..22 belong to the trial supervisor).
constexpr UINT alarm_event_message=WM_APP+24,control_list_message=WM_APP+30,pins_folder_message=WM_APP+31;
HWND event_target = nullptr;
std::atomic_bool event_queued=false;
std::atomic_bool visibility_queued=false;
std::atomic_bool names_queued=false;
// Windows already in the inventory. Their titles are read live by menus, so a
// title change (browsers, terminals, players update constantly) needs no rescan.
// Hook callbacks run on the GUI thread that owns this set.
std::unordered_set<HWND> known_windows;
void CALLBACK window_event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD event_time) {
    if(!event_target||!window||child!=CHILDID_SELF||(object!=OBJID_WINDOW&&event!=EVENT_SYSTEM_FOREGROUND))return;
    if(event==EVENT_OBJECT_NAMECHANGE&&known_windows.contains(window))return;
    if(event==EVENT_SYSTEM_FOREGROUND)PostMessageW(event_target,foreground_changed,reinterpret_cast<WPARAM>(window),event_time);
    if(event==EVENT_OBJECT_LOCATIONCHANGE) {
        if(window==GetForegroundWindow()&&!visibility_queued.exchange(true))PostMessageW(event_target,visibility_changed,0,0);
        return;
    }
    if(event!=EVENT_SYSTEM_FOREGROUND) {
    if(GetAncestor(window,GA_ROOT)!=window||GetWindowTextLengthW(window)==0)return;
    if(GetWindowLongPtrW(window,GWL_EXSTYLE)&(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))return;
    wchar_t name[128]{};GetClassNameW(window,name,128);
    if(wcscmp(name,L"Progman")==0||wcscmp(name,L"WorkerW")==0||wcsstr(name,L"TrayWnd"))return;
    }
    if(event==EVENT_OBJECT_NAMECHANGE) {
        // Only a first title can make an untitled window eligible; coalesce slowly.
        if(!names_queued.exchange(true))PostMessageW(event_target,names_changed,0,0);
        return;
    }
    if(!event_queued.exchange(true))PostMessageW(event_target,windows_changed,0,0);
}
void check(HRESULT hr) { if (FAILED(hr)) throw hr; }
std::string json_text(const std::wstring& value) {
    std::string utf8(WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),utf8.data(),static_cast<int>(utf8.size()),nullptr,nullptr);
    std::string escaped="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:utf8) {
        if(c=='"'||c=='\\'){escaped+='\\';escaped+=static_cast<char>(c);}
        else if(c<32){escaped+="\\u00";escaped+=hex[c>>4];escaped+=hex[c&15];}
        else escaped+=static_cast<char>(c);
    }
    return escaped+'"';
}
bool inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
struct Group { std::wstring name, initial, executable; std::vector<HWND> windows; std::wstring launch_path; NativeIcon icon; bool pinned=false; std::wstring app_id; ClickPulse pulse{}; bool pressed=false; std::int64_t appeared=-1; };
enum class Panel { None, Launcher, Settings, Tray, Calendar };

class Scene {
public:
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1BitmapBrush> backdrop;
    ComPtr<ID2D1LinearGradientBrush> material, sheen;
    ComPtr<ID2D1PathGeometry> launcher_mark,media_play_mark;
    ComPtr<ID2D1Layer> panel_layer;
    ClickPulse launcher_pulse,tray_pulse,settings_pulse,clock_pulse,desktop_pulse;
    int control_pressed=-1;
    std::array<ClickPulse,3> media_pulses;
    MediaSnapshot media;
    bool native_tray=false;
    std::vector<TrayIcon> tray_items;
    size_t tray_scroll=0;int tray_focus=-1;
    MediaMarquee media_marquee;
    ComPtr<IDWriteTextLayout> media_title;
    std::wstring media_title_text;
    std::wstring tooltip_label;
    ComPtr<IDWriteTextLayout> tooltip_measure;
    PanelReveal panel_reveal;
    struct IconBitmap { NativeIcon source;ComPtr<ID2D1Bitmap> bitmap; };
    std::vector<std::shared_ptr<IconBitmap>> app_icons, desktop_icons, tray_icons, app_icons_large;
    std::unordered_map<void*,std::unordered_map<UINT,std::shared_ptr<IconBitmap>>> icon_cache;
    float cache_width=0,cache_height=0;
    std::array<ComPtr<IDWriteTextFormat>, 8> fonts;
    ComPtr<IDWriteTextFormat> mono,emoji_small,emoji_big; // Text file previews; emoji.
    DockMachine machine;
    Inventory inventory;
    std::vector<Group> groups;
    Panel panel = Panel::None;
    Panel exiting_panel=Panel::None;
    float width = 1160, height = 740;
    float pointer_x = -1, pointer_y = -1;
    int focus = -1;
    bool reduced = false, high_contrast = false, transparency = true, initialized = false, pressed = false, desktop = false;
    // Smoked-crystal styling. `live_glass` means a compositor backdrop shows
    // through the recorded glass shapes; `readability_boost` thickens the
    // tint while the dock is revealed over application content.
    theme::Palette pal = theme::palette(false);
    GlassPainter painter;
    bool live_glass = false, readability_boost = false, external_draw = false;
    D2D1_MATRIX_3X2_F base_transform = D2D1::Matrix3x2F::Identity();
    HWND active_window = nullptr;
    // Hover magnification (slot 0 = launcher, i+1 = app i) and drag shifts,
    // both springs; the active-app indicator glides between icons.
    std::vector<Spring> magnify, shift, rise, spread;
    // Apps opening and closing: the pill's app count springs to its new size, new
    // icons pop in, and closed ones shrink away where they were.
    Spring slots_visual;bool slots_placed=false;
    struct Ghost {std::shared_ptr<IconBitmap> icon;std::wstring initial;float x;std::int64_t start;};
    std::vector<Ghost> ghosts;
    static constexpr float appear_time=280,vanish_time=220;
    bool appearing() const {
        if(reduced)return false;
        for(const auto& g:groups)if(g.appeared>=0&&static_cast<float>(clock_now-g.appeared)<appear_time)return true;
        return false;
    }
    // Hover motion chosen in the Dock Hover Springs preview (2026-10-14).
    static constexpr float hover_stiffness=320,hover_damping=17,hover_lift=6,hover_spread=3;
    // From Customize: hover scale, app icon size, the media wing, date line and 24-hour clock.
    float hover_grow=1.22f,icon_px=24;bool show_media=true,show_date=true,clock24=false;
    bool bar=false,bar_left=false; // Full-width bar style; apps centred or left-aligned.
    Spring tip_show,tip_x;     // Name tooltip: springs in and glides between icons.
    // Wing controls (tray, quick settings, clock, three media buttons) use the same
    // spring with a smaller lift: they are glyphs and text, not 24 DIP icons.
    static constexpr float wing_lift=3,wing_grow=.08f;
    std::array<Spring,6> wings{};
    bool wings_tuned=false;
    void sync_wings(DockMachine::Time now) {
        if(!wings_tuned){for(auto& s:wings)s.tune(hover_stiffness,hover_damping);wings_tuned=true;}
        const bool shown=machine.visible()&&machine.progress()>.85f&&dragging<0;
        const auto over=[&](D2D1_RECT_F r){return shown&&inside(r,pointer_x,pointer_y);};
        const bool targets[6]={over(tray_rect()),over(quick_settings_rect()),over(clock_rect()),
            media_enabled(0)&&over(media_button(0)),media_enabled(1)&&over(media_button(1)),media_enabled(2)&&over(media_button(2))};
        for(size_t i=0;i<wings.size();++i)wings[i].target(targets[i]?wing_lift:0,now,reduced);
    }
    float wing_scale(size_t i) const{return 1+wing_grow*std::max(0.0f,wings[i].value())/wing_lift;}
    std::wstring tip_label;
    Glide indicator; float indicator_x = 0; bool indicator_shown = false;
    // Windows that requested attention (FlashWindow) until activated.
    std::unordered_set<HWND> attention; DockMachine::Time attention_start = 0, clock_now = 0;
    int hovered = -1, tooltip_target = -1;       // slot encoding as above
    int dragging = -1; float drag_x = 0; size_t drag_slot = 0;
    float volume_level = 0; bool volume_muted = false, volume_available = false, slider_active = false;
    const wchar_t* volume_glyph() const {
        if(!volume_available)return L"\xE767";
        if(volume_muted)return L"\xE74F";
        return volume_level<.01f?L"\xE992":volume_level<.34f?L"\xE993":volume_level<.67f?L"\xE994":L"\xE995";
    }
    D2D1_RECT_F volume_track() const {const auto r=panel_rect();return D2D1::RectF(r.left+60,r.top+262,r.right-58,r.top+262);}
    D2D1_RECT_F volume_hit() const {const auto t=volume_track();return D2D1::RectF(t.left-10,t.top-13,t.right+10,t.top+13);}
    D2D1_RECT_F volume_mute_rect() const {const auto r=panel_rect();return D2D1::RectF(r.left+15,r.top+244,r.left+52,r.top+278);}
    // Brightness row under volume, only when the built-in panel exposes it (brightness.h).
    int brightness_level=0;bool brightness_available=false,brightness_active=false;
    D2D1_RECT_F brightness_track() const {const auto r=panel_rect();return D2D1::RectF(r.left+60,r.top+311,r.right-58,r.top+311);}
    D2D1_RECT_F brightness_hit() const {const auto t=brightness_track();return D2D1::RectF(t.left-10,t.top-13,t.right+10,t.top+13);}

    // ---- Quick settings: Wi-Fi, Bluetooth, mic, instant capture, timer, updates ----
    enum class SettingsPage {Main,Wifi,Bluetooth,Timer,AddTiles,Project};
    enum class TimerState {Idle,Running,Paused,Ringing};
    SettingsPage settings_page=SettingsPage::Main;
    std::wstring prayer_line; // Replaces the date under the clock when set.
    ConnectivitySnapshot net;
    UpdateStatus updates;
    bool mic_available=false,mic_muted=false,overlay_running=false;
    ULONGLONG capture_sent=0;
    // State behind the extra tiles (quick_actions.h), re-read each time the panel opens.
    DisplayState display;std::wstring keyboard_label;int keyboard_count=0;
    bool dark=false,captions_on=false,energy_on=false,vpn_on=false,rotation_sensor=false;
    // Airplane mode is "every radio we can see is off" (Windows' own flag has no public API).
    bool airplane_on() const {
        return (net.wifi_present||net.bluetooth_present)&&!(net.wifi_present&&net.wifi_on)&&!(net.bluetooth_present&&net.bluetooth_on);
    }
    std::wstring expanded,password; // Expanded row (SSID or device key); typed key.
    bool password_visible=false;
    float list_scroll=0;
    // ---- Launcher (the MinimaDock button) ----
    // Local-only search: apps (Windows' All apps list), dock actions, Settings
    // pages and answers (sums, units, dates). Nothing is sent online.
    enum class LaunchAction {WifiOn,WifiOff,BluetoothOn,BluetoothOff,MicMute,MicUnmute,Capture,Timer,Dark,Light,Lock,Sleep,Restart,Shutdown,SignOut,StartMenu,WinKeyStart,WinKeyDock,Customize};
    bool windows_key_on=false; // The Windows key opens this launcher (live dock only).
    struct LaunchResult {
        enum class Kind {Answer,App,Action,Setting,File,Window,Command,Emoji,Link};
        Kind kind=Kind::App;std::wstring name,detail,key;size_t index=0;int minutes=0;double score=0;
        HWND window=nullptr;std::wstring value{}; // Window to switch to; command, emoji or URL.
    };
    struct LaunchRow {bool header;std::wstring title;size_t result;float top,bottom;};
    std::shared_ptr<const std::vector<LauncherApp>> launch_apps=std::make_shared<std::vector<LauncherApp>>();
    std::map<std::wstring,Usage> launch_usage;
    // Installed desktop apps by program file and by program folder, so a helper
    // process (steamwebhelper.exe, a CEF/Electron child, an updater) and pins made
    // from it resolve to the real app that Start would open.
    std::shared_ptr<const std::vector<LauncherApp>> indexed_apps;
    std::unordered_map<std::wstring,size_t> app_programs,app_folders;
    static std::wstring lower_path(std::wstring p){for(auto& c:p)c=static_cast<wchar_t>(std::towlower(c));return p;}
    void index_programs() {
        if(indexed_apps==launch_apps)return;
        indexed_apps=launch_apps;app_programs.clear();app_folders.clear();
        wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);const auto system=lower_path(windows);
        for(size_t i=0;i<launch_apps->size();++i) {
            const auto program=lower_path((*launch_apps)[i].program);
            if(program.size()<5||program.compare(program.size()-4,4,L".exe")!=0)continue;
            app_programs.emplace(program,i);
            const auto slash=program.find_last_of(L'\\');
            // System folders hold many unrelated programs; never match by folder there.
            if(slash!=std::wstring::npos&&program.rfind(system,0)!=0)app_folders.emplace(program.substr(0,slash),i);
        }
    }
    // The installed app a program belongs to: itself, or the app whose own folder
    // contains it (up to four folders up).
    const LauncherApp* app_for_program(const std::wstring& exe) const {
        if(exe.empty()||app_programs.empty())return nullptr;
        auto path=lower_path(exe);
        if(const auto found=app_programs.find(path);found!=app_programs.end())return &(*launch_apps)[found->second];
        for(int level=0;level<4;++level) {
            const auto slash=path.find_last_of(L'\\');
            if(slash==std::wstring::npos||slash<3)break;
            path.resize(slash);
            if(path.ends_with(L"\\program files")||path.ends_with(L"\\program files (x86)")||path.ends_with(L"\\windowsapps"))break;
            if(const auto found=app_folders.find(path);found!=app_folders.end())return &(*launch_apps)[found->second];
        }
        return nullptr;
    }
    // A program that is not itself an installed app, mapped to the one it belongs to.
    const LauncherApp* helper_owner(const std::wstring& exe) const {
        const auto* app=app_for_program(exe);
        return app&&_wcsicmp(app->program.c_str(),exe.c_str())!=0?app:nullptr;
    }
    std::vector<LaunchResult> launch_found;
    std::vector<LaunchRow> launch_rows;
    std::map<const void*,ComPtr<ID2D1Bitmap>> app_bitmaps;
    std::shared_ptr<const FileResults> launch_files=std::make_shared<FileResults>();
    std::shared_ptr<const FilePreview> file_preview=std::make_shared<FilePreview>();
    // Tab opens the actions for the selected result (Raycast-style).
    enum class Verb {Open,Admin,ShowInFolder,OpenWith,CopyPath,CopyFile,Properties,Terminal,CopyName,CopyLink,CopyExpression,SnapLeft,SnapRight,Maximize,Minimize,CloseWindow,CopyValue,PinDock,UnpinDock};
    struct MenuItem {Verb verb;std::wstring name,keys;};
    std::vector<MenuItem> menu_items;
    bool menu_open=false,menu_anchored=false;int menu_sel=0;std::int64_t menu_opened=-1;
    D2D1_POINT_2F menu_point{}; // Right-click opens the menu at the pointer.
    Spring menu_glide;bool menu_glide_placed=false;
    int launch_sel=0,power_holding=-1,power_pending=-1,power_hover=-1;
    float launch_scroll=0;
    Spring launch_glide;bool glide_placed=false;
    std::int64_t launch_opened=-1,power_hold_start=-1,power_deadline=-1,toast_since=-1,preview_since=-1;
    std::array<std::int64_t,5> power_nudge{};
    std::wstring launch_toast;
    static constexpr float launch_width=680,launch_height=520,item_height=44,header_height=26,menu_width=264,menu_item=32;
    static constexpr std::int64_t hold_time=900,countdown_time=5000;
    struct ActionInfo {LaunchAction action;const wchar_t* name;const wchar_t* keywords;};
    static const std::vector<ActionInfo>& launch_actions() {
        static const std::vector<ActionInfo> list={
            {LaunchAction::WifiOff,L"Turn Wi-Fi off",L"wifi off wireless"},{LaunchAction::WifiOn,L"Turn Wi-Fi on",L"wifi on wireless"},
            {LaunchAction::BluetoothOff,L"Turn Bluetooth off",L"bluetooth off"},{LaunchAction::BluetoothOn,L"Turn Bluetooth on",L"bluetooth on"},
            {LaunchAction::MicMute,L"Mute microphone",L"mute mic microphone"},{LaunchAction::MicUnmute,L"Unmute microphone",L"unmute mic microphone"},
            {LaunchAction::Capture,L"Save instant clip",L"capture clip record replay nvidia"},
            {LaunchAction::Dark,L"Switch to dark mode",L"dark mode theme"},{LaunchAction::Light,L"Switch to light mode",L"light mode theme"},
            {LaunchAction::Lock,L"Lock",L"lock"},{LaunchAction::Sleep,L"Sleep",L"sleep suspend"},{LaunchAction::Restart,L"Restart",L"restart reboot"},
            {LaunchAction::Shutdown,L"Shut down",L"shut down shutdown power off"},{LaunchAction::SignOut,L"Sign out",L"sign out log off logout"},
            {LaunchAction::StartMenu,L"Open Start menu",L"start menu windows"},
            {LaunchAction::Customize,L"MinimaDock settings",L"customize settings theme color appearance dock taskbar bar prayer masjid"},
            {LaunchAction::WinKeyStart,L"Windows key opens Start",L"windows key win start menu restore default"},
            {LaunchAction::WinKeyDock,L"Windows key opens MinimaDock",L"windows key win launcher"},
        };
        return list;
    }
    static long long today_days() {
        SYSTEMTIME local{};GetLocalTime(&local);local.wHour=local.wMinute=local.wSecond=local.wMilliseconds=0;
        FILETIME file{};SystemTimeToFileTime(&local,&file);
        return static_cast<long long>(((static_cast<ULONGLONG>(file.dwHighDateTime)<<32)|file.dwLowDateTime)/864000000000ULL)-134774;
    }
    double usage_bonus(const std::wstring& key) const{const auto found=launch_usage.find(key);return found==launch_usage.end()?0:launcher::frecency(found->second.count,found->second.days);}
    // Results for the current query, grouped: Top hit, Answer, Apps, Actions, Settings.
    void search_launcher() {
        launch_found.clear();launch_rows.clear();launch_sel=0;launch_scroll=0;glide_placed=false;
        std::wstring q=query;while(!q.empty()&&q.back()==L' ')q.pop_back();while(!q.empty()&&q.front()==L' ')q.erase(q.begin());
        std::vector<std::pair<std::wstring,std::vector<LaunchResult>>> groups;
        if(special_search(q,groups)) {
        } else if(q.empty()) {
            // Before typing: a few quick actions, the apps you open most and recent files.
            std::vector<LaunchResult> quick;
            const auto add=[&](LaunchAction action,std::wstring name,int minutes=0){LaunchResult r;r.kind=LaunchResult::Kind::Action;r.name=std::move(name);r.key=L"action:"+r.name;r.index=static_cast<size_t>(action);r.minutes=minutes;quick.push_back(std::move(r));};
            add(LaunchAction::Timer,L"Start a 25 min timer",25);
            if(mic_available)add(mic_muted?LaunchAction::MicUnmute:LaunchAction::MicMute,mic_muted?L"Unmute microphone":L"Mute microphone");
            add(pal.light?LaunchAction::Dark:LaunchAction::Light,pal.light?L"Switch to dark mode":L"Switch to light mode");
            add(LaunchAction::Capture,L"Save instant clip");
            groups.push_back({L"Quick actions",std::move(quick)});
            std::vector<LaunchResult> recent;
            for(size_t i=0;i<launch_apps->size();++i){const auto& a=(*launch_apps)[i];const double s=usage_bonus(L"app:"+a.id);if(s>0)recent.push_back({LaunchResult::Kind::App,a.name,L"",L"app:"+a.id,i,0,s});}
            std::sort(recent.begin(),recent.end(),[](const LaunchResult& a,const LaunchResult& b){return a.score>b.score;});
            if(recent.size()>8)recent.resize(8);
            if(!recent.empty())groups.push_back({L"Recent",std::move(recent)});
            std::vector<LaunchResult> files;
            if(launch_files->query.empty())for(size_t i=0;i<launch_files->hits.size()&&files.size()<5;++i)files.push_back(file_result(i));
            if(!files.empty())groups.push_back({L"Recent files",std::move(files)});
        } else {
            if(auto link=web_link(q))groups.push_back({L"Web",{std::move(*link)}});
            SYSTEMTIME now{};GetLocalTime(&now);
            if(const auto a=launcher::answer(q,now.wYear,today_days()))groups.push_back({L"Answer",{{LaunchResult::Kind::Answer,a->value,a->detail,L"",0,0,1000}}});
            std::vector<LaunchResult> apps,actions,settings;
            for(size_t i=0;i<launch_apps->size();++i) {
                const auto& a=(*launch_apps)[i];const double s=launcher::match(a.name,q);
                if(s>=0)apps.push_back({LaunchResult::Kind::App,a.name,L"",L"app:"+a.id,i,0,s+usage_bonus(L"app:"+a.id)});
            }
            // "timer 25" starts a 25 minute timer.
            if(const auto lower=launcher::lower(q);lower.rfind(L"timer",0)==0) {
                const int minutes=_wtoi(lower.c_str()+5);
                if(minutes>0&&minutes<=600)actions.push_back({LaunchResult::Kind::Action,L"Start a "+std::to_wstring(minutes)+L" min timer",L"",L"action:timer",static_cast<size_t>(LaunchAction::Timer),minutes,95});
            }
            const auto& table=launch_actions();
            for(size_t i=0;i<table.size();++i) {
                // Only the Windows key choice that changes something is offered.
                if((table[i].action==LaunchAction::WinKeyStart&&!windows_key_on)||(table[i].action==LaunchAction::WinKeyDock&&(windows_key_on||!desktop)))continue;
                const double s=std::max(launcher::match(table[i].name,q),launcher::match(table[i].keywords,q)-5);
                if(s>=0)actions.push_back({LaunchResult::Kind::Action,table[i].name,L"",std::wstring(L"action:")+table[i].name,static_cast<size_t>(table[i].action),0,s+usage_bonus(std::wstring(L"action:")+table[i].name)});
            }
            const auto& pages=settings_pages();
            for(size_t i=0;i<pages.size();++i) {
                const double s=std::max(launcher::match(pages[i].name,q),launcher::match(pages[i].keywords,q)-5);
                if(s>=0)settings.push_back({LaunchResult::Kind::Setting,pages[i].name,L"Windows Settings",std::wstring(L"setting:")+pages[i].name,i,0,s+usage_bonus(std::wstring(L"setting:")+pages[i].name)});
            }
            const auto best=[](std::vector<LaunchResult>& list,size_t keep){std::stable_sort(list.begin(),list.end(),[](const LaunchResult& a,const LaunchResult& b){return a.score>b.score;});if(list.size()>keep)list.resize(keep);};
            best(apps,6);best(actions,4);best(settings,4);
            // Open windows, by title or app ("chrome", "inbox").
            std::vector<LaunchResult> windows;
            for(const auto& w:inventory.windows) {
                if(w.title.empty()||!IsWindow(w.hwnd))continue;
                const auto app=app_label(w.executable);
                const double s=std::max(launcher::match(w.title,q),launcher::match(app,q)-2);
                if(s<0)continue;
                LaunchResult r;r.kind=LaunchResult::Kind::Window;r.name=w.title;r.detail=app+(w.minimized?L" \x00B7 minimized":L"");r.window=w.hwnd;r.score=s;
                windows.push_back(std::move(r));
            }
            best(windows,5);
            // Top hit: the best non-answer result, shown first (as on the Mac).
            LaunchResult* top=nullptr;
            for(auto* list:{&apps,&actions,&settings})if(!list->empty()&&(!top||list->front().score>top->score))top=&list->front();
            if(top&&groups.empty()){auto hit=*top;for(auto* list:{&apps,&actions,&settings})if(!list->empty()&&&list->front()==top)list->erase(list->begin());groups.insert(groups.begin(),{L"Top hit",{hit}});}
            if(!windows.empty())groups.push_back({L"Windows",std::move(windows)});
            if(!apps.empty())groups.push_back({L"Apps",std::move(apps)});
            // Files arrive a moment later from the search thread; they never take the top hit.
            std::vector<LaunchResult> files;
            if(launch_files->query==q)for(size_t i=0;i<launch_files->hits.size()&&files.size()<5;++i)files.push_back(file_result(i));
            if(!files.empty())groups.push_back({L"Files",std::move(files)});
            if(!actions.empty())groups.push_back({L"Actions",std::move(actions)});
            if(!settings.empty())groups.push_back({L"Settings",std::move(settings)});
        }
        float y=0;
        for(auto& [title,items]:groups) {
            launch_rows.push_back({true,title,0,y,y+header_height});y+=header_height;
            for(auto& item:items){launch_rows.push_back({false,L"",launch_found.size(),y,y+item_height});y+=item_height;launch_found.push_back(std::move(item));}
        }
    }
    static std::wstring app_label(const std::wstring& executable) {
        auto name=executable.substr(executable.find_last_of(L"\\/")==std::wstring::npos?0:executable.find_last_of(L"\\/")+1);
        if(name.size()>4&&_wcsicmp(name.c_str()+name.size()-4,L".exe")==0)name.resize(name.size()-4);
        if(!name.empty())name[0]=static_cast<wchar_t>(std::towupper(name[0]));
        return name;
    }
    // "> command" runs in PowerShell; ":smile" or "emoji smile" finds emoji.
    bool special_search(const std::wstring& q,std::vector<std::pair<std::wstring,std::vector<LaunchResult>>>& groups) {
        if(q.empty())return false;
        if(q[0]==L'>') {
            LaunchResult r;r.kind=LaunchResult::Kind::Command;r.value=trimmed(q.substr(1));
            r.name=r.value.empty()?std::wstring(L"Open PowerShell"):L"Run \x201C"+r.value+L"\x201D";
            r.detail=L"In a new PowerShell window";
            groups.push_back({L"Command",{std::move(r)}});return true;
        }
        const auto lower=launcher::lower(q);std::wstring term;
        if(q[0]==L':')term=lower.substr(1);else if(lower.rfind(L"emoji ",0)==0)term=lower.substr(6);else if(lower!=L"emoji")return false;
        term=trimmed(term);
        std::vector<LaunchResult> list;
        for(size_t i=0;i<std::size(launcher::emoji_table);++i) {
            const auto& e=launcher::emoji_table[i];
            const double s=term.empty()?0:std::max(launcher::match(e.name,term),launcher::match(e.keywords,term)-5);
            if(s<0)continue;
            LaunchResult r;r.kind=LaunchResult::Kind::Emoji;r.name=e.name;r.name[0]=static_cast<wchar_t>(std::towupper(r.name[0]));
            r.value=e.glyph;r.key=std::wstring(L"emoji:")+e.name;r.index=i;r.score=s+usage_bonus(r.key);
            list.push_back(std::move(r));
        }
        std::stable_sort(list.begin(),list.end(),[](const LaunchResult& a,const LaunchResult& b){return a.score>b.score;});
        if(list.size()>40)list.resize(40);
        groups.push_back({L"Emoji",std::move(list)});
        return true;
    }
    static std::wstring url_encode(const std::wstring& text) {
        std::string utf8(static_cast<size_t>(std::max(0,WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr))),'\0');
        if(!utf8.empty())WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),utf8.data(),static_cast<int>(utf8.size()),nullptr,nullptr);
        std::wstring out;const wchar_t* hex=L"0123456789ABCDEF";
        for(const unsigned char c:utf8) {
            if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=static_cast<wchar_t>(c);
            else if(c==' ')out+=L'+';
            else{out+=L'%';out+=hex[c>>4];out+=hex[c&15];}
        }
        return out;
    }
    // Quicklinks ("yt lofi", "g weather", "gh minimadock", "wiki", "maps") and typed
    // web addresses. Nothing is sent until you press Enter.
    static std::optional<LaunchResult> web_link(const std::wstring& q) {
        static const struct {const wchar_t* prefix;const wchar_t* site;const wchar_t* url;} links[]={
            {L"yt",L"YouTube",L"https://www.youtube.com/results?search_query="},{L"g",L"Google",L"https://www.google.com/search?q="},
            {L"gh",L"GitHub",L"https://github.com/search?q="},{L"wiki",L"Wikipedia",L"https://en.wikipedia.org/w/index.php?search="},
            {L"maps",L"Google Maps",L"https://www.google.com/maps/search/"}};
        const auto space=q.find(L' ');
        if(space!=std::wstring::npos) {
            const auto prefix=launcher::lower(q.substr(0,space));const auto rest=trimmed(q.substr(space+1));
            for(const auto& link:links)if(prefix==link.prefix&&!rest.empty()) {
                LaunchResult r;r.kind=LaunchResult::Kind::Link;r.name=std::wstring(L"Search ")+link.site+L" for \x201C"+rest+L"\x201D";
                r.detail=L"Opens in your browser";r.value=std::wstring(link.url)+url_encode(rest);r.score=200;return r;
            }
            return std::nullopt;
        }
        const auto lower=launcher::lower(q);
        const bool scheme=lower.rfind(L"http://",0)==0||lower.rfind(L"https://",0)==0;
        if(!scheme) {
            const auto host_end=lower.find(L'/');const auto host=lower.substr(0,host_end);
            const auto dot=host.find_last_of(L'.');if(dot==std::wstring::npos||dot==0)return std::nullopt;
            static const wchar_t* domains[]={L"com",L"net",L"org",L"io",L"dev",L"ca",L"uk",L"co",L"app",L"ai",L"gov",L"edu",L"me",L"tv",L"gg",L"info",L"xyz",L"us",L"so"};
            const auto tld=host.substr(dot+1);
            if(std::none_of(std::begin(domains),std::end(domains),[&](const wchar_t* d){return tld==d;}))return std::nullopt;
        }
        LaunchResult r;r.kind=LaunchResult::Kind::Link;r.name=L"Open "+q;r.detail=L"Opens in your browser";r.value=scheme?q:L"https://"+q;r.score=200;
        return r;
    }
    static std::wstring trimmed(std::wstring q){while(!q.empty()&&q.back()==L' ')q.pop_back();while(!q.empty()&&q.front()==L' ')q.erase(q.begin());return q;}
    // The folder a file is in, with the profile shortened to "~".
    static std::wstring parent_of(const std::wstring& path) {
        auto folder=path.substr(0,path.find_last_of(L"\\/")==std::wstring::npos?0:path.find_last_of(L"\\/"));
        wchar_t profile[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"USERPROFILE",profile,MAX_PATH)) {
            const std::wstring home=profile;
            if(folder.size()>=home.size()&&_wcsnicmp(folder.c_str(),home.c_str(),home.size())==0)folder=L"~"+folder.substr(home.size());
        }
        return folder;
    }
    LaunchResult file_result(size_t i) const {
        const auto& hit=launch_files->hits[i];
        return {LaunchResult::Kind::File,hit.name,parent_of(hit.path),L"file:"+hit.path,i,0,hit.score};
    }
    const FileHit* file_of(const LaunchResult& item) const{return item.kind==LaunchResult::Kind::File&&item.index<launch_files->hits.size()?&launch_files->hits[item.index]:nullptr;}
    const LaunchResult* selected() const{return launch_found.empty()?nullptr:&launch_found[static_cast<size_t>(std::clamp(launch_sel,0,static_cast<int>(launch_found.size())-1))];}
    // A file or folder the selection stands for (files, and desktop apps).
    std::wstring selected_path() const {
        const auto* item=selected();if(!item)return {};
        if(const auto* file=file_of(*item))return file->path;
        if(item->kind==LaunchResult::Kind::App&&item->index<launch_apps->size())return app_file_path((*launch_apps)[item->index].id);
        return {};
    }
    void build_menu() {
        menu_items.clear();const auto* item=selected();if(!item)return;
        using K=LaunchResult::Kind;
        switch(item->kind) {
        case K::Answer:menu_items={{Verb::Open,L"Copy answer",L"\x21B5"},{Verb::CopyExpression,L"Copy with the sum",L""}};break;
        case K::Action:menu_items={{Verb::Open,L"Run",L"\x21B5"}};break;
        case K::Window:menu_items={{Verb::Open,L"Switch to window",L"\x21B5"},{Verb::SnapLeft,L"Snap left",L""},{Verb::SnapRight,L"Snap right",L""},
            {Verb::Maximize,L"Maximize",L""},{Verb::Minimize,L"Minimize",L""},{Verb::CloseWindow,L"Close window",L""}};break;
        case K::Command:menu_items={{Verb::Open,L"Run",L"\x21B5"},{Verb::Admin,L"Run as administrator",L"Shift+\x21B5"},{Verb::CopyValue,L"Copy command",L""}};break;
        case K::Emoji:menu_items={{Verb::Open,L"Copy emoji",L"\x21B5"},{Verb::CopyName,L"Copy name",L""}};break;
        case K::Link:menu_items={{Verb::Open,L"Open in browser",L"\x21B5"},{Verb::CopyValue,L"Copy link",L""}};break;
        case K::Setting:menu_items={{Verb::Open,L"Open in Settings",L"\x21B5"},{Verb::CopyLink,L"Copy settings link",L""}};break;
        case K::App:
            menu_items={{Verb::Open,L"Open",L"\x21B5"},{Verb::Admin,L"Run as administrator",L"Shift+\x21B5"}};
            if(const auto pin=launcher_pin(*item))menu_items.push_back(pinned_match(*pin)?MenuItem{Verb::UnpinDock,L"Unpin from dock",L""}:MenuItem{Verb::PinDock,L"Pin to dock",L""});
            if(!selected_path().empty())menu_items.push_back({Verb::ShowInFolder,L"Show in folder",L"Ctrl+\x21B5"});
            menu_items.push_back({Verb::CopyName,L"Copy name",L""});
            break;
        case K::File: {
            const auto* file=file_of(*item);
            if(file&&file->folder)menu_items={{Verb::Open,L"Open",L"\x21B5"},{Verb::ShowInFolder,L"Show in parent folder",L"Ctrl+\x21B5"},{Verb::Terminal,L"Open in Terminal",L""},
                {Verb::CopyPath,L"Copy path",L"Ctrl+Shift+C"},{Verb::Properties,L"Properties",L"Alt+\x21B5"}};
            else menu_items={{Verb::Open,L"Open",L"\x21B5"},{Verb::OpenWith,L"Open with\x2026",L""},{Verb::ShowInFolder,L"Show in folder",L"Ctrl+\x21B5"},
                {Verb::CopyPath,L"Copy path",L"Ctrl+Shift+C"},{Verb::CopyFile,L"Copy file",L""},{Verb::Properties,L"Properties",L"Alt+\x21B5"}};
            // Programs and shortcuts can be pinned, as with Windows' "Pin to taskbar".
            if(const auto pin=launcher_pin(*item))menu_items.insert(menu_items.begin()+1,pinned_match(*pin)?MenuItem{Verb::UnpinDock,L"Unpin from dock",L""}:MenuItem{Verb::PinDock,L"Pin to dock",L""});
            break;
        }
        }
    }
    // The dock pin a launcher result stands for: apps (Store apps by their app ID,
    // desktop apps by their program) and .exe/.lnk files.
    std::optional<Shortcut> launcher_pin(const LaunchResult& item) const {
        if(item.kind==LaunchResult::Kind::App&&item.index<launch_apps->size()) {
            const auto& app=(*launch_apps)[item.index];
            if(app.id.find(L'!')!=std::wstring::npos)return Shortcut{app.name,L"shell:AppsFolder\\"+app.id,false,{},{},app.id};
            const auto program=app_file_path(app.id);
            if(program.empty())return std::nullopt;
            const bool link=program.size()>4&&_wcsicmp(program.c_str()+program.size()-4,L".lnk")==0;
            return Shortcut{app.name,program,false,link?shortcut_target(program):program,{},{}};
        }
        if(const auto* file=file_of(item);file&&!file->folder) {
            const auto& p=file->path;
            const auto ends=[&](const wchar_t* e){const size_t n=wcslen(e);return p.size()>n&&_wcsicmp(p.c_str()+p.size()-n,e)==0;};
            if(ends(L".exe"))return Shortcut{file->name.substr(0,file->name.size()-4),p,false,p,{},{}};
            if(ends(L".lnk"))return Shortcut{file->name.substr(0,file->name.size()-4),p,false,shortcut_target(p),{},{}};
        }
        return std::nullopt;
    }
    const Shortcut* pinned_match(const Shortcut& candidate) const {
        for(const auto& pin:inventory.pinned) {
            if(pin_identity(pin)==pin_identity(candidate))return &pin;
            if(!candidate.app_id.empty()&&pin.app_id==candidate.app_id)return &pin;
            if(!candidate.target.empty()&&!pin.target.empty()&&_wcsicmp(pin.target.c_str(),candidate.target.c_str())==0)return &pin;
            if(const auto* app=helper_owner(pin.target);app&&!candidate.target.empty()&&_wcsicmp(app->program.c_str(),candidate.target.c_str())==0)return &pin;
        }
        return nullptr;
    }
    D2D1_RECT_F menu_rect() const {
        const auto r=panel_rect();const float h=40+menu_item*static_cast<float>(menu_items.size())+6;
        if(!menu_anchored)return D2D1::RectF(r.right-12-menu_width,r.bottom-54-h,r.right-12,r.bottom-54);
        // At the pointer, flipped to stay inside the launcher.
        float left=menu_point.x,top=menu_point.y;
        if(left+menu_width>r.right-8)left=menu_point.x-menu_width;
        if(top+h>r.bottom-8)top=menu_point.y-h;
        left=std::clamp(left,r.left+8,std::max(r.left+8,r.right-8-menu_width));top=std::clamp(top,r.top+8,std::max(r.top+8,r.bottom-8-h));
        return D2D1::RectF(left,top,left+menu_width,top+h);
    }
    D2D1_RECT_F menu_row(size_t i) const{const auto m=menu_rect();const float y=m.top+38+menu_item*static_cast<float>(i);return D2D1::RectF(m.left+6,y,m.right-6,y+menu_item);}
    D2D1_RECT_F launch_list() const{const auto r=panel_rect();return D2D1::RectF(r.left+8,r.top+60,r.right-(r.right-r.left>=600?258.0f:8.0f),r.bottom-50);}
    D2D1_RECT_F launch_preview() const{const auto r=panel_rect();return D2D1::RectF(r.right-250,r.top+57,r.right,r.bottom-47);}
    D2D1_RECT_F power_button(size_t i) const{const auto r=panel_rect();const float x=r.right-14-36*static_cast<float>(5-i);return D2D1::RectF(x,r.bottom-41,x+32,r.bottom-9);}
    D2D1_RECT_F countdown_cancel() const{const auto r=panel_rect();const float c=(r.left+r.right)/2;return D2D1::RectF(c+52,r.bottom-92,c+118,r.bottom-62);}
    const LaunchRow* row_of(size_t result) const{for(const auto& row:launch_rows)if(!row.header&&row.result==result)return &row;return nullptr;}
    // Keeps the selected row inside the list area.
    void keep_visible() {
        const auto* row=row_of(static_cast<size_t>(launch_sel));if(!row)return;const auto area=launch_list();const float height=area.bottom-area.top;
        float top=row->top;if(launch_sel==0)top=0; // Show the first group's title.
        if(top<launch_scroll)launch_scroll=top;else if(row->bottom>launch_scroll+height)launch_scroll=row->bottom-height;
    }
    ID2D1Bitmap* app_bitmap(size_t index){return index<launch_apps->size()?bitmap_of((*launch_apps)[index].image):nullptr;}
    ID2D1Bitmap* bitmap_of(const std::shared_ptr<void>& image) {
        if(!image)return nullptr;
        if(app_bitmaps.size()>600)app_bitmaps.clear();
        auto& slot=app_bitmaps[image.get()];
        if(!slot) {
            ComPtr<IWICBitmap> source;ComPtr<IWICFormatConverter> converted;
            if(SUCCEEDED(wic->CreateBitmapFromHBITMAP(static_cast<HBITMAP>(image.get()),nullptr,WICBitmapUsePremultipliedAlpha,source.put()))&&
               SUCCEEDED(wic->CreateFormatConverter(converted.put()))&&
               SUCCEEDED(converted->Initialize(source.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
                target->CreateBitmapFromWicBitmap(converted.get(),nullptr,slot.put());
        }
        return slot.get();
    }
    // Text with the characters matching the query in the accent colour.
    void highlighted(std::wstring_view label,std::wstring_view q,D2D1_RECT_F r,size_t font,D2D1_COLOR_F ink) {
        ComPtr<IDWriteTextLayout> layout;
        if(q.empty()||FAILED(write->CreateTextLayout(label.data(),static_cast<UINT32>(label.size()),fonts[font].get(),r.right-r.left,r.bottom-r.top,layout.put()))){text(label,r,font,ink);return;}
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        ComPtr<ID2D1SolidColorBrush> accent;target->CreateSolidColorBrush(pal.accent,accent.put());
        const auto marks=launcher::highlight(label,q);
        for(UINT32 i=0;i<marks.size();) {
            if(!marks[i]){++i;continue;}
            UINT32 j=i;while(j<marks.size()&&marks[j])++j;
            const DWRITE_TEXT_RANGE range{i,j-i};layout->SetDrawingEffect(accent.get(),range);layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,range);i=j;
        }
        color(ink);target->DrawTextLayout(D2D1::Point2F(r.left,r.top),layout.get(),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void result_icon(const LaunchResult& item,float cx,float cy,float size) {
        if(const auto* file=file_of(item)) {
            if(auto* bitmap=bitmap_of(file->image)){target->DrawBitmap(bitmap,D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2));return;}
        }
        if(item.kind==LaunchResult::Kind::App||item.kind==LaunchResult::Kind::File) {
            if(auto* bitmap=item.kind==LaunchResult::Kind::App?app_bitmap(item.index):nullptr){target->DrawBitmap(bitmap,D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2));return;}
            rect(D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2),pal.tile,size/4);
            text(item.name.substr(0,1),D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2),1,pal.ink,true);return;
        }
        if(item.kind==LaunchResult::Kind::Emoji){emoji(item.value,D2D1::RectF(cx-size,cy-size,cx+size,cy+size),size>40);return;}
        if(item.kind==LaunchResult::Kind::Window) {
            for(const auto& w:inventory.windows)if(w.hwnd==item.window) {
                if(const auto bitmap=bitmap_icon(w.icon,size);bitmap&&bitmap->bitmap){target->DrawBitmap(bitmap->bitmap.get(),D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2));return;}
                break;
            }
        }
        rect(D2D1::RectF(cx-size/2,cy-size/2,cx+size/2,cy+size/2),pal.tile,size/4);
        const auto glyph=item.kind==LaunchResult::Kind::Answer?Icon::Equal:item.kind==LaunchResult::Kind::Setting?Icon::Gear:
            item.kind==LaunchResult::Kind::Link?Icon::Search:item.kind==LaunchResult::Kind::Window?Icon::Plus:Icon::Zap;
        icon(glyph,cx,cy,size*.58f,pal.ink);
    }
    void draw_file_preview(const FileHit& file,const LaunchResult& item,D2D1_RECT_F pane) {
        const float x=pane.left+18,w=pane.right-18;
        const bool ready=file_preview->path==file.path;
        const std::wstring facts=(ready&&!file_preview->kind.empty()?file_preview->kind:std::wstring(file.folder?L"Folder":L"File"))+
            (file.folder?(ready?L" \x00B7 "+std::to_wstring(file_preview->children)+(file_preview->children==1?L" item":L" items"):L""):L" \x00B7 "+format_size(file.size));
        // Fades in when the preview arrives.
        const float e=ready?std::min(1.0f,since(preview_since)/180):0;
        if(ready&&file_preview->text) {
            text(file.name,D2D1::RectF(x,pane.top+10,w,pane.top+34),1,pal.ink);
            text(facts,D2D1::RectF(x,pane.top+32,w,pane.top+50),0,pal.ink_secondary);
            const auto box=D2D1::RectF(x-6,pane.top+58,w+6,pane.bottom-30);rect(box,pal.field,8);
            target->PushAxisAlignedClip(D2D1::RectF(box.left+8,box.top+6,box.right-6,box.bottom-6),D2D1_ANTIALIAS_MODE_ALIASED);
            for(size_t i=0;i<file_preview->lines.size();++i) {
                const float y=box.top+8+15*static_cast<float>(i);if(y+15>box.bottom-4)break;
                const auto& source=file_preview->lines[i];
                if(!source.empty()){color(fade(pal.ink,.85f*e));target->DrawTextW(source.c_str(),static_cast<UINT32>(source.size()),mono.get(),D2D1::RectF(box.left+8,y,box.right+400,y+15),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);}
            }
            target->PopAxisAlignedClip();
        } else if(file.folder) {
            result_icon(item,x+24,pane.top+36,48);
            text(file.name,D2D1::RectF(x,pane.top+66,w,pane.top+90),2,pal.ink);
            text(facts,D2D1::RectF(x,pane.top+90,w,pane.top+108),0,pal.ink_secondary);
            if(ready)for(size_t i=0;i<file_preview->lines.size();++i)
                text(file_preview->lines[i],D2D1::RectF(x,pane.top+120+19*static_cast<float>(i),w,pane.top+138+19*static_cast<float>(i)),0,fade(pal.ink,e));
        } else {
            const auto frame=D2D1::RectF(x,pane.top+12,w,pane.top+170);
            auto* thumb=ready?bitmap_of(file_preview->thumbnail):nullptr;
            if(thumb&&file_preview->width>0&&file_preview->height>0) {
                const float fw=frame.right-frame.left,fh=frame.bottom-frame.top;
                const float k=std::min({fw/static_cast<float>(file_preview->width),fh/static_cast<float>(file_preview->height),1.5f});
                const float tw=static_cast<float>(file_preview->width)*k,th=static_cast<float>(file_preview->height)*k;
                const float cx=(frame.left+frame.right)/2,cy=(frame.top+frame.bottom)/2,s=.96f+.04f*e;
                target->DrawBitmap(thumb,D2D1::RectF(cx-tw*s/2,cy-th*s/2,cx+tw*s/2,cy+th*s/2),e,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else result_icon(item,(frame.left+frame.right)/2,(frame.top+frame.bottom)/2,64);
            text(file.name,D2D1::RectF(x,pane.top+178,w,pane.top+202),2,pal.ink);
            text(facts,D2D1::RectF(x,pane.top+202,w,pane.top+220),0,pal.ink_secondary);
            text(L"Modified "+format_date(file.modified),D2D1::RectF(x,pane.top+220,w,pane.top+238),0,pal.ink_secondary);
        }
        text(item.detail,D2D1::RectF(x,pane.bottom-26,w,pane.bottom-8),0,pal.ink_secondary);
    }
    // Actions menu: springs up from the bottom-right corner; rows follow one by one.
    void draw_menu() {
        if(!menu_open||menu_items.empty())return;
        const auto m=menu_rect();const float t=since(menu_opened);
        const float e=t<240?bezier(.3f,1.25f,.5f,1,t/240):1,a=std::min(1.0f,t/120);
        D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
        const auto origin=menu_anchored?D2D1::Point2F(std::clamp(menu_point.x,m.left,m.right),std::clamp(menu_point.y,m.top,m.bottom)):D2D1::Point2F(m.right,m.bottom);
        target->SetTransform(D2D1::Matrix3x2F::Scale(.92f+.08f*e,.92f+.08f*e,origin)*before);
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),a),row_layer.get());
        rect(D2D1::RectF(m.left+2,m.top+6,m.right+2,m.bottom+8),fade(pal.shadow_color,.28f),12);
        rect(m,pal.solid_top,11);outline(m,pal.tile_edge,11);
        const auto* item=selected();
        text(item?L"Actions \x00B7 "+item->name:std::wstring(L"Actions"),D2D1::RectF(m.left+14,m.top+6,m.right-14,m.top+34),0,pal.ink_secondary);
        line(m.left+1,m.top+36,m.right-1,m.top+36,pal.separator);
        if(menu_glide_placed){const float y=menu_glide.value();rect(D2D1::RectF(m.left+6,y,m.right-6,y+menu_item),fade(pal.accent,.2f),7);}
        for(size_t i=0;i<menu_items.size();++i) {
            const auto row=menu_row(i);const float k=std::clamp((t-30*static_cast<float>(i))/160,0.0f,1.0f);const float ek=ease_out(k);
            if(static_cast<int>(i)!=menu_sel&&hover(row))rect(row,pal.hover,7);
            text(menu_items[i].name,D2D1::RectF(row.left+10+(1-ek)*6,row.top,row.right-96,row.bottom),1,fade(pal.ink,ek));
            if(!menu_items[i].keys.empty()) {
                const auto& keys=menu_items[i].keys;color(fade(pal.ink_secondary,ek));fonts[0]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                target->DrawText(keys.c_str(),static_cast<UINT32>(keys.size()),fonts[0].get(),D2D1::RectF(row.right-110,row.top,row.right-10,row.bottom),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                fonts[0]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
        target->PopLayer();target->SetTransform(before);
    }
    static const wchar_t* verb(const LaunchResult& item) {
        switch(item.kind) {
        case LaunchResult::Kind::Answer:case LaunchResult::Kind::Emoji:return L"Copy";
        case LaunchResult::Kind::Action:case LaunchResult::Kind::Command:return L"Run";
        case LaunchResult::Kind::Setting:return L"Settings";
        case LaunchResult::Kind::Window:return L"Switch";
        default:return L"Open";
        }
    }
    // Emoji in colour where Direct2D can (Windows 8.1+ device contexts).
    void emoji(std::wstring_view glyph,D2D1_RECT_F r,bool big) {
        auto& format=big?emoji_big:emoji_small;if(!format)return;
        ComPtr<ID2D1DeviceContext> context;
        const bool colour=SUCCEEDED(target->QueryInterface(__uuidof(ID2D1DeviceContext),reinterpret_cast<void**>(context.put())));
        color(pal.ink);
        target->DrawText(glyph.data(),static_cast<UINT32>(glyph.size()),format.get(),r,brush.get(),colour?D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT:D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    // Home view: the time, next prayer and system state at a glance.
    void draw_glance(D2D1_RECT_F pane) {
        const float x=pane.left+18,w=pane.right-18;
        SYSTEMTIME local{};GetLocalTime(&local);wchar_t clock[32]{},day[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&local,nullptr,clock,32);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,&local,L"dddd, d MMMM",day,64,nullptr);
        text(clock,D2D1::RectF(x,pane.top+8,w,pane.top+50),3,pal.ink);
        text(day,D2D1::RectF(x,pane.top+50,w,pane.top+70),0,pal.ink_secondary);
        if(!prayer_line.empty())text(prayer_line,D2D1::RectF(x,pane.top+70,w,pane.top+90),0,pal.accent);
        float y=pane.top+104;
        const auto row=[&](Icon glyph,const wchar_t* label,const std::wstring& value) {
            icon(glyph,x+8,y+11,14,pal.ink_secondary);
            text(label,D2D1::RectF(x+24,y,x+100,y+22),0,pal.ink_secondary);
            text(value,D2D1::RectF(x+100,y,w,y+22),0,pal.ink);
            y+=27;
        };
        if(net.wifi_present)row(Icon::Wifi,L"Wi-Fi",!net.wifi_on?L"Off":net.wifi_connected?(net.wifi_name.empty()?L"Connected":net.wifi_name):L"Not connected");
        if(net.bluetooth_present) {
            size_t connected=0;for(const auto& d:net.devices)connected+=d.connected;
            row(Icon::Bluetooth,L"Bluetooth",!net.bluetooth_on?std::wstring(L"Off"):connected?std::to_wstring(connected)+L" connected":std::wstring(L"On"));
        }
        if(volume_available)row(Icon::Volume,L"Volume",volume_muted?std::wstring(L"Muted"):std::to_wstring(std::lround(volume_level*100))+L"%");
        if(mic_available)row(Icon::Mic,L"Mic",mic_muted?L"Muted":L"On");
        SYSTEM_POWER_STATUS power{};
        if(GetSystemPowerStatus(&power)&&power.BatteryFlag!=128&&power.BatteryFlag!=255&&power.BatteryLifePercent<=100)
            row(Icon::Zap,L"Battery",std::to_wstring(power.BatteryLifePercent)+L"%"+(power.ACLineStatus==1?L" \x00B7 charging":L""));
        if(updates.known)row(Icon::Refresh,L"Updates",updates.restart?L"Restart to finish":updates.pending?L"Ready to install":L"Up to date");
        if(timer_state==TimerState::Running||timer_state==TimerState::Paused)row(Icon::Timer,L"Timer",clock_text(timer_remaining())+(timer_state==TimerState::Paused?L" \x00B7 paused":L""));
    }
    static const wchar_t* power_name(size_t i){static const wchar_t* names[]={L"Lock",L"Sleep",L"Restart",L"Shut down",L"Sign out"};return names[i];}
    void draw_launcher(D2D1_RECT_F r) {
        // Search field.
        icon(Icon::Search,r.left+28,r.top+28,18,pal.ink_secondary);
        const auto field=D2D1::RectF(r.left+50,r.top+12,r.right-170,r.top+44);
        if(query.empty())text(L"Search apps, actions and settings, or type a sum",field,2,pal.ink_secondary);
        else text(query,field,2,pal.ink);
        {   // Caret after the text.
            float width=0;ComPtr<IDWriteTextLayout> layout;
            if(!query.empty()&&SUCCEEDED(write->CreateTextLayout(query.c_str(),static_cast<UINT32>(query.size()),fonts[2].get(),4096,32,layout.put()))){DWRITE_TEXT_METRICS m{};layout->GetMetrics(&m);width=m.widthIncludingTrailingWhitespace;}
            const float x=std::min(field.right,field.left+width+1);
            if(((clock_now/530)%2)==0||reduced)line(x,r.top+18,x,r.top+38,pal.accent,1.5f);
        }
        const auto chip=D2D1::RectF(r.right-160,r.top+17,r.right-16,r.top+39);
        outline(chip,pal.tile_edge,6);text(L"Local only \x00B7 nothing sent",chip,0,pal.ink_secondary,true);
        line(r.left+1,r.top+56,r.right-1,r.top+56,pal.separator);
        // Results with a highlight that glides to the selection.
        const auto area=launch_list();
        target->PushAxisAlignedClip(area,D2D1_ANTIALIAS_MODE_ALIASED);
        if(const auto* row=launch_found.empty()?nullptr:row_of(static_cast<size_t>(launch_sel))) {
            const float y=glide_placed?launch_glide.value():row->top;
            rect(D2D1::RectF(area.left,area.top+y-launch_scroll,area.right,area.top+y-launch_scroll+item_height),fade(pal.accent,.18f),9);
        }
        float order=0;
        for(const auto& row:launch_rows) {
            const float top=area.top+row.top-launch_scroll,bottom=area.top+row.bottom-launch_scroll;
            if(bottom<area.top||top>area.bottom)continue;
            // Rows rise in when the launcher opens.
            const float t=since(launch_opened)-40*order++;const float e=t<220?bezier(.2f,.8f,.2f,1,std::max(0.0f,t)/220):1;
            D2D1_MATRIX_3X2_F before;target->GetTransform(&before);const bool easing=e<.999f;
            if(easing){target->SetTransform(D2D1::Matrix3x2F::Translation(0,(1-e)*8)*before);target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),e),row_layer.get());}
            if(row.header)text(row.title,D2D1::RectF(area.left+10,top+6,area.right-10,bottom),0,pal.ink_secondary);
            else {
                const auto& item=launch_found[row.result];const auto box=D2D1::RectF(area.left,top,area.right,bottom);
                if(static_cast<int>(row.result)!=launch_sel&&hover(box))rect(box,pal.hover,9);
                result_icon(item,box.left+24,(top+bottom)/2,28);
                const bool two=!item.detail.empty()&&item.kind!=LaunchResult::Kind::Answer;
                const auto name_box=D2D1::RectF(box.left+48,top+(two?4:0),box.right-70,two?top+26:bottom);
                if(item.kind==LaunchResult::Kind::Answer)text(item.name,name_box,1,pal.ink);else highlighted(item.name,query,name_box,1,pal.ink);
                if(two)text(item.detail,D2D1::RectF(box.left+48,top+24,box.right-70,bottom-4),0,pal.ink_secondary);
                text(verb(item),D2D1::RectF(box.right-68,top,box.right-10,bottom),0,pal.ink_secondary);
            }
            if(easing){target->PopLayer();target->SetTransform(before);}
        }
        if(launch_rows.empty()) {
            if(query.empty())text(launch_apps->empty()?L"Loading your apps\x2026":L"Type to search. The apps you open most will appear here.",D2D1::RectF(area.left+10,area.top+8,area.right-10,area.top+40),1,pal.ink_secondary);
            else text(L"No results",D2D1::RectF(area.left+10,area.top+8,area.right-10,area.top+40),1,pal.ink_secondary);
        }
        target->PopAxisAlignedClip();
        // Preview of the selection.
        if(r.right-r.left>=600) {
            const auto pane=launch_preview();line(pane.left,pane.top+4,pane.left,pane.bottom-4,pal.separator);
            const float x=pane.left+18,w=pane.right-18;
            if(launch_found.empty()) {
                text(L"Search is local",D2D1::RectF(x,pane.top+16,w,pane.top+40),1,pal.ink);
                text(L"No Bing, no ads, no web results.",D2D1::RectF(x,pane.top+40,w,pane.top+60),0,pal.ink_secondary);
                text(L"Try an app, \x201Cwifi off\x201D, \x201Ctimer 10\x201D,",D2D1::RectF(x,pane.top+76,w,pane.top+96),0,pal.ink_secondary);
                text(L"\x201C" L"12*7\x201D or \x201C" L"5 km in mi\x201D.",D2D1::RectF(x,pane.top+94,w,pane.top+114),0,pal.ink_secondary);
                text(L"> runs a command, : finds emoji,",D2D1::RectF(x,pane.top+122,w,pane.top+142),0,pal.ink_secondary);
                text(L"yt / g / gh / wiki / maps search the web.",D2D1::RectF(x,pane.top+140,w,pane.top+160),0,pal.ink_secondary);
            } else {
                const auto& item=launch_found[static_cast<size_t>(std::clamp(launch_sel,0,static_cast<int>(launch_found.size())-1))];
                if(query.empty()&&item.kind==LaunchResult::Kind::Action)draw_glance(pane);
                else if(const auto* file=file_of(item))draw_file_preview(*file,item,pane);
                else if(item.kind==LaunchResult::Kind::Emoji) {
                    emoji(item.value,D2D1::RectF(x,pane.top+8,w,pane.top+120),true);
                    text(item.name,D2D1::RectF(x,pane.top+128,w,pane.top+152),2,pal.ink,true);
                    text(L"Enter copies it",D2D1::RectF(x,pane.top+152,w,pane.top+172),0,pal.ink_secondary,true);
                }
                else if(item.kind==LaunchResult::Kind::Command) {
                    text(item.value.empty()?std::wstring(L"PowerShell"):std::wstring(L"Command"),D2D1::RectF(x,pane.top+12,w,pane.top+36),2,pal.ink);
                    text(L"Opens a new window in your profile folder.",D2D1::RectF(x,pane.top+36,w,pane.top+54),0,pal.ink_secondary);
                    text(L"Shift+Enter runs it as administrator.",D2D1::RectF(x,pane.top+54,w,pane.top+72),0,pal.ink_secondary);
                    if(!item.value.empty()) {
                        const auto box=D2D1::RectF(x-6,pane.top+84,w+6,pane.top+118);rect(box,pal.field,8);
                        const std::wstring line_text=L"> "+item.value;color(pal.ink);
                        target->DrawText(line_text.c_str(),static_cast<UINT32>(line_text.size()),mono.get(),D2D1::RectF(box.left+10,box.top+9,box.right-8,box.bottom),brush.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                else if(item.kind==LaunchResult::Kind::Link) {
                    result_icon(item,x+24,pane.top+36,48);
                    text(item.name,D2D1::RectF(x,pane.top+70,w,pane.top+94),1,pal.ink);
                    text(item.value,D2D1::RectF(x,pane.top+94,w,pane.top+112),0,pal.ink_secondary);
                    text(L"Opens in your default browser.",D2D1::RectF(x,pane.top+124,w,pane.top+142),0,pal.ink_secondary);
                    text(L"Nothing is sent until you press Enter.",D2D1::RectF(x,pane.top+142,w,pane.top+160),0,pal.ink_secondary);
                }
                else if(item.kind==LaunchResult::Kind::Window) {
                    result_icon(item,x+32,pane.top+48,64);
                    text(item.name,D2D1::RectF(x,pane.top+88,w,pane.top+114),2,pal.ink);
                    text(item.detail,D2D1::RectF(x,pane.top+114,w,pane.top+134),0,pal.ink_secondary);
                    text(L"Tab or right-click to snap or close",D2D1::RectF(x,pane.top+142,w,pane.top+162),0,pal.ink_secondary);
                }
                else if(item.kind==LaunchResult::Kind::Answer) {
                    text(item.name,D2D1::RectF(x,pane.top+16,w,pane.top+62),3,pal.ink);
                    text(item.detail,D2D1::RectF(x,pane.top+64,w,pane.top+84),0,pal.ink_secondary);
                    text(L"Enter copies the answer",D2D1::RectF(x,pane.top+84,w,pane.top+104),0,pal.ink_secondary);
                } else {
                    result_icon(item,x+32,pane.top+48,64);
                    text(item.name,D2D1::RectF(x,pane.top+88,w,pane.top+114),2,pal.ink);
                    std::wstring detail;
                    if(item.kind==LaunchResult::Kind::App) {
                        const auto found=launch_usage.find(item.key);
                        detail=found==launch_usage.end()?L"Not opened from here yet":L"Opened "+std::to_wstring(found->second.count)+(found->second.count==1?L" time":L" times")+L" from here";
                    } else detail=item.kind==LaunchResult::Kind::Setting?L"Opens this page in Windows Settings":L"Runs right away";
                    text(detail,D2D1::RectF(x,pane.top+114,w,pane.top+134),0,pal.ink_secondary);
                }
            }
        }
        // Footer: key hints and power.
        line(r.left+1,r.bottom-47,r.right-1,r.bottom-47,pal.separator);
        std::wstring hint=L"\x2191\x2193 Move   \x21B5 Open   Esc Close";
        if(!launch_found.empty()) {
            const auto& item=launch_found[static_cast<size_t>(std::clamp(launch_sel,0,static_cast<int>(launch_found.size())-1))];
            hint=std::wstring(L"\x21B5 ")+verb(item)+(item.kind==LaunchResult::Kind::App||item.kind==LaunchResult::Kind::Command?L"   Shift+\x21B5 Admin":L"")+L"   Tab or right-click Actions   Esc Close";
            if(menu_open)hint=L"\x2191\x2193 Move   \x21B5 Run action   Esc Back";
        }
        text(hint,D2D1::RectF(r.left+18,r.bottom-44,r.right-200,r.bottom-6),0,pal.ink_secondary);
        for(size_t i=0;i<5;++i) {
            const auto b=power_button(i);const bool over=hover(b);if(over)rect(b,pal.hover,8);
            const float cx=(b.left+b.right)/2,cy=(b.top+b.bottom)/2,t=since(power_nudge[i]);const auto ink=pal.ink;
            switch(i) { // Each icon moves a little on hover.
            case 0:{const float lift=t<420?keys(ease_in_out(t/420),{0,-2.5f,0}):0;icon_part(Icon::Lock,0,cx,cy,16,ink);icon_part(Icon::Lock,1,cx,cy,16,ink,nudge(0,lift));break;}
            case 1:{const float a=t<480?keys(ease_in_out(t/480),{0,-14,10,0}):0;icon_part(Icon::Moon,0,cx,cy,16,ink,turn(a,12,12));break;}
            case 2:{const float a=t<650?-360*bezier(.3f,1.3f,.5f,1,t/650):0;icon_part(Icon::Restart,0,cx,cy,16,ink,turn(a,12,12));break;}
            case 3:{const float d=t<420?keys(ease_in_out(t/420),{0,-2.5f,0}):0;icon_part(Icon::Power,0,cx,cy,16,ink,nudge(0,d));icon_part(Icon::Power,1,cx,cy,16,ink);break;}
            default:{const float d=t<420?keys(ease_in_out(t/420),{0,2.5f,0}):0;icon_part(Icon::Signout,0,cx,cy,16,ink);icon_part(Icon::Signout,1,cx,cy,16,ink,nudge(d,0));break;}
            }
            // Hold to confirm: a ring fills round the button.
            if(power_holding==static_cast<int>(i)) {
                const float p=std::clamp(static_cast<float>(clock_now-power_hold_start)/hold_time,0.0f,1.0f);
                ComPtr<ID2D1PathGeometry> ring;
                if(p>0&&SUCCEEDED(factory->CreatePathGeometry(ring.put()))) {
                    ComPtr<ID2D1GeometrySink> sink;ring->Open(sink.put());const float radius=14,a=p*6.2831853f-1.5707963f;
                    sink->BeginFigure(D2D1::Point2F(cx,cy-radius),D2D1_FIGURE_BEGIN_HOLLOW);
                    if(p>=.999f){sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx,cy+radius),D2D1::SizeF(radius,radius),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx,cy-radius),D2D1::SizeF(radius,radius),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));}
                    else sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx+radius*std::cos(a),cy+radius*std::sin(a)),D2D1::SizeF(radius,radius),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,p>.5f?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->Close();color(pal.accent);target->DrawGeometry(ring.get(),brush.get(),2.4f,icon_stroke.get());
                }
            }
            if(over&&power_holding<0&&power_pending<0) {
                const std::wstring tip=i==0?std::wstring(L"Lock"):std::wstring(L"Hold to ")+launcher::lower(power_name(i));
                const auto box=D2D1::RectF(b.right-130,b.top-30,b.right,b.top-6);rect(box,fade(pal.tile,1),7);text(tip,box,0,pal.ink,true);
            }
        }
        // Countdown (cancellable) and short confirmations spring in above the footer.
        const auto banner=[&](std::wstring_view message,float t,bool cancel) {
            const float e=t<260?bezier(.3f,1.3f,.5f,1,t/260):1,c=(r.left+r.right)/2;
            const auto box=D2D1::RectF(c-(cancel?130.0f:110.0f),r.bottom-96,c+(cancel?130.0f:110.0f),r.bottom-58);
            D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
            target->SetTransform(D2D1::Matrix3x2F::Scale(.9f+.1f*e,.9f+.1f*e,D2D1::Point2F(c,box.bottom))*D2D1::Matrix3x2F::Translation(0,(1-std::min(e,1.0f))*8)*before);
            rect(box,theme::rgb(0x18181c,.96f),10);outline(box,pal.tile_edge,10);
            text(message,D2D1::RectF(box.left+14,box.top,cancel?box.right-80:box.right-14,box.bottom),1,theme::rgb(0xf4f5f8),!cancel);
            if(cancel){const auto x=countdown_cancel();if(hover(x))rect(x,fade(pal.accent,.2f),7);text(L"Cancel",x,1,pal.accent,true);}
            target->SetTransform(before);
        };
        draw_menu();
        if(power_pending>=0) {
            const auto left=std::max<std::int64_t>(0,power_deadline-clock_now);const auto seconds=(left+999)/1000;
            static const wchar_t* verbs[]={L"Locking",L"Sleeping",L"Restarting",L"Shutting down",L"Signing out"};
            banner(std::wstring(verbs[power_pending])+L" in "+std::to_wstring(seconds)+L" s",static_cast<float>(countdown_time-left),true);
        } else if(!launch_toast.empty()&&since(toast_since)<1600)banner(launch_toast,since(toast_since),false);
    }
    // Hold and countdown progress; returns a power action that is now due (or -1).
    int tick_launcher(DockMachine::Time now) {
        if(!glide_placed){launch_glide.tune(500,35);launch_glide.precision(.05f);}
        if(const auto* row=launch_found.empty()?nullptr:row_of(static_cast<size_t>(launch_sel))) {
            if(!glide_placed){launch_glide.snap(row->top);glide_placed=true;}else launch_glide.target(row->top,now,reduced);
        }
        launch_glide.tick(now);
        if(menu_open&&!menu_items.empty()) {
            const float y=menu_row(static_cast<size_t>(std::clamp(menu_sel,0,static_cast<int>(menu_items.size())-1))).top;
            if(!menu_glide_placed){menu_glide.tune(520,34);menu_glide.precision(.05f);menu_glide.snap(y);menu_glide_placed=true;}else menu_glide.target(y,now,reduced);
            menu_glide.tick(now);
        }
        if(power_holding>=0&&now-power_hold_start>=hold_time){power_pending=power_holding;power_holding=-1;power_deadline=now+countdown_time;}
        if(power_pending>=0&&now>=power_deadline){const int due=power_pending;power_pending=-1;return due;}
        return -1;
    }
    // Real launcher motion only. The caret blink is a slow timer (caret_timer),
    // not a reason to draw every frame.
    bool launcher_motion_active() const {
        if(exiting_panel==Panel::Launcher)return true;
        if(panel!=Panel::Launcher)return false;
        if(launch_glide.moving()||menu_glide.moving()||power_holding>=0||power_pending>=0)return true;
        if(since(toast_since)<1700||since(launch_opened)<900||since(menu_opened)<420||since(preview_since)<200)return true;
        for(const auto start:power_nudge)if(since(start)<700)return true;
        return false;
    }
    void sync_launcher(DockMachine::Time now) {
        if(panel!=Panel::Launcher){power_hover=-1;return;}
        const auto r=panel_rect();const float c=(r.left+r.right)/2,s=panel_reveal.scale();
        const float x=c+(pointer_x-c)/s,y=r.bottom+(pointer_y-r.bottom)/s;
        int over=-1;for(size_t i=0;i<5;++i)if(inside(power_button(i),x,y))over=static_cast<int>(i);
        if(over!=power_hover){if(over>=0)power_nudge[static_cast<size_t>(over)]=now;power_hover=over;}
    }
    // ---- Quick settings motion (chosen in the Quick Settings Motion preview) ----
    // Lucide line icons as cached geometry, animated per part.
    enum class Icon {Wifi,Bluetooth,Mic,Clapper,Timer,Refresh,Chevron,Back,Plus,Volume,Gear,Search,Lock,Moon,Restart,Power,Signout,Zap,Equal,Pencil,Minus,Monitor,Plane,Tower,Contrast,Captions,Keyboard,Cast,Leaf,Shield,Share,Access,Blend,BellOff,Sun};
    ComPtr<ID2D1StrokeStyle> icon_stroke;
    std::vector<std::vector<ComPtr<ID2D1PathGeometry>>> icon_geometry;
    ComPtr<ID2D1Layer> page_layer,row_layer;
    struct TileMotion {std::int64_t start=-1;bool toggle=false;};
    // Motion state is kept per tile (not per slot), so it follows a tile that moves.
    static constexpr size_t tile_count=static_cast<size_t>(Tile::Count);
    static size_t ti(Tile t){return static_cast<size_t>(t);}
    std::array<TileMotion,tile_count> tile_motion{};
    // -1: the tile shows its first state without a fade.
    std::array<std::int64_t,tile_count> tile_lit_since=[]{std::array<std::int64_t,tile_count> a;a.fill(-1);return a;}(),arrow_nudge{};
    std::array<bool,tile_count> tile_lit{};
    bool tiles_known=false,motion_tuned=false,hover_suppressed=false;
    std::array<Spring,tile_count> tile_press{};
    // Which tiles show, in what order (quick_tiles.h). Snapshots keep the default.
    QuickLayout quick=QuickLayout::defaults();size_t quick_page=0;
    Tile slot_tile(size_t slot) const {return quick.at(quick_page,slot);}
    // Edit mode: "-" badges remove, dragging reorders; + Add and Done replace the footer.
    bool quick_edit=false;int quick_drag=-1; // Layout index of the tile being dragged.
    D2D1_POINT_2F drag_at{},drag_grab{};     // Pointer, and where on the tile it grabbed.
    float swipe_dx=0;                        // Live horizontal drag that pages (outside edit mode).
    Spring quick_slide;                      // Shown page, in pages; between pages while sliding.
    std::array<Spring,tile_count> tile_dx{},tile_dy{}; // Reflow: a tile's offset from its slot, springing to 0.
    std::int64_t back_nudge=-1,list_opened=-1,volume_wave=-1;
    Spring gear_turn,knob,page_slide;
    SettingsPage page_from=SettingsPage::Main;
    float page_dir=1;
    int panel_hover=-1;
    void build_icons() {
        const auto build=[&](const icon_paths::Part* parts,size_t count) {
            std::vector<ComPtr<ID2D1PathGeometry>> list;
            for(size_t p=0;p<count;++p) {
                ComPtr<ID2D1PathGeometry> path;check(factory->CreatePathGeometry(path.put()));
                ComPtr<ID2D1GeometrySink> sink;check(path->Open(sink.put()));bool open=false;
                for(size_t k=0;k<parts[p].count;++k) {
                    const auto& op=parts[p].ops[k];
                    if(op.op=='M'){if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->BeginFigure(D2D1::Point2F(op.v[0],op.v[1]),D2D1_FIGURE_BEGIN_HOLLOW);open=true;}
                    else if(op.op=='L')sink->AddLine(D2D1::Point2F(op.v[0],op.v[1]));
                    else if(op.op=='C')sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(op.v[0],op.v[1]),D2D1::Point2F(op.v[2],op.v[3]),D2D1::Point2F(op.v[4],op.v[5])));
                    else if(op.op=='Z'&&open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;}
                }
                if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
                check(sink->Close());list.push_back(std::move(path));
            }
            icon_geometry.push_back(std::move(list));
        };
        using namespace icon_paths; // Same order as Icon.
        build(wifi,std::size(wifi));build(bluetooth,std::size(bluetooth));build(mic,std::size(mic));build(clapper,std::size(clapper));
        build(timer,std::size(timer));build(refresh,std::size(refresh));build(chevron,std::size(chevron));build(back,std::size(back));
        build(plus,std::size(plus));build(volume,std::size(volume));build(gear,std::size(gear));
        build(search,std::size(search));build(lock,std::size(lock));build(moon,std::size(moon));build(restart,std::size(restart));
        build(power,std::size(power));build(signout,std::size(signout));build(zap,std::size(zap));build(equal,std::size(equal));
        build(pencil,std::size(pencil));build(minus,std::size(minus));build(monitor,std::size(monitor));build(plane,std::size(plane));
        build(tower,std::size(tower));build(contrast,std::size(contrast));build(captions,std::size(captions));build(keyboard,std::size(keyboard));
        build(cast,std::size(cast));build(leaf,std::size(leaf));build(shield,std::size(shield));build(share,std::size(share));
        build(access,std::size(access));build(blend,std::size(blend));build(belloff,std::size(belloff));build(sun,std::size(sun));
        const D2D1_STROKE_STYLE_PROPERTIES round{D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,10,D2D1_DASH_STYLE_SOLID,0};
        check(factory->CreateStrokeStyle(round,nullptr,0,icon_stroke.put()));
    }
    // One icon part, `size` DIP wide and centred on (cx, cy). `local` moves it
    // inside the 24-unit icon box; `drawn` < 1 strokes only its first fraction.
    void icon_part(Icon which,size_t part,float cx,float cy,float size,D2D1_COLOR_F ink,
                   D2D1_MATRIX_3X2_F local=D2D1::Matrix3x2F::Identity(),float drawn=1) {
        const auto& parts=icon_geometry[static_cast<size_t>(which)];
        if(part>=parts.size()||drawn<=.001f||ink.a<=.001f)return;
        D2D1_MATRIX_3X2_F before;target->GetTransform(&before);const float s=size/24;
        target->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&local)*D2D1::Matrix3x2F::Translation(-12,-12)*D2D1::Matrix3x2F::Scale(s,s)*D2D1::Matrix3x2F::Translation(cx,cy)*before);
        color(ink);
        if(drawn>=.999f)target->DrawGeometry(parts[part].get(),brush.get(),2,icon_stroke.get());
        else {
            float length=0;parts[part]->ComputeLength(nullptr,&length);
            const float dashes[]={length*drawn/2,length/2+1}; // In stroke widths.
            const D2D1_STROKE_STYLE_PROPERTIES style{D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,10,D2D1_DASH_STYLE_CUSTOM,0};
            ComPtr<ID2D1StrokeStyle> partial;
            if(SUCCEEDED(factory->CreateStrokeStyle(style,dashes,2,partial.put())))target->DrawGeometry(parts[part].get(),brush.get(),2,partial.get());
        }
        target->SetTransform(before);
    }
    void icon(Icon which,float cx,float cy,float size,D2D1_COLOR_F ink) {
        for(size_t p=0;p<icon_geometry[static_cast<size_t>(which)].size();++p)icon_part(which,p,cx,cy,size,ink);
    }
    float since(std::int64_t start) const{return reduced||start<0?1e9f:static_cast<float>(clock_now-start);}
    static float ease_in_out(float t){t=std::clamp(t,0.0f,1.0f);return t<.5f?4*t*t*t:1-std::pow(-2*t+2,3.0f)/2;}
    static float ease_out(float t){t=std::clamp(t,0.0f,1.0f);return 1-std::pow(1-t,3.0f);}
    // CSS cubic-bezier(x1, y1, x2, y2) at time t.
    static float bezier(float x1,float y1,float x2,float y2,float t) {
        t=std::clamp(t,0.0f,1.0f);float u=t;
        for(int i=0;i<8;++i) {
            const float x=3*(1-u)*(1-u)*u*x1+3*(1-u)*u*u*x2+u*u*u-t,dx=3*(1-u)*(1-u)*x1+6*(1-u)*u*(x2-x1)+3*u*u*(1-x2);
            if(std::abs(dx)<1e-5f)break;u=std::clamp(u-x/dx,0.0f,1.0f);
        }
        return 3*(1-u)*(1-u)*u*y1+3*(1-u)*u*u*y2+u*u*u;
    }
    // Evenly spaced keyframes at progress p (0-1).
    static float keys(float p,std::initializer_list<float> values) {
        const auto* v=values.begin();const size_t n=values.size();p=std::clamp(p,0.0f,1.0f)*static_cast<float>(n-1);
        const size_t i=std::min(n-2,static_cast<size_t>(p));return v[i]+(v[i+1]-v[i])*(p-static_cast<float>(i));
    }
    static D2D1_MATRIX_3X2_F turn(float degrees,float x,float y){return D2D1::Matrix3x2F::Rotation(degrees,D2D1::Point2F(x,y));}
    static D2D1_MATRIX_3X2_F nudge(float x,float y){return D2D1::Matrix3x2F::Translation(x,y);}
    void tune_motion() {
        if(motion_tuned)return;
        for(auto& s:tile_press){s.tune(500,26);s.snap(1);}  // Toggle press: 0.95 and back.
        gear_turn.tune(hover_stiffness,hover_damping);knob.tune(hover_stiffness,hover_damping);
        gear_turn.precision(.2f);knob.precision(.003f); // Degrees; knob travel 0..1.
        page_slide.tune(320,26);page_slide.snap(1);quick_slide.tune(320,30);quick_slide.snap(static_cast<float>(quick_page));
        for(size_t i=0;i<tile_count;++i){tile_dx[i].tune(380,30);tile_dy[i].tune(380,30);tile_dx[i].snap(0);tile_dy[i].snap(0);}
        motion_tuned=true;
    }
    // Hover edges start the icon/arrow animations; state changes start the
    // highlight fades. Called before the frame loop decides whether to run.
    void sync_panel_motion(DockMachine::Time now) {
        tune_motion();
        int over=-1;
        if(panel==Panel::Settings&&exiting_panel==Panel::None) {
            const auto r=panel_rect();const float c=(r.left+r.right)/2,s=panel_reveal.scale();
            const float x=c+(pointer_x-c)/s,y=r.bottom+(pointer_y-r.bottom)/s;
            if(settings_page==SettingsPage::Main&&quick_drag<0&&swipe_dx==0&&!quick_slide.moving()) {
                for(size_t i=0;i<tiles_per_page;++i)if(slot_tile(i)!=Tile::Count&&inside(settings_tile(i),x,y))over=static_cast<int>(split_tile(slot_tile(i))&&inside(tile_arrow(i),x,y)?20+i:10+i);
                if(inside(settings_gear(),x,y))over=31;
            } else if(inside(page_back(),x,y))over=30;
        }
        if(over!=panel_hover) {
            const int was=panel_hover>=10&&panel_hover<26?panel_hover%10:-1,is=over>=10&&over<26?over%10:-1;
            if(is>=0&&is!=was)tile_motion[ti(slot_tile(static_cast<size_t>(is)))]={now,false};
            if(over>=20&&over<26)arrow_nudge[ti(slot_tile(static_cast<size_t>(over-20)))]=now;
            if(over==30)back_nudge=now;
            panel_hover=over;
        }
        gear_turn.target(over==31?90.0f:0.0f,now,reduced);
        for(size_t i=0;i<tile_count;++i) {
            const auto v=tile_view(static_cast<Tile>(i));const bool lit=v.on&&v.enabled;
            if(!tiles_known){tile_lit[i]=lit;tile_lit_since[i]=-1;}else if(lit!=tile_lit[i]){tile_lit[i]=lit;tile_lit_since[i]=now;}
        }
        tiles_known=true;
        if(settings_page==SettingsPage::Wifi||settings_page==SettingsPage::Bluetooth)
            knob.target((settings_page==SettingsPage::Wifi?net.wifi_on:net.bluetooth_on)?1.0f:0.0f,now,reduced);
    }
    bool panel_motion_active() const {
        if(panel!=Panel::Settings&&exiting_panel!=Panel::Settings)return false;
        for(const auto& m:tile_motion)if(since(m.start)<1000)return true;
        for(const auto start:tile_lit_since)if(since(start)<200)return true;
        for(const auto start:arrow_nudge)if(since(start)<400)return true;
        if(since(back_nudge)<400||since(list_opened)<900||since(volume_wave)<520)return true;
        for(const auto& s:tile_press)if(s.moving())return true;
        for(size_t i=0;i<tile_count;++i)if(tile_dx[i].moving()||tile_dy[i].moving())return true;
        return gear_turn.moving()||knob.moving()||page_slide.moving()||quick_slide.moving();
    }
    // Pages slide: the new page comes in from the side, the old one drifts out.
    void start_page(SettingsPage page,DockMachine::Time now) {
        tune_motion();if(page==settings_page)return;
        page_from=settings_page;page_dir=page==SettingsPage::Main?-1.0f:1.0f;settings_page=page;
        page_slide.snap(0);page_slide.target(1,now,reduced);
        if(page==SettingsPage::Wifi||page==SettingsPage::Bluetooth) {
            list_opened=now;knob.snap((page==SettingsPage::Wifi?net.wifi_on:net.bluetooth_on)?1.0f:0.0f);
        }
    }
    D2D1_RECT_F settings_gear() const{const auto r=panel_rect();return D2D1::RectF(r.right-45,r.bottom-36,r.right-16,r.bottom-5);}
    // ---- Quick settings pages and edit mode ----
    D2D1_RECT_F quick_area() const {const auto r=panel_rect();return D2D1::RectF(r.left+1,r.top+46,r.right-1,r.top+214);}
    D2D1_RECT_F quick_dot(size_t p) const {
        const auto r=panel_rect();const float c=(r.left+r.right)/2+(static_cast<float>(p)-(static_cast<float>(quick.pages())-1)/2)*16;
        return D2D1::RectF(c-7,r.top+214,c+7,r.top+228);
    }
    D2D1_RECT_F quick_edit_button() const {const auto r=panel_rect();return D2D1::RectF(r.right-78,r.bottom-36,r.right-49,r.bottom-5);}
    D2D1_RECT_F quick_add_button() const {const auto r=panel_rect();return D2D1::RectF(r.left+16,r.bottom-35,r.left+110,r.bottom-7);}
    D2D1_RECT_F quick_done_button() const {const auto r=panel_rect();return D2D1::RectF(r.right-96,r.bottom-35,r.right-16,r.bottom-7);}
    D2D1_RECT_F quick_empty_button() const {const auto r=panel_rect();const float c=(r.left+r.right)/2;return D2D1::RectF(c-60,r.top+112,c+60,r.top+146);}
    D2D1_RECT_F tile_remove(size_t i) const {const auto t=settings_tile(i);return D2D1::RectF(t.left-7,t.top-7,t.left+11,t.top+11);}
    float page_width() const {const auto r=panel_rect();return r.right-r.left;}
    // Where layout index k sits, pages laid side by side (page 0 at x 0).
    D2D1_POINT_2F quick_home(size_t k) const {const auto t=settings_tile(k%tiles_per_page);return D2D1::Point2F(t.left+static_cast<float>(k/tiles_per_page)*page_width(),t.top);}
    // Slot on the shown page nearest to a panel point (tiles and labels count).
    size_t quick_slot_at(float x,float y) const {
        const auto r=panel_rect();
        const auto col=std::clamp(static_cast<int>((x-(r.left+15))/109),0,2),row=std::clamp(static_cast<int>((y-(r.top+51))/85),0,1);
        return static_cast<size_t>(row*3+col);
    }
    // Animate tiles from where they were in `before` to their new slots.
    void reflow(const QuickLayout& before,DockMachine::Time now) {
        tune_motion();
        for(size_t k=0;k<quick.tiles.size();++k) {
            const auto it=std::find(before.tiles.begin(),before.tiles.end(),quick.tiles[k]);
            if(it==before.tiles.end()||static_cast<int>(k)==quick_drag)continue;
            const auto was=quick_home(static_cast<size_t>(it-before.tiles.begin())),is=quick_home(k);
            if(was.x==is.x&&was.y==is.y)continue;
            auto& dx=tile_dx[ti(quick.tiles[k])];auto& dy=tile_dy[ti(quick.tiles[k])];
            dx.snap(dx.value()+was.x-is.x);dy.snap(dy.value()+was.y-is.y);dx.target(0,now,reduced);dy.target(0,now,reduced);
        }
    }
    // Add tiles page: a two-column grid of the tiles not shown.
    D2D1_RECT_F add_cell(size_t i) const {
        const auto a=page_list();const float w=(a.right-a.left-16)/2,x=a.left+4+static_cast<float>(i%2)*(w+8),y=a.top+4+static_cast<float>(i/2)*42-list_scroll;
        return D2D1::RectF(x,y,x+w,y+36);
    }
    // Tiles offered in Add: those not shown, minus ones this PC can never use.
    std::vector<Tile> addable() const {
        auto list=quick.hidden();
        std::erase_if(list,[&](Tile t){return (t==Tile::Rotation&&!rotation_sensor)||(t==Tile::Keyboard&&keyboard_count<2);});
        return list;
    }
    float add_extent() const {return static_cast<float>((addable().size()+1)/2)*42+8;}
    TimerState timer_state=TimerState::Idle;
    std::int64_t timer_duration=5*60000,timer_left=0;
    ULONGLONG timer_end=0;
    static constexpr int timer_presets[]={1,5,10,15,25,30,45,60};
    std::int64_t timer_remaining() const {
        if(timer_state==TimerState::Running){const auto now=GetTickCount64();return timer_end>now?static_cast<std::int64_t>(timer_end-now):0;}
        return timer_state==TimerState::Idle?timer_duration:timer_left;
    }
    static std::wstring clock_text(std::int64_t ms) {
        const long long s=(std::max<std::int64_t>(0,ms)+999)/1000;wchar_t value[32]{};
        if(s>=3600)swprintf(value,32,L"%lld:%02lld:%02lld",s/3600,(s/60)%60,s%60);else swprintf(value,32,L"%02lld:%02lld",s/60,s%60);
        return value;
    }
    static std::wstring minutes_text(std::int64_t ms){const auto m=ms/60000;return m>=60&&m%60==0?std::to_wstring(m/60)+L" hr":std::to_wstring(m)+L" min";}
    static bool split_tile(Tile t){return spec(t).split;}
    D2D1_RECT_F settings_tile(size_t i) const {const auto r=panel_rect();const float x=r.left+20+static_cast<float>(i%3)*109,y=r.top+56+static_cast<float>(i/3)*85;return D2D1::RectF(x,y,x+100,y+48);}
    D2D1_RECT_F tile_arrow(size_t i) const {const auto t=settings_tile(i);return D2D1::RectF(t.left+67,t.top,t.right,t.bottom);}
    D2D1_RECT_F page_back() const {const auto r=panel_rect();return D2D1::RectF(r.left+12,r.top+10,r.left+44,r.top+42);}
    D2D1_RECT_F page_switch() const {const auto r=panel_rect();return D2D1::RectF(r.right-62,r.top+16,r.right-20,r.top+36);}
    D2D1_RECT_F page_list() const {const auto r=panel_rect();return D2D1::RectF(r.left+8,r.top+50,r.right-8,r.bottom-46);}
    D2D1_RECT_F page_footer_link() const {const auto r=panel_rect();return D2D1::RectF(r.left+10,r.bottom-38,r.left+214,r.bottom-6);}
    D2D1_RECT_F page_footer_action() const {const auto r=panel_rect();return D2D1::RectF(r.right-46,r.bottom-38,r.right-14,r.bottom-6);}
    D2D1_RECT_F page_notice_button() const {const auto r=panel_rect();return D2D1::RectF(r.left+24,r.top+128,r.left+194,r.top+160);}
    struct ListRow {D2D1_RECT_F box;size_t index;bool open;};
    static bool row_needs_password(const WifiNetwork& n){return !n.connected&&!n.saved&&n.secured&&n.password_supported&&n.auth!=10;}
    float row_height(size_t i) const {
        if(settings_page==SettingsPage::Wifi){const auto& n=net.networks[i];return n.ssid!=expanded?52.0f:row_needs_password(n)?136.0f:96.0f;}
        return net.devices[i].key==expanded?96.0f:52.0f;
    }
    size_t list_count() const{return settings_page==SettingsPage::Wifi?net.networks.size():settings_page==SettingsPage::Bluetooth?net.devices.size():0;}
    float list_extent() const{float h=0;for(size_t i=0;i<list_count();++i)h+=row_height(i);return h;}
    std::vector<ListRow> list_rows() const {
        std::vector<ListRow> rows;const auto area=page_list();float y=area.top-list_scroll;
        for(size_t i=0;i<list_count();++i){const float h=row_height(i);rows.push_back({D2D1::RectF(area.left,y,area.right,y+h),i,h>52});y+=h;}
        return rows;
    }
    // Inside an expanded row: its action button, the security-key field and its eye.
    static D2D1_RECT_F row_button(D2D1_RECT_F row){return D2D1::RectF(row.right-128,row.bottom-42,row.right-12,row.bottom-10);}
    static D2D1_RECT_F row_field(D2D1_RECT_F row){return D2D1::RectF(row.left+48,row.top+52,row.right-12,row.top+86);}
    static D2D1_RECT_F row_eye(D2D1_RECT_F row){const auto f=row_field(row);return D2D1::RectF(f.right-34,f.top,f.right,f.bottom);}
    D2D1_RECT_F timer_chip(size_t i) const {const auto r=panel_rect();const float x=r.left+20+static_cast<float>(i%4)*81,y=r.top+150+static_cast<float>(i/4)*42;return D2D1::RectF(x,y,x+75,y+34);}
    D2D1_RECT_F timer_step(bool up) const {const auto r=panel_rect();const float c=(r.left+r.right)/2;return up?D2D1::RectF(c+96,r.top+76,c+128,r.top+108):D2D1::RectF(c-128,r.top+76,c-96,r.top+108);}
    D2D1_RECT_F timer_button(size_t i,size_t count) const {
        const auto r=panel_rect();const float w=(r.right-r.left-40-8*static_cast<float>(count-1))/static_cast<float>(count),x=r.left+20+static_cast<float>(i)*(w+8);
        return D2D1::RectF(x,r.top+246,x+w,r.top+282);
    }
    std::vector<const wchar_t*> timer_buttons() const {
        switch(timer_state){case TimerState::Idle:return {L"Start"};case TimerState::Running:return {L"Pause",L"Reset"};case TimerState::Paused:return {L"Resume",L"Reset"};default:return {L"Dismiss",L"+5 min"};}
    }
    struct TileView {const wchar_t* glyph;std::wstring label;bool on,enabled,badge;};
    TileView tile_view(Tile t) const {
        switch(t) {
        case Tile::Wifi:{
            std::wstring label=!net.wifi_on?L"Wi-Fi":!net.connecting.empty()?L"Connecting\x2026":!net.wifi_name.empty()?net.wifi_name:net.wifi_connected?L"Connected":L"Not connected";
            return {L"\xE701",label,net.wifi_on,net.wifi_present,false};
        }
        case Tile::Bluetooth:{
            std::wstring label=L"Bluetooth";
            if(net.bluetooth_on){label=L"Not connected";for(const auto& d:net.devices)if(d.connected){label=d.name;break;}}
            return {L"\xE702",label,net.bluetooth_on,net.bluetooth_present,false};
        }
        case Tile::Mic:return {mic_muted?L"\xEC54":L"\xE720",!mic_available?L"No microphone":mic_muted?L"Mic muted":L"Mic on",mic_muted,mic_available,false};
        case Tile::Capture:{const bool sent=capture_sent&&GetTickCount64()-capture_sent<2500;return {L"\xE714",sent?L"Saving clip":overlay_running?L"Instant capture":L"NVIDIA overlay off",sent,true,false};}
        case Tile::Timer:{
            const auto left=timer_remaining();
            std::wstring label=timer_state==TimerState::Idle?L"Timer":timer_state==TimerState::Ringing?L"Time's up":timer_state==TimerState::Paused?L"Paused "+clock_text(left):clock_text(left);
            return {L"\xE916",label,timer_state==TimerState::Running||timer_state==TimerState::Ringing,true,false};
        }
        case Tile::Updates:{
            std::wstring label=!updates.known?L"Windows Update":updates.restart?L"Restart to update":updates.pending>0?std::to_wstring(updates.pending)+(updates.pending==1?L" update":L" updates"):L"Up to date";
            return {L"\xE895",label,false,true,updates.restart||updates.pending>0};
        }
        case Tile::Project:{
            const wchar_t* mode=display.displays<2?L"Project":display.mode==DisplayMode::Clone?L"Duplicate":display.mode==DisplayMode::Extend?L"Extend":display.mode==DisplayMode::External?L"Second screen":L"PC screen only";
            return {L"",mode,display.displays>1&&display.mode!=DisplayMode::Internal,true,false};
        }
        case Tile::Airplane:return {L"",L"Airplane mode",airplane_on(),net.wifi_present||net.bluetooth_present,false};
        case Tile::DarkMode:return {L"",L"Dark mode",dark,true,false};
        case Tile::Captions:return {L"",L"Live captions",captions_on,true,false};
        case Tile::Keyboard:return {L"",keyboard_label.empty()?L"Keyboard":keyboard_label,false,keyboard_count>1,false};
        case Tile::EnergySaver:return {L"",L"Energy saver",energy_on,true,false};
        case Tile::Vpn:return {L"",vpn_on?L"VPN connected":L"VPN",vpn_on,true,false};
        case Tile::Rotation:return {L"",L"Rotation lock",false,rotation_sensor,false};
        default:return {L"",std::wstring(spec(t).name),false,true,false}; // Settings launchers.
        }
    }
    void tile_icon(Tile tile,float cx,float cy,D2D1_COLOR_F ink) {
        constexpr float size=18;const auto& m=tile_motion[ti(tile)];const float t=since(m.start);
        switch(tile) {
        case Tile::Wifi: // Wi-Fi: the bars fade out and back in, one after another.
            for(size_t b=0;b<4;++b) {
                float o=1;
                if(net.wifi_on){const float p=(t-90*static_cast<float>(b))/420;if(p>0&&p<1)o=keys(ease_in_out(p),{1,0,1});}
                icon_part(Icon::Wifi,b,cx,cy,size,fade(ink,o));
            }
            break;
        case Tile::Bluetooth: { // Bluetooth: wiggles on hover, draws itself in when turned on.
            const float angle=!m.toggle&&t<420?keys(ease_in_out(t/420),{0,-10,8,0}):0;
            const float drawn=m.toggle&&net.bluetooth_on&&t<420?ease_out(t/420):1;
            icon_part(Icon::Bluetooth,0,cx,cy,size,ink,turn(angle,12,12),drawn);
            break;
        }
        case Tile::Mic: { // Mic: hops on hover; the mute slash draws across (and back off).
            const float hop=!m.toggle&&!mic_muted&&t<360?keys(ease_in_out(t/360),{0,-2,0}):0;
            for(size_t p=0;p<3;++p)icon_part(Icon::Mic,p,cx,cy,size,ink,nudge(0,hop));
            float slash=mic_muted?1.0f:0.0f;
            if(m.toggle&&t<260){const float e=ease_out(t/260);slash=mic_muted?e:1-e;}
            icon_part(Icon::Mic,3,cx,cy,size,ink,D2D1::Matrix3x2F::Identity(),slash);
            break;
        }
        case Tile::Capture: { // Instant capture: the clapper snaps shut.
            const float d=m.toggle?300.0f:420.0f,a=t<d?keys(ease_in_out(t/d),{0,-14,0}):0;
            icon_part(Icon::Clapper,0,cx,cy,size,ink,turn(a,3,11));icon_part(Icon::Clapper,1,cx,cy,size,ink);
            break;
        }
        case Tile::Timer: { // Timer: the hand sweeps once round.
            const float d=m.toggle?700.0f:600.0f,a=t<d?360*ease_in_out(t/d):0;
            icon_part(Icon::Timer,0,cx,cy,size,ink);icon_part(Icon::Timer,1,cx,cy,size,ink,turn(a,12,14));icon_part(Icon::Timer,2,cx,cy,size,ink);
            break;
        }
        case Tile::Updates: { // Windows Update: tilts on hover, spins once when clicked.
            float a=0;
            if(m.toggle&&t<900)a=360*bezier(.3f,1.4f,.5f,1,t/900);
            else if(!m.toggle&&t<650)a=keys(ease_in_out(t/650),{0,45,0});
            icon_part(Icon::Refresh,0,cx,cy,size,ink,turn(a,12,12));
            break;
        }
        // The newer tiles share a few small motions: a wiggle, a hop, or a part drawing in.
        case Tile::Project: { // The stand stays, the screen hops.
            const float hop=t<360?keys(ease_in_out(t/360),{0,-2,0}):0;
            icon_part(Icon::Monitor,0,cx,cy,size,ink,nudge(0,hop));icon_part(Icon::Monitor,1,cx,cy,size,ink);
            break;
        }
        case Tile::Airplane: { // Takes off: a short climb up and to the right.
            const float p=t<480?keys(ease_in_out(t/480),{0,1,0}):0;
            icon_part(Icon::Plane,0,cx,cy,size,ink,nudge(2.5f*p,-2.5f*p));
            break;
        }
        case Tile::Hotspot: // The waves pulse out from the mast.
            icon_part(Icon::Tower,0,cx,cy,size,ink);
            for(size_t w=1;w<3;++w){const float p=(t-120*static_cast<float>(w-1))/400;icon_part(Icon::Tower,w,cx,cy,size,fade(ink,p>0&&p<1?keys(ease_in_out(p),{1,0,1}):1));}
            break;
        case Tile::DarkMode: { // The half turns over.
            const float a=t<600?180*ease_in_out(t/600):0;
            icon_part(Icon::Contrast,0,cx,cy,size,ink);icon_part(Icon::Contrast,1,cx,cy,size,ink,turn(a,12,12));
            break;
        }
        case Tile::Captions: // The caption lines write themselves in.
            icon_part(Icon::Captions,0,cx,cy,size,ink);icon_part(Icon::Captions,1,cx,cy,size,ink,D2D1::Matrix3x2F::Identity(),t<420?ease_out(t/420):1);
            break;
        case Tile::Keyboard: { // The keys press down.
            const float press=t<300?keys(ease_in_out(t/300),{0,1.5f,0}):0;
            icon_part(Icon::Keyboard,0,cx,cy,size,ink);icon_part(Icon::Keyboard,1,cx,cy,size,ink,nudge(0,press));
            break;
        }
        case Tile::Cast: // The waves draw in from the corner.
            icon_part(Icon::Cast,0,cx,cy,size,ink);icon_part(Icon::Cast,1,cx,cy,size,ink,D2D1::Matrix3x2F::Identity(),t<420?ease_out(t/420):1);
            break;
        case Tile::DoNotDisturb: { // The bell rings, the slash draws across.
            const float a=t<480?keys(ease_in_out(t/480),{0,-12,10,-6,0}):0;
            icon_part(Icon::BellOff,0,cx,cy,size,ink,turn(a,12,4));icon_part(Icon::BellOff,1,cx,cy,size,ink,D2D1::Matrix3x2F::Identity(),t<300?ease_out(t/300):1);
            break;
        }
        case Tile::Nearby: { // The links draw between the dots.
            icon_part(Icon::Share,0,cx,cy,size,ink);icon_part(Icon::Share,1,cx,cy,size,ink,D2D1::Matrix3x2F::Identity(),t<420?ease_out(t/420):1);
            break;
        }
        default: { // Single-part icons wiggle.
            const Icon which=tile==Tile::EnergySaver?Icon::Leaf:tile==Tile::Vpn?Icon::Shield:tile==Tile::NightLight?Icon::Moon:
                tile==Tile::Accessibility?Icon::Access:tile==Tile::ColorFilters?Icon::Blend:Icon::Lock;
            const float a=t<420?keys(ease_in_out(t/420),{0,-10,8,0}):0;
            for(size_t p=0;p<icon_geometry[static_cast<size_t>(which)].size();++p)icon_part(which,p,cx,cy,size,ink,turn(a,12,12));
            break;
        }
        }
    }
    // The knob slides with the dock's spring.
    void settings_switch(D2D1_RECT_F s,bool on) {
        const float cy=(s.top+s.bottom)/2,x=std::clamp(knob.value(),-.2f,1.2f);
        const float cx=s.left+10+x*(s.right-s.left-20),radius=5+std::clamp(x,0.0f,1.0f);
        if(on){rect(s,pal.accent,10);color(pal.on_accent);}else{outline(s,pal.ink_secondary,10);color(pal.ink_secondary);}
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),radius,radius),brush.get());
    }
    void settings_button(D2D1_RECT_F b,const wchar_t* label,bool primary) {
        const bool over=hover(b);
        rect(b,primary?pal.accent:over?pal.hover_strong:pal.tile,6);
        if(primary&&over)rect(b,theme::rgb(0xffffff,.10f),6);else if(!primary)outline(b,pal.tile_edge,6);
        text(label,b,1,primary?pal.on_accent:pal.ink,true);
    }
    // Pages sit side by side and slide; with one page this draws exactly as before.
    void draw_settings_tiles(D2D1_RECT_F r) {
        text(quick_edit?L"Edit quick settings":L"Quick settings",D2D1::RectF(r.left+20,r.top+12,r.right-20,r.top+40),2,pal.ink);
        if(quick.tiles.empty()){settings_button(quick_empty_button(),L"+ Add tiles",false);return;}
        const size_t pages=quick.pages();const float width=page_width(),shown=quick_slide.value()-swipe_dx/width;
        const bool clip=pages>1||swipe_dx!=0||quick_slide.moving()||quick_drag>=0;
        if(clip)target->PushAxisAlignedClip(quick_area(),D2D1_ANTIALIAS_MODE_ALIASED);
        const bool suppressed=hover_suppressed;
        for(size_t p=0;p<pages;++p) {
            const float dx=(static_cast<float>(p)-shown)*width;if(std::abs(dx)>=width)continue;
            hover_suppressed=suppressed||std::abs(dx)>.5f||quick_drag>=0;
            for(size_t i=0;i<tiles_per_page;++i) {
                const size_t k=p*tiles_per_page+i;const auto id=quick.at(p,i);if(id==Tile::Count||static_cast<int>(k)==quick_drag)continue;
                const auto home=settings_tile(i);const float ox=dx+tile_dx[ti(id)].value(),oy=tile_dy[ti(id)].value();
                draw_tile(id,D2D1::RectF(home.left+ox,home.top+oy,home.right+ox,home.bottom+oy));
            }
        }
        hover_suppressed=suppressed;
        if(clip)target->PopAxisAlignedClip();
        // The dragged tile floats above the others, slightly lifted.
        if(quick_drag>=0&&static_cast<size_t>(quick_drag)<quick.tiles.size()) {
            const float x=drag_at.x-drag_grab.x,y=drag_at.y-drag_grab.y;const auto tile=D2D1::RectF(x,y,x+100,y+48);
            D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
            target->SetTransform(D2D1::Matrix3x2F::Scale(1.05f,1.05f,D2D1::Point2F(x+50,y+24))*before);
            rect(D2D1::RectF(tile.left+1,tile.top+3,tile.right+1,tile.bottom+4),theme::rgb(0x000000,.18f),9);
            draw_tile(quick.tiles[static_cast<size_t>(quick_drag)],tile);
            target->SetTransform(before);
        }
        if(pages>1)for(size_t p=0;p<pages;++p) {
            const auto d=quick_dot(p);const bool current=p==quick_page;if(hover(d))rect(d,pal.hover,7);
            color(current?pal.ink:pal.ink_muted);const float radius=current?3.5f:3;
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F((d.left+d.right)/2,(d.top+d.bottom)/2),radius,radius),brush.get());
        }
    }
    void draw_tile(Tile id,D2D1_RECT_F tile) {
        {
            const auto v=tile_view(id);const bool split=split_tile(id),lit=v.on&&v.enabled,over=hover(tile);
            const auto centre=D2D1::Point2F((tile.left+tile.right)/2,(tile.top+tile.bottom)/2);
            // Press: the tile squeezes to 0.95 and springs back.
            D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
            const float squeeze=tile_press[ti(id)].value();
            target->SetTransform(D2D1::Matrix3x2F::Scale(squeeze,squeeze,centre)*before);
            // On: the accent fades and grows in; off: it fades out.
            float k=lit?1.0f:0.0f,grow=1;
            if(const float t=since(tile_lit_since[ti(id)]);t<180){const float e=bezier(.2f,.8f,.2f,1,t/180);k=lit?e:1-e;if(lit)grow=.9f+.1f*e;}
            rect(tile,over?pal.hover_strong:pal.tile,8);
            if(k<1)outline(tile,fade(pal.tile_edge,1-k),8);
            if(k>0) {
                rect(D2D1::RectF(centre.x-(centre.x-tile.left)*grow,centre.y-(centre.y-tile.top)*grow,centre.x+(tile.right-centre.x)*grow,centre.y+(tile.bottom-centre.y)*grow),fade(pal.accent,k),8);
                if(over)rect(tile,theme::rgb(0xffffff,.10f*k),8);
            }
            const auto ink=!v.enabled?pal.ink_muted:k>.5f?pal.on_accent:pal.ink;
            tile_icon(id,split?tile.left+33.5f:centre.x,centre.y,ink);
            if(split) {
                line(tile.left+67,tile.top+10,tile.left+67,tile.bottom-10,k>.5f?theme::rgb(0x000000,.20f):pal.separator);
                const float t=since(arrow_nudge[ti(id)]),dx=t<380?keys(ease_in_out(t/380),{0,3,0}):0;
                icon(Icon::Chevron,tile.left+83.5f+dx,centre.y,16,ink);
            }
            if(v.badge){color(pal.accent);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tile.right-9,tile.top+9),4,4),brush.get());}
            target->SetTransform(before);
            text(v.label,D2D1::RectF(tile.left-4,tile.top+51,tile.right+4,tile.top+72),0,pal.ink,true);
            if(quick_edit) { // Remove badge.
                const auto b=D2D1::RectF(tile.left-7,tile.top-7,tile.left+11,tile.top+11);const auto c=D2D1::Point2F(tile.left+2,tile.top+2);
                color(pal.solid_top);target->FillEllipse(D2D1::Ellipse(c,9,9),brush.get()); // Opaque: it overlaps the tile edge.
                if(hover(b)){color(pal.hover_strong);target->FillEllipse(D2D1::Ellipse(c,9,9),brush.get());}
                color(pal.separator);target->DrawEllipse(D2D1::Ellipse(c,9,9),brush.get(),1);
                icon(Icon::Minus,c.x,c.y,12,pal.ink);
            }
        }
    }
    void page_header(D2D1_RECT_F r,const wchar_t* title,int toggle) { // toggle: -1 none, 0 off, 1 on
        const auto back=page_back();if(hover(back))rect(back,pal.hover,8);
        {const float t=since(back_nudge),dx=t<380?keys(ease_in_out(t/380),{0,-3,0}):0;icon(Icon::Back,(back.left+back.right)/2+dx,(back.top+back.bottom)/2,18,pal.ink);}
        text(title,D2D1::RectF(r.left+52,r.top+10,r.right-80,r.top+42),2,pal.ink);
        if(toggle>=0){const auto s=page_switch();hover(s);settings_switch(s,toggle==1);}
    }
    void page_footer(D2D1_RECT_F r,const wchar_t* link,Icon glyph) {
        line(r.left+1,r.bottom-44,r.right-1,r.bottom-44,pal.separator);
        const auto a=page_footer_link();if(hover(a))rect(a,pal.hover,6);text(link,D2D1::RectF(a.left+10,a.top,a.right,a.bottom),1,pal.ink);
        const auto b=page_footer_action();if(hover(b))rect(b,pal.hover,6);icon(glyph,(b.left+b.right)/2,(b.top+b.bottom)/2,18,pal.ink);
    }
    void page_notice(D2D1_RECT_F r,std::wstring_view first,std::wstring_view second,const wchar_t* button) {
        text(first,D2D1::RectF(r.left+24,r.top+70,r.right-24,r.top+94),1,pal.ink);
        if(!second.empty())text(second,D2D1::RectF(r.left+24,r.top+94,r.right-24,r.top+114),0,pal.ink_secondary);
        if(button)settings_button(page_notice_button(),button,false);
    }
    void list_row_background(const ListRow& row,bool connected) {
        const auto inner=D2D1::RectF(row.box.left,row.box.top+2,row.box.right,row.box.bottom-2);
        if(row.open)rect(inner,pal.tile,6);else if(hover(inner))rect(inner,pal.hover,6);
        if(connected)rect(D2D1::RectF(row.box.left+1,row.box.top+15,row.box.left+4,row.box.top+37),pal.accent,1.5f);
    }
    void draw_network(const ListRow& row) {
        const auto& n=net.networks[row.index];const auto b=row.box;
        list_row_background(row,n.connected);
        // Signal: lit bars over dim ones.
        {
            const int bars=n.signal>=75?3:n.signal>=50?2:n.signal>=25?1:0;const float cx=b.left+24,cy=b.top+26;
            icon_part(Icon::Wifi,0,cx,cy,18,pal.ink);
            for(int k=1;k<=3;++k)icon_part(Icon::Wifi,static_cast<size_t>(k),cx,cy,18,k<=bars?pal.ink:pal.ink_muted);
        }
        text(n.ssid,D2D1::RectF(b.left+48,b.top+6,b.right-12,b.top+28),1,pal.ink);
        std::wstring status=net.connecting==n.ssid?L"Connecting\x2026":n.connected?(n.secured?L"Connected, secured":L"Connected"):n.secured?(n.saved?L"Secured, saved":L"Secured"):L"Open";
        if(row.open&&!net.wifi_message.empty()&&net.connecting.empty()&&!n.connected)status=net.wifi_message;
        text(status,D2D1::RectF(b.left+48,b.top+27,b.right-12,b.top+46),0,pal.ink_secondary);
        if(!row.open)return;
        if(row_needs_password(n)) {
            const auto f=row_field(b);rect(f,pal.field,6);outline(f,pal.accent,6);
            const std::wstring shown=password.empty()?L"Enter the network security key":password_visible?password:std::wstring(password.size(),L'\x2022');
            text(shown,D2D1::RectF(f.left+10,f.top,f.right-38,f.bottom),1,password.empty()?pal.ink_secondary:pal.ink);
            const auto eye=row_eye(b);if(hover(eye))rect(eye,pal.hover,6);text(password_visible?L"\xED1A":L"\xE7B3",eye,5,pal.ink_secondary,true);
        }
        settings_button(row_button(b),n.connected?L"Disconnect":!n.password_supported?L"Open settings":L"Connect",!n.connected&&n.password_supported&&net.connecting.empty());
    }
    void draw_device(const ListRow& row) {
        const auto& d=net.devices[row.index];const auto b=row.box;using Kind=BluetoothDevice::Kind;
        list_row_background(row,d.connected);
        const wchar_t* glyph=d.kind==Kind::Audio?L"\xE7F6":d.kind==Kind::Keyboard?L"\xE765":d.kind==Kind::Mouse?L"\xE962":d.kind==Kind::Phone?L"\xE8EA":d.kind==Kind::Computer?L"\xE7F8":L"\xE702";
        text(glyph,D2D1::RectF(b.left+8,b.top+4,b.left+40,b.top+48),5,pal.ink,true);
        text(d.name,D2D1::RectF(b.left+48,b.top+6,b.right-12,b.top+28),1,pal.ink);
        text(net.bluetooth_busy==d.key?L"Working\x2026":d.connected?L"Connected":L"Paired",D2D1::RectF(b.left+48,b.top+27,b.right-12,b.top+46),0,pal.ink_secondary);
        if(!row.open)return;
        if(!d.switchable)text(L"Windows connects it on its own.",D2D1::RectF(b.left+48,b.top+52,b.right-140,b.top+86),0,pal.ink_secondary);
        settings_button(row_button(b),d.switchable?(d.connected?L"Disconnect":L"Connect"):L"Open settings",d.switchable&&!d.connected&&net.bluetooth_busy.empty());
    }
    void draw_timer_page(D2D1_RECT_F r) {
        page_header(r,L"Timer",-1);
        const auto left=timer_remaining();const bool ringing=timer_state==TimerState::Ringing;
        text(ringing?L"Time's up":clock_text(left),D2D1::RectF(r.left+56,r.top+60,r.right-56,r.top+124),4,ringing?pal.accent:pal.ink,true);
        if(timer_state==TimerState::Idle) {
            for(const bool up:{false,true}){const auto s=timer_step(up);if(hover(s))rect(s,pal.hover,8);text(up?L"\xE710":L"\xE738",s,5,pal.ink,true);}
        } else if(!ringing) {
            const float a=r.left+40,b=r.right-40,y=r.top+132;
            const float done=timer_duration>0?1-static_cast<float>(left)/static_cast<float>(timer_duration):0;
            rect(D2D1::RectF(a,y-2,b,y+2),pal.track,2);rect(D2D1::RectF(a,y-2,a+(b-a)*std::clamp(done,0.0f,1.0f),y+2),pal.accent,2);
        }
        for(size_t i=0;i<std::size(timer_presets);++i) {
            const auto c=timer_chip(i);const bool chosen=timer_state==TimerState::Idle&&timer_duration==timer_presets[i]*60000LL;const bool over=hover(c);
            rect(c,chosen?pal.accent:over?pal.hover_strong:pal.tile,6);if(!chosen)outline(c,pal.tile_edge,6);
            text(minutes_text(timer_presets[i]*60000LL),c,1,chosen?pal.on_accent:pal.ink,true);
        }
        const auto labels=timer_buttons();
        for(size_t i=0;i<labels.size();++i)settings_button(timer_button(i,labels.size()),labels[i],i==0);
        text(L"An alarm sounds when time is up.",D2D1::RectF(r.left+20,r.bottom-36,r.right-20,r.bottom-10),0,pal.ink_secondary,true);
    }
    // A horizontal slider on `track` (a zero-height rect): accent fill and a ringed knob.
    void slider(D2D1_RECT_F track,float level,bool muted,bool grabbed,bool knob=true) {
        const auto a=track.left,b=track.right,y=track.top;
        rect(D2D1::RectF(a,y-2,b,y+2),pal.track,2);
        if(!knob)return;
        const float k=a+(b-a)*std::clamp(level,0.0f,1.0f);
        if(k>a+1)rect(D2D1::RectF(a,y-2,k,y+2),muted?pal.ink_muted:pal.accent,2);
        color(pal.light?theme::rgb(0xffffff):theme::rgb(0x2b2b31));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k,y),9,9),brush.get());
        color(pal.tile_edge);target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(k,y),9,9),brush.get(),1);
        color(muted?pal.ink_muted:pal.accent);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k,y),grabbed?5.5f:4.5f,grabbed?5.5f:4.5f),brush.get());
    }
    void draw_settings_main(D2D1_RECT_F r) {
        draw_settings_tiles(r);
        line(r.left+20,r.top+231,r.right-20,r.top+231,pal.separator);
        // Live master volume: the glyph toggles mute; drag, click, wheel
        // or Up/Down set the level.
        if(volume_available&&hover(volume_mute_rect()))rect(volume_mute_rect(),pal.hover,10);
        {
            // Speaker with as many waves as the level; the waves pulse while it changes.
            const float cx=r.left+33.5f,cy=r.top+261;const auto ink=volume_available?pal.ink:pal.ink_secondary;
            icon_part(Icon::Volume,0,cx,cy,18,ink);
            if(!volume_available||volume_muted)icon_part(Icon::Volume,3,cx,cy,18,ink);
            else for(size_t w=0;w<2;++w) {
                if((w==0&&volume_level<=0)||(w==1&&volume_level<.5f))continue;
                const float p=(since(volume_wave)-110*static_cast<float>(w))/380;
                icon_part(Icon::Volume,1+w,cx,cy,18,fade(ink,p>0&&p<1?keys(ease_in_out(p),{1,.15f,1}):1));
            }
        }
        if(volume_available)slider(volume_track(),volume_muted?0.0f:volume_level,volume_muted,slider_active||hover(volume_hit()));
        else slider(volume_track(),0,false,false,false);
        icon(Icon::Chevron,r.right-32,r.top+261,16,pal.ink);
        const std::wstring volume_label=!volume_available?L"No output device":volume_muted?L"Muted":L"Volume "+std::to_wstring(static_cast<int>(std::lround(volume_level*100)))+L"%";
        text(volume_label,D2D1::RectF(r.left+60,r.top+277,r.right-20,r.top+297),0,pal.ink_secondary);
        if(brightness_available) { // The sun's rays turn while it changes.
            const float cx=r.left+33.5f,cy=r.top+310;
            icon_part(Icon::Sun,0,cx,cy,18,pal.ink);icon_part(Icon::Sun,1,cx,cy,18,pal.ink,turn(brightness_active?static_cast<float>(brightness_level)*.9f:0,12,12));
            slider(brightness_track(),static_cast<float>(brightness_level)/100,false,brightness_active||hover(brightness_hit()));
            text(L"Brightness "+std::to_wstring(brightness_level)+L"%",D2D1::RectF(r.left+60,r.top+326,r.right-20,r.top+346),0,pal.ink_secondary);
        }
        line(r.left+1,r.bottom-40,r.right-1,r.bottom-40,pal.separator);
        if(quick_edit) {
            settings_button(quick_add_button(),quick.full()?L"Full":L"+ Add",false);
            settings_button(quick_done_button(),L"Done",true);
            return;
        }
        text(desktop?L"Windows taskbar · Ctrl+Alt+Backspace":(recovery_entry.empty()?L"Recovery bundle not prepared":L"Restore Windows desktop"),D2D1::RectF(r.left+20,r.bottom-33,r.right-88,r.bottom-8),0,pal.ink);
        {const auto e=quick_edit_button();if(hover(e))rect(e,pal.hover,6);icon(Icon::Pencil,(e.left+e.right)/2,(e.top+e.bottom)/2,16,pal.ink);}
        {const auto g=settings_gear();const float cx=(g.left+g.right)/2,cy=(g.top+g.bottom)/2; // The gear turns a quarter on hover.
         icon_part(Icon::Gear,0,cx,cy,18,pal.ink,turn(gear_turn.value(),12,12));icon_part(Icon::Gear,1,cx,cy,18,pal.ink);}
    }
    void draw_settings_pages(D2D1_RECT_F r) {
        tune_motion(); // Springs rest at 1 before the first frame (also in snapshots).
        const float x=page_slide.value();
        if(page_from==settings_page||x>=.999f){draw_page(settings_page,r,0,1,true);return;}
        target->PushAxisAlignedClip(D2D1::RectF(r.left+1,r.top+1,r.right-1,r.bottom-1),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        draw_page(page_from,r,-x*page_dir*40,std::max(0.0f,1-x*1.6f),false);
        draw_page(settings_page,r,(1-x)*page_dir*60,std::clamp(x,0.0f,1.0f),true);
        target->PopAxisAlignedClip();
    }
    void draw_page(SettingsPage page,D2D1_RECT_F r,float dx,float alpha,bool live) {
        if(alpha<=.01f)return;
        const auto current=settings_page;settings_page=page;hover_suppressed=!live;
        D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
        if(dx!=0)target->SetTransform(D2D1::Matrix3x2F::Translation(dx,0)*before);
        const bool faded=alpha<.999f;
        if(faded)target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),alpha),page_layer.get());
        if(page==SettingsPage::Main)draw_settings_main(r);else draw_settings_page(r);
        if(faded)target->PopLayer();
        target->SetTransform(before);settings_page=current;hover_suppressed=false;
    }
    void draw_add_page(D2D1_RECT_F r) {
        page_header(r,L"Add tiles",-1);
        line(r.left+1,r.bottom-44,r.right-1,r.bottom-44,pal.separator);
        {const auto a=page_footer_link();if(hover(a))rect(a,pal.hover,6);text(L"Reset to default",D2D1::RectF(a.left+10,a.top,a.right,a.bottom),1,pal.ink);}
        const auto hidden=addable();
        if(hidden.empty()){page_notice(r,L"Every tile is shown",L"",nullptr);return;}
        if(quick.full()){page_notice(r,L"Quick settings is full",L"Remove a tile to add another.",nullptr);return;}
        const auto area=page_list();
        target->PushAxisAlignedClip(area,D2D1_ANTIALIAS_MODE_ALIASED);
        for(size_t i=0;i<hidden.size();++i) {
            const auto c=add_cell(i);if(c.bottom<=area.top||c.top>=area.bottom)continue;
            rect(c,hover(c)?pal.hover_strong:pal.tile,6);outline(c,pal.tile_edge,6);
            const float cy=(c.top+c.bottom)/2;tile_icon(hidden[i],c.left+20,cy,pal.ink);
            text(spec(hidden[i]).name,D2D1::RectF(c.left+38,c.top,c.right-24,c.bottom),0,pal.ink);
            icon(Icon::Plus,c.right-14,cy,14,pal.ink_secondary);
        }
        target->PopAxisAlignedClip();
    }
    // Project: the four Win+P modes, the current one in accent.
    static constexpr DisplayMode project_modes[]={DisplayMode::Internal,DisplayMode::Clone,DisplayMode::Extend,DisplayMode::External};
    D2D1_RECT_F project_cell(size_t i) const {
        const auto r=panel_rect();const float w=(r.right-r.left-48)/2,x=r.left+20+static_cast<float>(i%2)*(w+8),y=r.top+60+static_cast<float>(i/2)*80;
        return D2D1::RectF(x,y,x+w,y+72);
    }
    void draw_project_page(D2D1_RECT_F r) {
        page_header(r,L"Project",-1);
        page_footer(r,L"Display settings",Icon::Monitor);
        if(display.displays<2){page_notice(r,L"Only one display",L"Connect a display or projector to project.",nullptr);return;}
        static constexpr const wchar_t* names[]={L"PC screen only",L"Duplicate",L"Extend",L"Second screen only"};
        for(size_t i=0;i<4;++i) {
            const auto c=project_cell(i);const bool on=display.mode==project_modes[i];
            rect(c,on?pal.accent:hover(c)?pal.hover_strong:pal.tile,6);if(!on)outline(c,pal.tile_edge,6);
            const auto ink=on?pal.on_accent:pal.ink;
            // Two small screens show the mode: which one is lit, or both.
            const float cx=(c.left+c.right)/2,cy=c.top+26;
            const bool left=i!=3,right=i!=0;
            icon(Icon::Monitor,cx-12,cy,18,fade(ink,left?1:.35f));
            if(i==2)icon(Icon::Monitor,cx+12,cy,18,ink);else icon(Icon::Monitor,cx+12,cy,18,fade(ink,right?1:.35f));
            text(names[i],D2D1::RectF(c.left+6,c.top+44,c.right-6,c.bottom-6),0,ink,true);
        }
    }
    void draw_settings_page(D2D1_RECT_F r) {
        if(settings_page==SettingsPage::Timer){draw_timer_page(r);return;}
        if(settings_page==SettingsPage::Project){draw_project_page(r);return;}
        if(settings_page==SettingsPage::AddTiles){draw_add_page(r);return;}
        const bool wifi=settings_page==SettingsPage::Wifi;
        page_header(r,wifi?L"Wi-Fi":L"Bluetooth",wifi?(net.wifi_present?(net.wifi_on?1:0):-1):(net.bluetooth_present?(net.bluetooth_on?1:0):-1));
        page_footer(r,wifi?L"More Wi-Fi settings":L"More Bluetooth settings",wifi?Icon::Refresh:Icon::Plus);
        if(wifi) {
            if(!net.wifi_present){page_notice(r,L"No Wi-Fi adapter found",L"",nullptr);return;}
            if(!net.wifi_on){page_notice(r,L"Wi-Fi is turned off",L"Turn it on with the switch above.",nullptr);return;}
            if(net.location_denied){page_notice(r,L"Windows hides nearby networks",L"Let desktop apps use your location to list them.",L"Location settings");return;}
            if(net.networks.empty()){page_notice(r,net.scanning?L"Looking for networks\x2026":L"No networks found",L"",nullptr);return;}
        } else {
            if(!net.bluetooth_present){page_notice(r,L"No Bluetooth adapter found",L"",nullptr);return;}
            if(!net.bluetooth_on){page_notice(r,L"Bluetooth is turned off",L"Turn it on with the switch above.",nullptr);return;}
            if(net.devices.empty()){page_notice(r,L"No paired devices",L"Add one with + below.",nullptr);return;}
        }
        const auto area=page_list();
        target->PushAxisAlignedClip(area,D2D1_ANTIALIAS_MODE_ALIASED);
        float order=0;
        for(const auto& row:list_rows()) {
            if(row.box.bottom<=area.top||row.box.top>=area.bottom)continue;
            // Rows fade and rise in one after another when the page opens.
            const float t=since(list_opened)-60-45*order++;
            if(t<=0)continue;
            const float e=t<260?bezier(.2f,.8f,.2f,1,t/260):1;
            D2D1_MATRIX_3X2_F before;const bool easing=e<.999f;
            if(easing) {
                target->GetTransform(&before);target->SetTransform(D2D1::Matrix3x2F::Translation(0,(1-e)*8)*before);
                target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),e),row_layer.get());
            }
            if(wifi)draw_network(row);else draw_device(row);
            if(easing){target->PopLayer();target->SetTransform(before);}
        }
        target->PopAxisAlignedClip();
    }
    static D2D1_COLOR_F fade(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
    std::wstring query;
    std::wstring recovery_entry;
    std::wstring clock_time,clock_date,clock_short;

    void initialize() {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));
        check(factory->CreatePathGeometry(launcher_mark.put()));
        ComPtr<ID2D1GeometrySink> logo_sink;check(launcher_mark->Open(logo_sink.put()));
        logo_sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        for(size_t i=1;i<launcher_logo::offsets.size();++i) {
            const auto begin=launcher_logo::offsets[i-1],end=launcher_logo::offsets[i];
            logo_sink->BeginFigure(launcher_logo::points[begin],D2D1_FIGURE_BEGIN_FILLED);
            logo_sink->AddLines(launcher_logo::points.data()+begin+1,static_cast<UINT32>(end-begin-1));
            logo_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }
        check(logo_sink->Close());
        build_icons();
        check(factory->CreatePathGeometry(media_play_mark.put()));
        ComPtr<ID2D1GeometrySink> play_sink;check(media_play_mark->Open(play_sink.put()));
        play_sink->BeginFigure(D2D1::Point2F(-3,-4.5f),D2D1_FIGURE_BEGIN_FILLED);
        play_sink->AddLine(D2D1::Point2F(4,0));play_sink->AddLine(D2D1::Point2F(-3,4.5f));
        play_sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(play_sink->Close());
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IDWriteFontCollection> collection;check(write->GetSystemFontCollection(collection.put()));
        const auto ui_family=theme::text_family(collection.get());
        const float sizes[] = {11, 13, 16, 28, 40, 18, 12, 12};
        for (size_t i = 0; i < fonts.size(); ++i) {
            const auto weight=i==5?DWRITE_FONT_WEIGHT_NORMAL:DWRITE_FONT_WEIGHT_LIGHT;
            check(write->CreateTextFormat(i==5?L"Segoe Fluent Icons":ui_family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-us", fonts[i].put()));
            fonts[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fonts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis; write->CreateEllipsisTrimmingSign(fonts[i].get(), ellipsis.put());
            fonts[i]->SetTrimming(&trimming, ellipsis.get());
        }
        check(write->CreateTextFormat(L"Consolas",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11.0f,L"en-us",mono.put()));
        mono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        for(auto [format,size]:{std::pair{&emoji_small,20.0f},std::pair{&emoji_big,72.0f}}) {
            check(write->CreateTextFormat(L"Segoe UI Emoji",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",format->put()));
            (*format)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);(*format)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);(*format)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        initialized = true;
        update_clock();
    }
    void update_clock() {
        wchar_t time[64]{},date[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS|(clock24?TIME_FORCE24HOURFORMAT:0),nullptr,nullptr,time,64);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,nullptr,nullptr,date,64,nullptr);
        clock_time=time; clock_date=date;
        // Compact date beside the next prayer, e.g. "14 Oct 26".
        wchar_t compact[64]{};GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,nullptr,L"d MMM yy",compact,64,nullptr);clock_short=compact;
    }
    void bind(ID2D1RenderTarget* render) {
        target.reset(); render->AddRef(); *target.put() = render;
        check(target->CreateSolidColorBrush(theme::primary(), brush.put()));
        const auto layer_size=target->GetSize();check(target->CreateLayer(&layer_size,panel_layer.put()));
        check(target->CreateLayer(&layer_size,page_layer.put()));check(target->CreateLayer(&layer_size,row_layer.put()));
        app_bitmaps.clear();
        backdrop.reset(); material.reset(); sheen.reset(); cache_width=cache_height=0;
        painter.palette=pal; painter.live=live_glass; painter.bind(render,factory.get());
        icon_cache.clear();
        rebuild_icons();
    }
    // Next drawing session of the same Direct2D device: keep every resource.
    void retarget(ID2D1RenderTarget* render) {
        target.reset(); render->AddRef(); *target.put() = render; painter.retarget(render);
    }
    std::shared_ptr<IconBitmap> bitmap_icon(const NativeIcon& icon,float dip_size=24) {
        if(!icon) return {};
        float dx=96,dy=96;target->GetDpi(&dx,&dy);
        const auto pixels=static_cast<UINT>(std::max(1.0f,std::round(dip_size*dx/96)));
        if(const auto found=icon_cache.find(icon.get());found!=icon_cache.end()) {
            if(const auto size=found->second.find(pixels);size!=found->second.end())return size->second;
        }
        auto result=std::make_shared<IconBitmap>();result->source=icon;
        ComPtr<IWICBitmap> bitmap;ComPtr<IWICBitmapScaler> scaled;ComPtr<IWICFormatConverter> converted;
        if(FAILED(wic->CreateBitmapFromHICON(static_cast<HICON>(icon.get()),bitmap.put())) || FAILED(wic->CreateFormatConverter(converted.put()))) return result;
        if(FAILED(wic->CreateBitmapScaler(scaled.put()))||FAILED(scaled->Initialize(bitmap.get(),pixels,pixels,WICBitmapInterpolationModeFant)))return {};
        if(SUCCEEDED(converted->Initialize(scaled.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) target->CreateBitmapFromWicBitmap(converted.get(),nullptr,result->bitmap.put());
        icon_cache[icon.get()].emplace(pixels,result);
        return result;
    }
    void rebuild_icons() {
        app_icons.clear(); desktop_icons.clear();tray_icons.clear();
        if(!target) return;
        // 32 DIP copies keep magnified icons (x1.22 plus overshoot) sharp; rest state stays pixel-exact at 24.
        app_icons_large.clear();
        for(const auto& g:groups) {app_icons.push_back(bitmap_icon(g.icon,icon_px));app_icons_large.push_back(bitmap_icon(g.icon,icon_px*4/3));}
        for(const auto& i:tray_items)tray_icons.push_back(bitmap_icon(i.icon,20));
        for(size_t i=0;i<std::min<size_t>(8,inventory.desktop.size());++i)desktop_icons.push_back(bitmap_icon(inventory.desktop[i].icon,34));
        prune_icon_cache();
    }
    // Keep only bitmaps still displayed. Each entry owns its source HICON, so
    // replaced tray/window icons are released instead of accumulating.
    void prune_icon_cache() {
        std::unordered_set<const IconBitmap*> used;
        for(const auto* list:{&app_icons,&desktop_icons,&tray_icons,&app_icons_large})for(const auto& icon:*list)if(icon)used.insert(icon.get());
        for(auto outer=icon_cache.begin();outer!=icon_cache.end();) {
            std::erase_if(outer->second,[&](const auto& entry){return !used.contains(entry.second.get());});
            outer=outer->second.empty()?icon_cache.erase(outer):std::next(outer);
        }
    }
    void background(ID2D1RenderTarget* rt,ID2D1SolidColorBrush* b) {
        rt->Clear(theme::background());
        const auto ellipse=[&](unsigned c,float x,float y,float rx,float ry) { b->SetColor(theme::rgb(c));rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(width*x,height*y),width*rx,height*ry),b); };
        ellipse(0x19232f,.77f,.12f,.47f,.65f); ellipse(0x243444,.93f,.38f,.38f,.55f);
        ellipse(0x30465a,1.02f,.70f,.34f,.51f); ellipse(0x496079,1.12f,.97f,.31f,.49f);
        ellipse(0x274f68,.17f,1.12f,.29f,.34f); ellipse(0x32637a,.12f,1.14f,.21f,.24f);
    }
    void cache_material() {
        if(cache_width==width&&cache_height==height&&material) return;
        if(!desktop) {
        const auto bw=static_cast<UINT>(std::max(1.0f,width/4)),bh=static_cast<UINT>(std::max(1.0f,height/4));
        ComPtr<IWICBitmap> image; ComPtr<ID2D1RenderTarget> offscreen; ComPtr<ID2D1SolidColorBrush> b;
        check(wic->CreateBitmap(bw,bh,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,image.put()));
        check(factory->CreateWicBitmapRenderTarget(image.get(),D2D1::RenderTargetProperties(),offscreen.put()));
        check(offscreen->CreateSolidColorBrush(theme::primary(),b.put()));
        offscreen->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(bw)/width,static_cast<float>(bh)/height));
        offscreen->BeginDraw(); background(offscreen.get(),b.get()); check(offscreen->EndDraw());
        ComPtr<IWICBitmapLock> lock; WICRect region{0,0,static_cast<INT>(bw),static_cast<INT>(bh)};
        check(image->Lock(&region,WICBitmapLockRead,lock.put()));
        UINT stride=0,bytes=0; BYTE* data=nullptr; lock->GetStride(&stride); lock->GetDataPointer(&bytes,&data);
        std::vector<BYTE> pixels(static_cast<size_t>(bw)*bh*4),temp(pixels.size());
        for(UINT y=0;y<bh;++y) std::copy_n(data+y*stride,bw*4,pixels.data()+static_cast<size_t>(y)*bw*4);
        // Two separable box passes approximate frosted blur. Cached on size changes only.
        for(int pass=0;pass<2;++pass) {
            for(UINT y=0;y<bh;++y) for(UINT x=0;x<bw;++x) for(UINT c=0;c<4;++c) {
                unsigned sum=0; for(int k=-3;k<=3;++k) { const auto xx=std::clamp(static_cast<int>(x)+k,0,static_cast<int>(bw)-1); sum+=pixels[(static_cast<size_t>(y)*bw+xx)*4+c]; }
                temp[(static_cast<size_t>(y)*bw+x)*4+c]=static_cast<BYTE>(sum/7);
            }
            for(UINT y=0;y<bh;++y) for(UINT x=0;x<bw;++x) for(UINT c=0;c<4;++c) {
                unsigned sum=0; for(int k=-3;k<=3;++k) { const auto yy=std::clamp(static_cast<int>(y)+k,0,static_cast<int>(bh)-1); sum+=temp[(static_cast<size_t>(yy)*bw+x)*4+c]; }
                pixels[(static_cast<size_t>(y)*bw+x)*4+c]=static_cast<BYTE>(sum/7);
            }
        }
        ComPtr<ID2D1Bitmap> bitmap;
        check(target->CreateBitmap(D2D1::SizeU(bw,bh),pixels.data(),bw*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),bitmap.put()));
        check(target->CreateBitmapBrush(bitmap.get(),backdrop.put()));
        backdrop->SetTransform(D2D1::Matrix3x2F::Scale(width/bw,height/bh));
        }
        const auto stops=theme::material_stops();
        ComPtr<ID2D1GradientStopCollection> collection; check(target->CreateGradientStopCollection(stops.data(),static_cast<UINT32>(stops.size()),collection.put()));
        check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,48)),collection.get(),material.put()));
        const auto highlights=theme::sheen_stops();
        check(target->CreateGradientStopCollection(highlights.data(),static_cast<UINT32>(highlights.size()),collection.put()));
        check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,24)),collection.get(),sheen.put()));
        cache_width=width;cache_height=height;
    }
    std::vector<D2D1_RECT_F> overlay_rects; // drawn outside the dock/flyout (tooltips)
    void glass_tooltip(D2D1_RECT_F r) {painter.paint(r,8,GlassKind::Tooltip);overlay_rects.push_back(r);}
    void glass(D2D1_RECT_F r,float radius,bool panel=false) {
        painter.paint(r,radius,panel?GlassKind::Panel:GlassKind::Dock,!desktop&&backdrop?backdrop.get():nullptr);
    }
    void color(D2D1_COLOR_F c) { brush->SetColor(c); }
    void rect(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        color(c);
        if (radius) target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get()); else target->FillRectangle(r, brush.get());
    }
    void outline(D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float thickness=1) {
        color(c); target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get(), thickness);
    }
    void text(std::wstring_view label, D2D1_RECT_F r, size_t font, D2D1_COLOR_F c, bool center = false) {
        color(c); fonts[font]->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        target->DrawText(label.data(), static_cast<UINT32>(label.size()), fonts[font].get(), r, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    D2D1_RECT_F tooltip_rect(std::wstring_view label,D2D1_RECT_F anchor) {
        if(!tooltip_measure||tooltip_label!=label) {
            tooltip_label=label;
            check(write->CreateTextLayout(label.data(),static_cast<UINT32>(label.size()),fonts[6].get(),16384,26,tooltip_measure.put()));
            tooltip_measure->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        DWRITE_TEXT_METRICS metrics{};check(tooltip_measure->GetMetrics(&metrics));
        const auto w=std::clamp(std::ceil(metrics.widthIncludingTrailingWhitespace)+16,24.0f,std::max(24.0f,std::min(300.0f,width-16)));
        const auto left=std::clamp((anchor.left+anchor.right-w)/2,8.0f,std::max(8.0f,width-w-8));
        const auto bottom=dock().top-6;
        return D2D1::RectF(left,bottom-26,left+w,bottom);
    }
    void tooltip(std::wstring_view label,D2D1_RECT_F anchor) {
        const auto r=tooltip_rect(label,anchor);
        glass_tooltip(r);
        text(label,D2D1::RectF(r.left+8,r.top,r.right-8,r.bottom),6,pal.ink);
    }
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float stroke=1) {
        color(c); target->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush.get(),stroke);
    }
    void media_symbol(size_t index,D2D1_RECT_F button,D2D1_COLOR_F ink) {
        const auto x=(button.left+button.right)/2,y=(button.top+button.bottom)/2;
        D2D1_MATRIX_3X2_F saved;target->GetTransform(&saved);
        target->SetTransform(D2D1::Matrix3x2F::Scale(1.5f,1.5f)*D2D1::Matrix3x2F::Translation(x,y)*saved);
        if(index==1) {
            if(media.playing){line(-2,-4,-2,4,ink,1.2f);line(2,-4,2,4,ink,1.2f);}
            else {color(ink);target->FillGeometry(media_play_mark.get(),brush.get());}
        } else {
            const float direction=index==0?-1.0f:1.0f;
            line(-3*direction,-4,2*direction,0,ink,.9f);
            line(2*direction,0,-3*direction,4,ink,.9f);
            line(5*direction,-4,5*direction,4,ink,.9f);
        }
        target->SetTransform(saved);
    }
    size_t shown_groups() const {
        return std::min(groups.size(),DockLayout::capacity(width-40));
    }
    bool overflow(size_t i) const { return groups.size()>shown_groups()&&i+1==shown_groups(); }
    bool tile_running(size_t i) const {
        if(overflow(i)) return std::any_of(groups.begin()+static_cast<std::ptrdiff_t>(i),groups.end(),[](const Group& g){return !g.windows.empty();});
        return !groups[i].windows.empty();
    }
    DockLayout layout() const {
        DockLayout l{width/2,height-34,width-40,shown_groups()};if(slots_placed)l.slots=slots_visual.value();l.media=show_media;
        if(bar) { // 48 DIP along the bottom edge; apps centred or after the media wing.
            l.bar=true;l.bar_width=width;l.center_y=height-24;l.available_width=width;
            if(bar_left)l.center=(show_media?DockLayout::utility_width+40:12)+l.collapsed_width()/2;
        }
        return l;
    }
    float app_x(size_t index) const { return layout().app_x(index); }
    D2D1_RECT_F app_rect(size_t i) const { const auto x=app_x(i),y=layout().center_y;return D2D1::RectF(x-18,y-19,x+18,y+19); }
    D2D1_RECT_F launcher_rect() const { const auto l=layout();const float x=l.left()+DockLayout::edge;return D2D1::RectF(x,l.center_y-22,x+DockLayout::button,l.center_y+22); }
    D2D1_RECT_F dock() const {
        const auto l=layout();const auto p=machine.progress(),h=l.height(p);
        if(l.bar)return D2D1::RectF(0,height-48,width,height);
        const float left=l.center-l.collapsed_width()/2+(l.expanded_left()-(l.center-l.collapsed_width()/2))*p;
        const float right=l.center+l.collapsed_width()/2+(l.expanded_right()-(l.center+l.collapsed_width()/2))*p;
        return D2D1::RectF(left,l.center_y-h/2,right,l.center_y+h/2);
    }
    D2D1_RECT_F media_rect() const { const auto l=layout();return D2D1::RectF(l.expanded_left()+12,l.center_y-18,l.expanded_left()+DockLayout::utility_width+4,l.center_y+18); }
    D2D1_RECT_F media_button(size_t index) const {const auto r=media_rect();const auto x=(r.left+r.right)/2+(static_cast<float>(index)-1)*40;return D2D1::RectF(x-16,r.top+19,x+16,r.bottom+1);}
    void measure_media_title() {
        const auto label=media.available?media.title:(media.unavailable?L"Media unavailable":L"No media");
        if(media_title&&media_title_text==label)return;
        media_title_text=label;media_title.reset();
        check(write->CreateTextLayout(label.data(),static_cast<UINT32>(label.size()),fonts[6].get(),16384,17,media_title.put()));
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_NONE,0,0};media_title->SetTrimming(&trimming,nullptr);media_title->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DWRITE_TEXT_METRICS metrics{};check(media_title->GetMetrics(&metrics));
        const auto r=media_rect();media_marquee.configure(media.available?metrics.widthIncludingTrailingWhitespace-(r.right-r.left):0);
    }
    bool media_enabled(size_t index) const {return media.available&&(index==0?media.previous:index==1?media.toggle:media.next);}
    D2D1_RECT_F system_rect() const { const auto l=layout();return D2D1::RectF(l.expanded_right()-DockLayout::utility_width-10-DockLayout::desktop_width,l.center_y-20,l.expanded_right()-10-DockLayout::desktop_width,l.center_y+20); }
    D2D1_RECT_F tray_rect() const {const auto s=system_rect();const auto cy=layout().center_y;return D2D1::RectF(s.left,cy-19,s.left+24,cy+19);}
    D2D1_RECT_F quick_settings_rect() const { const auto s=system_rect();const auto cy=layout().center_y;return D2D1::RectF(s.left+24,cy-19,s.left+77,cy+19); }
    D2D1_RECT_F clock_rect() const { const auto s=system_rect();const auto cy=layout().center_y;return D2D1::RectF(s.left+81,cy-19,s.right,cy+19); }
    D2D1_RECT_F desktop_button_rect() const {
        const auto l=layout();const auto right=l.expanded_right(),cy=l.center_y;
        // Keep the hidden sliver in its utility wing while the capsule reveals
        // it, rather than sweeping a desktop target across the last app.
        return D2D1::RectF(right-4-DockLayout::desktop_width,cy-13,right-4,cy+13);
    }
    Panel drawn_panel() const {return panel==Panel::None?exiting_panel:panel;}
    D2D1_RECT_F panel_rect() const {
        const auto panel=drawn_panel();
        if(panel==Panel::Tray&&native_tray){const auto columns=std::min<size_t>(5,std::max<size_t>(1,tray_items.size()));const float w=32+44*static_cast<float>(columns),h=80+44*static_cast<float>(std::min<size_t>(6,std::max<size_t>(1,(tray_items.size()+columns-1)/columns)));const auto button=tray_rect();const float left=std::clamp((button.left+button.right-w)/2,20.0f,std::max(20.0f,width-20-w)),bottom=dock().top-12+panel_reveal.offset();return D2D1::RectF(left,bottom-h,left+w,bottom);}
        const float bottom=dock().top-12+panel_reveal.offset();
        if(panel==Panel::Launcher) {
            // Centred on the dock.
            const auto d=dock();
            const float w=std::min(width-40,launch_width),left=std::clamp((d.left+d.right-w)/2,20.0f,std::max(20.0f,width-20-w));
            return D2D1::RectF(left,bottom-launch_height,left+w,bottom);
        }
        const float w=std::min(width-40,360.0f);
        const float right=std::min(width-20,system_rect().right+8);
        return D2D1::RectF(right-w,bottom-(panel==Panel::Settings&&brightness_available?392:348),right,bottom);
    }
    D2D1_RECT_F tray_tile(size_t i) const {const auto r=panel_rect();const auto columns=std::min<size_t>(5,std::max<size_t>(1,tray_items.size()));const auto x=r.left+16+44*static_cast<float>(i%columns),y=r.top+44+44*static_cast<float>(i/columns);return D2D1::RectF(x,y,x+44,y+44);}
    int tray_hit(float x,float y) const {if(panel!=Panel::Tray||!native_tray)return -1;for(size_t i=0;i<std::min<size_t>(30,tray_items.size()-std::min(tray_scroll,tray_items.size()));++i)if(inside(tray_tile(i),x,y))return static_cast<int>(i+tray_scroll);return -1;}
    void clear_press(){pressed=false;control_pressed=-1;for(auto& g:groups)g.pressed=false;}
    void press_at(float x,float y) {
        clear_press();if(!inside(dock(),x,y))return;
        if(inside(launcher_rect(),x,y)){pressed=true;return;}
        for(size_t i=0;i<shown_groups();++i)if(inside(app_rect(i),x,y)){groups[i].pressed=true;return;}
        const auto count=static_cast<int>(shown_groups());
        if(machine.progress()>.85f&&inside(desktop_button_rect(),x,y)){control_pressed=count+4;return;}
        if(machine.progress()>.85f){
            if(inside(tray_rect(),x,y))control_pressed=count+1;
            else if(inside(quick_settings_rect(),x,y))control_pressed=count+2;
            else if(inside(clock_rect(),x,y))control_pressed=count+3;
            else if(show_media)for(size_t i=0;i<3;++i)if(media_enabled(i)&&inside(media_button(i),x,y))control_pressed=count+5+static_cast<int>(i);
        }
    }
    size_t active_pulses() const {
        size_t count=launcher_pulse.active()+tray_pulse.active()+settings_pulse.active()+clock_pulse.active()+desktop_pulse.active();
        for(const auto& g:groups)if(g.pulse.active())++count;
        for(const auto& pulse:media_pulses)if(pulse.active())++count;
        return count;
    }
    void ensure_motion() {
        const size_t n=shown_groups()+1;
        const auto hover_spring=[](float rest,float precision){Spring s;s.tune(hover_stiffness,hover_damping);s.precision(precision);s.snap(rest);return s;};
        while(magnify.size()<n)magnify.push_back(hover_spring(1,.002f));
        while(rise.size()<n)rise.push_back(hover_spring(0,.05f));
        while(spread.size()<n)spread.push_back(hover_spring(0,.05f));
        while(shift.size()<n){Spring s;s.precision(.05f);shift.push_back(s);}
        magnify.resize(n);shift.resize(n);rise.resize(n);spread.resize(n);
    }
    int hover_slot(float x,float y) const {
        if(!machine.visible()||!inside(dock(),x,y))return -1;
        if(inside(launcher_rect(),x,y))return 0;
        for(size_t i=0;i<shown_groups();++i)if(inside(app_rect(i),x,y))return static_cast<int>(i)+1;
        return -1;
    }
    // The hovered icon springs up and grows; its two neighbours ease aside a little.
    // Slots (hit targets) never move.
    void set_hovered(int slot,DockMachine::Time now) {
        ensure_motion();hovered=slot;
        for(size_t k=0;k<magnify.size();++k) {
            float grow=1,lift=0,aside=0;
            if(slot>=0&&dragging<0&&!reduced) {
                const int d=std::abs(static_cast<int>(k)-slot);
                // Only the two direct neighbours make room; icons further away stay
                // still (sub-pixel nudges there read as a jittery ripple).
                if(d==0){grow=hover_grow;lift=hover_lift*std::min(1.0f,(hover_grow-1)/.22f);}
                else if(d==1)aside=static_cast<int>(k)<slot?-hover_spread:hover_spread;
            }
            magnify[k].target(grow,now,reduced);rise[k].target(lift,now,reduced);spread[k].target(aside,now,reduced);
        }
    }
    // Follows tooltip_target: the name springs in, glides to the next icon while
    // shown, and fades out with its last label.
    void sync_tooltip(DockMachine::Time now) {
        if(tip_show.value()==0&&tip_show.goal()==0)tip_show.tune(hover_stiffness,hover_damping),tip_x.tune(hover_stiffness,hover_damping),tip_show.precision(.004f),tip_x.precision(.05f);
        const bool shown=tooltip_target>=0&&dragging<0&&panel==Panel::None&&!pressed&&static_cast<size_t>(tooltip_target)<=shown_groups();
        if(!shown){tip_show.target(0,now,reduced);return;}
        float x=layout().launcher_x();std::wstring label=L"Applications";
        if(tooltip_target>0){const auto i=static_cast<size_t>(tooltip_target-1);x=app_x(i);label=overflow(i)?L"More applications":groups[i].name;}
        if(tip_show.value()<.05f||tip_label.empty())tip_x.snap(x);
        tip_label=std::move(label);tip_x.target(x,now,reduced);tip_show.target(1,now,reduced);
    }
    void name_tooltip() {
        const float show=std::clamp(tip_show.value(),0.0f,1.25f);
        if(show<=.01f||tip_label.empty())return;
        const auto r=tooltip_rect(tip_label,D2D1::RectF(tip_x.value()-1,0,tip_x.value()+1,0));
        const float alpha=std::min(1.0f,show),grow=.9f+.1f*show,drop=(1-std::min(1.0f,show))*6;
        D2D1_MATRIX_3X2_F before;target->GetTransform(&before);
        target->SetTransform(D2D1::Matrix3x2F::Scale(grow,grow,D2D1::Point2F((r.left+r.right)/2,r.bottom))*D2D1::Matrix3x2F::Translation(0,drop)*before);
        const bool fading=alpha<.999f;
        if(fading)target->PushLayer(D2D1::LayerParameters(D2D1::RectF(r.left-12,r.top-12,r.right+12,r.bottom+16),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),alpha),panel_layer.get());
        painter.layer_opacity=alpha;glass_tooltip(r);painter.layer_opacity=1;
        text(tip_label,D2D1::RectF(r.left+8,r.top,r.right-8,r.bottom),6,pal.ink);
        if(fading)target->PopLayer();
        target->SetTransform(before);
    }
    int active_slot() const {
        if(!active_window)return -1;
        for(size_t i=0;i<shown_groups();++i)if(!overflow(i)&&std::find(groups[i].windows.begin(),groups[i].windows.end(),active_window)!=groups[i].windows.end())return static_cast<int>(i);
        return -1;
    }
    float indicator_left=0;
    void sync_indicator(DockMachine::Time now) {
        const int slot=active_slot();
        if(slot<0){indicator.clear();indicator_shown=false;return;}
        // When the whole group re-centres (an app opened or closed), every slot
        // moves at once: jump with it instead of gliding behind.
        if(const float left=layout().left();left!=indicator_left){indicator_left=left;indicator.clear();}
        indicator.to(app_x(static_cast<size_t>(slot)),now,reduced);indicator_shown=true;indicator_x=indicator.current(now);
    }
    bool wants_attention(size_t i) const {
        for(const auto window:groups[i].windows)if(attention.contains(window))return true;
        return false;
    }
    bool attention_pulsing() const{return !attention.empty()&&!reduced&&clock_now-attention_start<4800;}
    bool motion_moving() const {
        for(const auto* list:{&magnify,&shift,&rise,&spread})for(const auto& spring:*list)if(spring.moving())return true;
        if(tip_show.moving()||tip_x.moving())return true;
        if(slots_visual.moving()||!ghosts.empty()||appearing())return true;
        for(const auto& spring:wings)if(spring.moving())return true;
        if(panel_motion_active()||launcher_motion_active())return true;
        return indicator.moving()||attention_pulsing();
    }
    bool feedback_moving() const{return active_pulses()!=0||panel_reveal.active()||motion_moving();}
    void tick_feedback(DockMachine::Time time) {
        clock_now=time;
        for(auto* list:{&magnify,&shift,&rise,&spread})for(auto& spring:*list)spring.tick(time);
        sync_tooltip(time);tip_show.tick(time);tip_x.tick(time);
        sync_wings(time);for(auto& spring:wings)spring.tick(time);
        sync_launcher(time);
        if(slots_placed&&slots_visual.goal()!=static_cast<float>(shown_groups()))slots_visual.target(static_cast<float>(shown_groups()),time,reduced);
        slots_visual.tick(time);
        std::erase_if(ghosts,[&](const Ghost& g){return reduced||static_cast<float>(time-g.start)>=vanish_time;});
        sync_panel_motion(time);for(auto& spring:tile_press)spring.tick(time);gear_turn.tick(time);knob.tick(time);page_slide.tick(time);quick_slide.tick(time);for(size_t i=0;i<tile_count;++i){tile_dx[i].tick(time);tile_dy[i].tick(time);}
        indicator.tick(time);if(indicator_shown)indicator_x=indicator.current(time);
        launcher_pulse.tick(time);tray_pulse.tick(time);settings_pulse.tick(time);clock_pulse.tick(time);desktop_pulse.tick(time);panel_reveal.tick(time);
        if(panel_reveal.hidden())exiting_panel=Panel::None;
        for(auto& g:groups)g.pulse.tick(time);
        for(auto& pulse:media_pulses)pulse.tick(time);
    }
    void stop_feedback() {
        launcher_pulse.stop();tray_pulse.stop();settings_pulse.stop();clock_pulse.stop();desktop_pulse.stop();panel_reveal.settle();exiting_panel=Panel::None;
        for(auto& g:groups)g.pulse.stop();clear_press();
        for(auto& pulse:media_pulses)pulse.stop();
        for(auto* list:{&magnify,&shift,&rise,&spread})for(auto& spring:*list)spring.snap(spring.goal());
        tip_show.snap(tip_show.goal());tip_x.snap(tip_x.goal());
        slots_visual.snap(slots_visual.goal());ghosts.clear();for(auto& g:groups)g.appeared=-1;
        for(auto& spring:wings)spring.snap(spring.goal());
        for(auto& spring:tile_press)spring.snap(spring.goal());gear_turn.snap(gear_turn.goal());knob.snap(knob.goal());page_slide.snap(page_slide.goal());quick_slide.snap(quick_slide.goal());for(size_t i=0;i<tile_count;++i){tile_dx[i].snap(0);tile_dy[i].snap(0);}
    }
    float click_scale(const ClickPulse& pulse,bool down=false) const{return reduced?1.0f:down?.94f:pulse.scale();}
    void bubble(float x,float y,const ClickPulse& pulse,float opacity=1,float squeeze=1) {
        if(!pulse.active())return;
        const auto radius=pulse.radius(),alpha=pulse.opacity()*opacity;
        const auto ellipse=D2D1::Ellipse(D2D1::Point2F(x,y),radius*squeeze,radius);
        const unsigned ink=pal.light?0x000000:0xffffff;
        color(theme::rgb(ink,alpha));target->FillEllipse(ellipse,brush.get());
        color(theme::rgb(ink,alpha*1.35f));target->DrawEllipse(ellipse,brush.get(),.8f);
    }
    D2D1_RECT_F desktop_item(size_t i) const {
        const float x = 30 + static_cast<float>(i / 4) * 112, y = 174 + static_cast<float>(i % 4) * 101;
        return D2D1::RectF(x,y,x+92,y+90);
    }
    static bool same_identity(const Group& a,const Group& b) {
        if(!a.app_id.empty()&&!b.app_id.empty())return a.app_id==b.app_id;
        if(!a.executable.empty()&&!b.executable.empty())return _wcsicmp(a.executable.c_str(),b.executable.c_str())==0;
        if(!a.launch_path.empty()&&!b.launch_path.empty())return _wcsicmp(a.launch_path.c_str(),b.launch_path.c_str())==0;
        return !a.windows.empty()&&!b.windows.empty()&&a.windows.front()==b.windows.front();
    }
    bool accept(Inventory value, bool full) {
        if (full) inventory = std::move(value); else inventory.windows = std::move(value.windows);
        // Where each icon is drawn now, to slide it to its new place afterwards.
        std::vector<float> old_x;std::vector<std::shared_ptr<IconBitmap>> old_icons=app_icons;
        for(size_t i=0;i<shown_groups();++i)old_x.push_back(app_x(i)+(i+1<shift.size()?shift[i+1].value():0));
        const auto previous=std::move(groups);groups.clear();
        for(const auto& pin:inventory.pinned) {
            if(std::any_of(groups.begin(),groups.end(),[&](const Group& g){return (!pin.app_id.empty()&&pin.app_id==g.app_id)||(!pin.target.empty()&&!g.executable.empty()&&_wcsicmp(g.executable.c_str(),pin.target.c_str())==0);}))continue;
            groups.push_back({pin.title,std::wstring(1,static_cast<wchar_t>(towupper(pin.title[0]))),pin.target,{},pin.path,pin.icon,true,pin.app_id});
            // A pin saved from a helper window (e.g. steamwebhelper) opens the real app.
            if(const auto* app=helper_owner(pin.target);app&&pin.app_id.find(L'!')==std::wstring::npos) {
                auto& g=groups.back();g.name=app->name;g.initial=std::wstring(1,static_cast<wchar_t>(towupper(app->name[0])));g.launch_path=app->program;
            }
        }
        for (const auto& window : inventory.windows) {
            const auto* owner=window.app_id.find(L'!')==std::wstring::npos?helper_owner(window.executable):nullptr;
            auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
                if(!window.app_id.empty()&&!g.app_id.empty()&&window.app_id==g.app_id)return true;
                if(!window.app_id.empty()&&!g.app_id.empty())return false;
                if(!window.executable.empty()&&!g.executable.empty()&&_wcsicmp(g.executable.c_str(),window.executable.c_str())==0)return true;
                // A helper's window joins its app's pin (Steam's window is steamwebhelper's).
                return owner&&!g.launch_path.empty()&&_wcsicmp(g.launch_path.c_str(),owner->program.c_str())==0;
            });
            if (found == groups.end()) {
                std::wstring name = window.executable.empty() ? window.title : std::filesystem::path(window.executable).stem().wstring();
                if(owner)name=owner->name;
                groups.push_back({name, std::wstring(1, static_cast<wchar_t>(towupper(name[0]))), window.executable, {window.hwnd},owner?owner->program:window.executable,window.icon,false,window.app_id});
            } else { found->windows.push_back(window.hwnd); if(!found->icon) found->icon=window.icon; }
        }
        // Rank each group once; the sort comparator then compares cached ranks.
        for(auto& g:groups)std::sort(g.windows.begin(),g.windows.end(),std::less<HWND>{});
        std::vector<size_t> ranks(groups.size());
        for(size_t i=0;i<groups.size();++i) {
            ranks[i]=previous.size();
            for(size_t j=0;j<previous.size();++j)if(same_identity(groups[i],previous[j])){ranks[i]=j;break;}
        }
        std::vector<size_t> order(groups.size());
        for(size_t i=0;i<order.size();++i)order[i]=i;
        // EnumWindows follows foreground Z order. Keep established app order instead.
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){
            if(groups[a].pinned!=groups[b].pinned)return groups[a].pinned;
            return !groups[a].pinned&&ranks[a]<ranks[b];
        });
        std::vector<Group> sorted;sorted.reserve(groups.size());
        for(const auto i:order) {
            auto& g=groups[i];
            if(ranks[i]<previous.size()){g.pulse=previous[ranks[i]].pulse;g.pressed=previous[ranks[i]].pressed;g.appeared=previous[ranks[i]].appeared;}
            else if(!previous.empty())g.appeared=clock_now; // Not on first load.
            sorted.push_back(std::move(g));
        }
        groups=std::move(sorted);
        const bool changed=full||groups.size()!=previous.size()||!std::equal(groups.begin(),groups.end(),previous.begin(),[](const Group& a,const Group& b){
            return a.name==b.name&&a.executable==b.executable&&a.app_id==b.app_id&&a.windows==b.windows&&a.launch_path==b.launch_path&&a.pinned==b.pinned&&a.icon.get()==b.icon.get();
        });
        if(!changed)return false;
        rebuild_icons();
        animate_changes(previous,old_x,old_icons);
        return true;
    }
    void animate_changes(const std::vector<Group>& previous,const std::vector<float>& old_x,const std::vector<std::shared_ptr<IconBitmap>>& old_icons) {
        const float shown=static_cast<float>(shown_groups());
        if(!slots_placed||previous.empty()||reduced){slots_visual.tune(300,30);slots_visual.precision(.002f);slots_visual.snap(shown);slots_placed=true;ghosts.clear();return;}
        slots_visual.target(shown,clock_now,reduced);
        ensure_motion();
        std::vector<bool> kept(previous.size(),false);
        for(size_t j=0;j<shown_groups();++j) {
            size_t k=previous.size();
            for(size_t p=0;p<previous.size();++p)if(same_identity(groups[j],previous[p])){k=p;break;}
            if(k<previous.size())kept[k]=true;
            // Survivors slide from where they were drawn to their new slot.
            if(k<old_x.size()&&j+1<shift.size()){shift[j+1].snap(old_x[k]-app_x(j));shift[j+1].target(0,clock_now,reduced);}
        }
        // Closed apps shrink and fade where they were.
        for(size_t k=0;k<previous.size()&&k<old_x.size();++k)
            if(!kept[k])ghosts.push_back({k<old_icons.size()?old_icons[k]:nullptr,previous[k].initial,old_x[k],clock_now});
    }
    // Every pointer-dependent visual tests its rectangle through hover(); the
    // recorded regions (in unscaled scene space) let mouse moves skip redraws
    // when the pointer stays over the same set of targets.
    std::vector<D2D1_RECT_F> hover_regions;
    float hover_cx=0,hover_by=0,hover_scale=1;
    bool hover_mapped=false;
    bool hover(D2D1_RECT_F r) {
        if(hover_suppressed)return false;
        auto raw=r;
        if(hover_mapped)raw=D2D1::RectF(hover_cx+(r.left-hover_cx)*hover_scale,hover_by+(r.top-hover_by)*hover_scale,hover_cx+(r.right-hover_cx)*hover_scale,hover_by+(r.bottom-hover_by)*hover_scale);
        hover_regions.push_back(raw);
        return inside(r,pointer_x,pointer_y);
    }
    std::uint64_t hover_signature(float x,float y) const {
        std::uint64_t hash=1469598103934665603ULL;
        for(size_t i=0;i<hover_regions.size();++i)if(inside(hover_regions[i],x,y)){hash^=i+1;hash*=1099511628211ULL;}
        return hash;
    }
    void launcher_symbol(float cx, float cy, float scale = 1) {
        D2D1_MATRIX_3X2_F previous;target->GetTransform(&previous);
        const auto size=theme::launcher_icon*scale;
        target->SetTransform(D2D1::Matrix3x2F::Scale(size/24,size/24)*
            D2D1::Matrix3x2F::Translation(cx-size/2,cy-size/2)*previous);
        color(pal.ink);target->FillGeometry(launcher_mark.get(),brush.get());
        target->SetTransform(previous);
    }
    void draw() {
        hover_regions.clear();hover_mapped=false;overlay_rects.clear();
        painter.palette=pal;painter.live=live_glass||(!desktop&&transparency&&!high_contrast);painter.boost=readability_boost;
        painter.high_contrast=high_contrast;painter.origin=base_transform;painter.begin_frame();
        if(transparency&&!high_contrast) cache_material();
        if(!external_draw)target->BeginDraw();
        // The desktop layer target reserves popup height; rasterize and clear
        // only the rows actually presented (scene size tracks the visible canvas).
        D2D1_MATRIX_3X2_F frame_transform;target->GetTransform(&frame_transform);
        if(desktop){target->SetTransform(base_transform);target->PushAxisAlignedClip(D2D1::RectF(0,0,width,height),D2D1_ANTIALIAS_MODE_ALIASED);target->SetTransform(frame_transform);}
        if(desktop) target->Clear(D2D1::ColorF(0,0,0,0));
        else {
        if(!high_contrast) background(target.get(),brush.get());else target->Clear(D2D1::ColorF(0,0,0));
        text(L"MINIMA / DOCK",D2D1::RectF(34,24,300,47),0,theme::secondary());
        text(L"A familiar, stable app row.",D2D1::RectF(32,54,width-32,108),4,theme::primary());
        text(L"24 DIP sword mark  ·  44 DIP button  ·  44 DIP glass pill",D2D1::RectF(34,110,width-32,138),1,theme::secondary());
        rect(D2D1::RectF(width-184,27,width-32,54),theme::rgb(0xffffff,.055f),13);
        text(L"DESIGN  /  02",D2D1::RectF(width-184,27,width-32,54),0,theme::secondary(),true);

        const size_t desktop_count = std::min<size_t>(inventory.desktop.size(),8);
        for (size_t i=0;i<desktop_count;++i) {
            const auto r=desktop_item(i); const bool over=hover(r);
            const float lift=over&&!reduced?3.0f:0.0f;
            const auto icon_rect=D2D1::RectF(r.left+24,r.top+12-lift,r.left+68,r.top+56-lift);
            if(over) rect(r,theme::rgb(0xffffff,.045f),14);
            if(i<desktop_icons.size()&&desktop_icons[i]&&desktop_icons[i]->bitmap) target->DrawBitmap(desktop_icons[i]->bitmap.get(),D2D1::RectF(r.left+29,r.top+15-lift,r.left+63,r.top+49-lift));
            else { rect(icon_rect,theme::rgb(inventory.desktop[i].folder?0xb5c6db:0x6987ac,.85f),11); text(L"↗",icon_rect,3,theme::primary(),true); }
            if(over) text(inventory.desktop[i].title,D2D1::RectF(r.left,r.top+58,r.right,r.bottom),0,theme::primary(),true);
        }
        if(desktop_count==0) text(L"Desktop shortcuts will appear here",D2D1::RectF(34,184,340,211),1,theme::secondary());
        text(L"HOVER TO EXPAND",D2D1::RectF(width-258,height-203,width-32,height-181),0,theme::secondary());
        text(L"Launcher first. Apps stay in order.",D2D1::RectF(width-258,height-178,width-32,height-150),1,theme::primary());
        text(L"F: fullscreen simulation  ·  R: reduced motion  ·  Tab / arrows: dock focus",D2D1::RectF(34,height-128,width-32,height-107),0,theme::secondary());
        }

        if (machine.visible()) {
            const auto r=dock(); const auto p=machine.progress();
            glass(r,bar?0.0f:(r.bottom-r.top)/2);
            const auto cy=layout().center_y;
            const bool launcher_over=hover(launcher_rect());
            ensure_motion();
            const float launcher_x=layout().launcher_x()+spread[0].value();
            if(launcher_over)
                rect(D2D1::RectF(launcher_x-18,cy-19,launcher_x+18,cy+19),pal.hover,16);
            bubble(launcher_x,cy,launcher_pulse);
            if(focus==0) line(launcher_x-4,cy+17,launcher_x+4,cy+17,pal.accent,1.5f);
            const float launcher_grow=magnify[0].value(),launcher_lift=rise[0].value();
            launcher_symbol(launcher_x,cy-launcher_lift,click_scale(launcher_pulse,pressed)*launcher_grow);
            if(launcher_lift>.5f){const float half=12*launcher_grow;overlay_rects.push_back(D2D1::RectF(launcher_x-half,cy-half-launcher_lift,launcher_x+half,cy+half-launcher_lift));}
            constexpr unsigned colors[]={0x758bbb,0x618e7f,0xb39b78,0x9290b9};
            // Clipped at the pill's sides only: hovered icons rise above its top edge.
            target->PushAxisAlignedClip(D2D1::RectF(r.left+3,r.top-24,r.right-3,r.bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const int active=active_slot();
            const auto attention_glow=[&](float x) {
                // Warm glow that pulses a few times, then stays until activated.
                const float phase=attention_pulsing()?.5f-.5f*std::cos(static_cast<float>(clock_now-attention_start)/800*6.2831853f):1.0f;
                color(theme::rgb(0xf0a030,.12f+.16f*phase));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,cy),17,17),brush.get());
            };
            for(const auto& g:ghosts) {
                const float t=std::clamp(static_cast<float>(clock_now-g.start)/vanish_time,0.0f,1.0f),half=12*(1-.45f*ease_out(t));
                const auto box=D2D1::RectF(g.x-half,cy-half,g.x+half,cy+half);
                if(g.icon&&g.icon->bitmap)target->DrawBitmap(g.icon->bitmap.get(),box,1-t);
                else {rect(box,theme::rgb(0x758bbb,1-t),7);text(g.initial,box,1,theme::rgb(0xf2f4fa,1-t),true);}
            }
            for(size_t i=0;i<shown_groups();++i) {
                // A newly opened app pops in: it fades up and springs to full size.
                float opacity=1,pop=1;
                if(groups[i].appeared>=0&&!reduced) {
                    const float t=static_cast<float>(clock_now-groups[i].appeared)/appear_time;
                    if(t<1){opacity=std::clamp(t*1.8f,0.0f,1.0f);pop=.45f+.55f*bezier(.3f,1.45f,.5f,1,std::max(0.0f,t));}
                }
                if(static_cast<int>(i)==dragging)continue; // drawn last, above the others
                const auto button=app_rect(i);const float aside=spread[i+1].value();const auto x=app_x(i)+shift[i+1].value()+aside;
                const bool over=hover(button);
                if(over&&dragging<0) rect(D2D1::RectF(button.left+aside,button.top,button.right+aside,button.bottom),pal.hover,16);
                if(wants_attention(i))attention_glow(x);
                bubble(x,cy,groups[i].pulse,opacity);
                const float grow=magnify[i+1].value();
                const float lift=rise[i+1].value();
                const float half=icon_px/2*grow*pop*click_scale(groups[i].pulse,groups[i].pressed);
                if(lift>.5f)overlay_rects.push_back(D2D1::RectF(x-half,cy-half-lift,x+half,cy+half-lift)); // Visible above the pill.
                float dx=96,dy=96;target->GetDpi(&dx,&dy);
                const auto snap_x=[&](float value){return std::round(value*dx/96)*96/dx;};
                const auto snap_y=[&](float value){return std::round(value*dy/96)*96/dy;};
                const auto icon_rect=D2D1::RectF(snap_x(x-half),snap_y(cy-half-lift),snap_x(x+half),snap_y(cy+half-lift));
                if(overflow(i)) text(L"···",icon_rect,2,fade(pal.ink,opacity),true);
                else if(grow>1.003f&&i<app_icons_large.size()&&app_icons_large[i]&&app_icons_large[i]->bitmap) target->DrawBitmap(app_icons_large[i]->bitmap.get(),D2D1::RectF(x-half,cy-half-lift,x+half,cy+half-lift),opacity);
                else if(i<app_icons.size()&&app_icons[i]&&app_icons[i]->bitmap) target->DrawBitmap(app_icons[i]->bitmap.get(),icon_rect,opacity);
                else {rect(icon_rect,theme::rgb(colors[i%4],opacity),7);text(groups[i].initial,icon_rect,1,theme::rgb(0xf2f4fa,opacity),true);}
                if(tile_running(i)&&static_cast<int>(i)!=active) {
                    // Neutral dot (warm while asking for attention), plus a second
                    // dot for several windows. The active app's pill is drawn below.
                    rect(D2D1::RectF(x-3,cy+16.5f,x+3,cy+19),wants_attention(i)?theme::rgb(0xf0a030):pal.running,1.25f);
                    if(groups[i].windows.size()>1)rect(D2D1::RectF(x+5,cy+16.5f,x+7.5f,cy+19),pal.ink_muted,1.25f);
                }
                if(focus==static_cast<int>(i)+1) outline(button,pal.accent,14,1.5f);
            }
            // Accent pill glides between apps as the foreground changes.
            if(active>=0&&indicator_shown&&active!=dragging)
                rect(D2D1::RectF(indicator_x-7,cy+16.5f,indicator_x+7,cy+19),pal.accent,1.25f);
            if(dragging>=0&&static_cast<size_t>(dragging)<shown_groups()) {
                const auto i=static_cast<size_t>(dragging);const float half=12*1.14f,x=drag_x,y=cy-4;
                color(fade(pal.shadow_color,.22f*pal.shadow));target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,cy+12),12,3),brush.get());
                if(i<app_icons_large.size()&&app_icons_large[i]&&app_icons_large[i]->bitmap)target->DrawBitmap(app_icons_large[i]->bitmap.get(),D2D1::RectF(x-half,y-half,x+half,y+half));
                else {rect(D2D1::RectF(x-half,y-half,x+half,y+half),theme::rgb(colors[i%4]),7);text(groups[i].initial,D2D1::RectF(x-half,y-half,x+half,y+half),1,theme::rgb(0xf2f4fa),true);}
            }
            if(p>.01f) {
                const auto music=media_rect(),system=system_rect();
                const auto quick_settings=quick_settings_rect();
                const bool tray_over=hover(D2D1::RectF(system.left,cy-19,system.left+24,cy+19));
                const bool settings_over=hover(quick_settings);
                if(tray_over)rect(D2D1::RectF(system.left-7,cy-19,system.left+29,cy+19),fade(pal.hover,p),16);
                if(settings_over)rect(quick_settings,fade(pal.hover,p),16);
                bubble(system.left+11,cy,tray_pulse,p);
                bubble((quick_settings.left+quick_settings.right)/2,cy,settings_pulse,p);
                const wchar_t* system_symbols[]={L"\xE70E",L"\xE701",volume_glyph()};
                constexpr float icon_offsets[]={0,27,52};
                for(size_t i=0;i<3;++i) {
                    const bool over=i==0?tray_over:settings_over;
                    const float x=system.left+icon_offsets[i]+11;
                    (void)over;const size_t wing=i==0?0:1;const float lift=wings[wing].value();
                    D2D1_MATRIX_3X2_F transform;target->GetTransform(&transform);
                    const auto button=i==0?tray_rect():quick_settings;
                    const auto scale=wing_scale(wing)*click_scale(i==0?tray_pulse:settings_pulse,control_pressed==static_cast<int>(shown_groups())+(i==0?1:2));
                    target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,D2D1::Point2F((button.left+button.right)/2,cy))*transform);
                    text(system_symbols[i],D2D1::RectF(x-11,cy-17-lift,x+11,cy+17-lift),5,fade(pal.ink,p),true);
                    target->SetTransform(transform);
                }
                if(show_media) {
                line(music.right+6,cy-12,music.right+6,cy+12,fade(pal.separator,p));
                measure_media_title();
                const auto title_area=D2D1::RectF(music.left,music.top,music.right,music.top+17);
                if(reduced)text(media_title_text,title_area,6,fade(pal.ink,p));
                else {target->PushAxisAlignedClip(title_area,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);color(fade(pal.ink,p));target->DrawTextLayout(D2D1::Point2F(music.left-media_marquee.offset(),music.top),media_title.get(),brush.get());target->PopAxisAlignedClip();}
                for(size_t i=0;i<3;++i) {
                    auto button=media_button(i);const bool enabled=media_enabled(i),over=enabled&&hover(button);
                    bubble((button.left+button.right)/2,(button.top+button.bottom)/2,media_pulses[i],p);
                    const auto lift=wings[3+i].value();button.top-=lift;button.bottom-=lift;
                    D2D1_MATRIX_3X2_F transform;target->GetTransform(&transform);
                    const auto scale=wing_scale(3+i)*click_scale(media_pulses[i],control_pressed==static_cast<int>(shown_groups())+5+static_cast<int>(i));
                    target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,D2D1::Point2F((button.left+button.right)/2,(button.top+button.bottom)/2))*transform);
                    media_symbol(i,button,fade(pal.ink,p*(enabled?(over||high_contrast?1.0f:.84f):(high_contrast?.50f:.30f))));
                    target->SetTransform(transform);
                    if(focus==static_cast<int>(shown_groups())+5+static_cast<int>(i))outline(media_button(i),pal.accent,8,1.5f);
                }
                }
                line(system.left-6,cy-12,system.left-6,cy+12,fade(pal.separator,p));
                const auto clock=clock_rect();const bool clock_over=hover(clock);
                if(clock_over)rect(clock,fade(pal.hover,p),16);
                bubble((clock.left+clock.right)/2,cy,clock_pulse,p);
                const float clock_lift=wings[2].value();
                D2D1_MATRIX_3X2_F clock_transform;target->GetTransform(&clock_transform);
                const auto clock_scale=click_scale(clock_pulse,control_pressed==static_cast<int>(shown_groups())+3);
                target->SetTransform(D2D1::Matrix3x2F::Scale(clock_scale,clock_scale,D2D1::Point2F((clock.left+clock.right)/2,cy))*clock_transform);
                // With prayer times: "11:00 PM · 14 Oct 26" over the next prayer; otherwise time over date.
                text(prayer_line.empty()||!show_date?clock_time:clock_time+L" \x00B7 "+clock_short,D2D1::RectF(clock.left-4,cy-17-clock_lift,clock.right+4,cy+1-clock_lift),6,fade(pal.ink,p),true);
                text(prayer_line.empty()?(show_date?clock_short:std::wstring()):prayer_line,D2D1::RectF(clock.left-4,cy+1-clock_lift,clock.right+4,cy+18-clock_lift),7,fade(pal.ink,p),true);
                target->SetTransform(clock_transform);
                if(focus==static_cast<int>(shown_groups())+1)outline(D2D1::RectF(system.left,system.top,system.left+23,system.bottom),pal.accent,10,1.5f);
                if(focus==static_cast<int>(shown_groups())+2)outline(quick_settings,pal.accent,14,1.5f);
                if(focus==static_cast<int>(shown_groups())+3)outline(clock,pal.accent,14,1.5f);
            }
            const auto show_desktop=desktop_button_rect();
            const bool desktop_over=p>.85f&&hover(show_desktop);
            const bool desktop_focused=p>.85f&&focus==static_cast<int>(shown_groups())+4;
            // A slim separator at the outer edge, like the native taskbar.
            // Hover brightens the edge instead of drawing a floating button box.
            if(p>.01f){
                const auto separator=show_desktop.left+3;
                bubble((show_desktop.left+show_desktop.right)/2,cy,desktop_pulse,p,(show_desktop.right-show_desktop.left)/36);
                const auto desktop_scale=click_scale(desktop_pulse,control_pressed==static_cast<int>(shown_groups())+4);
                line(separator,cy-13*desktop_scale,separator,cy+13*desktop_scale,
                    desktop_focused?pal.accent:fade(pal.ink,(desktop_over?1.0f:.4f)*p),desktop_over||desktop_focused?2.0f:1.0f);
            }
            target->PopAxisAlignedClip();
            if(desktop_over||desktop_focused) {
                const auto tooltip=D2D1::RectF(r.right-112,cy-52,r.right,cy-26);
                glass_tooltip(tooltip);
                text(L"Show desktop",tooltip,0,pal.ink,true);
            }
            // Name tooltip after the hover delay (running apps show previews instead).
            name_tooltip();
            const auto media_area=media_rect();
            if(media.available&&p>.85f)for(size_t i=0;i<3;++i)if(!media_enabled(i)&&hover(media_button(i))) {
                const wchar_t* unavailable[]={L"Previous track unavailable in this player",L"Playback control unavailable in this player",L"Next track unavailable in this player"};
                tooltip(unavailable[i],media_button(i));
            }
            if(media.available&&media.toggle&&p>.85f&&hover(media_button(1))) {
                const auto center=(media_button(1).left+media_button(1).right)/2;
                const auto tooltip=D2D1::RectF(center-28,cy-52,center+28,cy-26);
                glass_tooltip(tooltip);text(media.playing?L"Pause":L"Play",tooltip,6,pal.ink,true);
            }
            if(media.available&&!media.artist.empty()&&p>.85f&&hover(D2D1::RectF(media_area.left,media_area.top,media_area.right,media_area.top+17))) {
                tooltip(media.artist,D2D1::RectF(media_area.left,media_area.top,media_area.right,media_area.top+18));
            }
        } else if(!desktop) text(L"Overlay suppressed — press F to return",D2D1::RectF(0,height-100,width,height-50),2,pal.ink_secondary,true);

        const auto visible_panel=drawn_panel();
        if(visible_panel != Panel::None && visible_panel != Panel::Calendar && machine.visible()) {
            const auto r=panel_rect();
            const auto original_x=pointer_x,original_y=pointer_y,center=(r.left+r.right)/2;
            pointer_x=center+(pointer_x-center)/panel_reveal.scale();pointer_y=r.bottom+(pointer_y-r.bottom)/panel_reveal.scale();
            hover_mapped=true;hover_cx=center;hover_by=r.bottom;hover_scale=panel_reveal.scale();
            D2D1_MATRIX_3X2_F panel_transform;target->GetTransform(&panel_transform);
            target->SetTransform(D2D1::Matrix3x2F::Scale(panel_reveal.scale(),panel_reveal.scale(),D2D1::Point2F((r.left+r.right)/2,r.bottom))*panel_transform);
            const bool fading=panel_reveal.active();
            if(fading)target->PushLayer(D2D1::LayerParameters(
                D2D1::RectF(r.left-28,r.top-24,r.right+28,r.bottom+40),nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),panel_reveal.opacity()),panel_layer.get());
            painter.layer_opacity=fading?panel_reveal.opacity():1.0f;glass(r,22,true);painter.layer_opacity=1;
            if(visible_panel==Panel::Launcher) {
                draw_launcher(r);
            } else if(visible_panel==Panel::Settings) {
                draw_settings_pages(r);
            } else if(native_tray) {
                text(L"System tray",D2D1::RectF(r.left+18,r.top+8,r.right-18,r.top+36),1,pal.ink);
                const auto count=std::min<size_t>(30,tray_items.size()-std::min(tray_scroll,tray_items.size()));
                std::wstring label=tray_items.empty()?L"No tray icons":L"";
                for(size_t i=0;i<count;++i){const auto tile=tray_tile(i);const auto index=i+tray_scroll;const bool over=hover(tile);if(over){rect(D2D1::RectF(tile.left+4,tile.top+4,tile.right-4,tile.bottom-4),pal.hover,16);label=tray_items[index].title;}if(tray_focus==static_cast<int>(index)){outline(D2D1::RectF(tile.left+4,tile.top+4,tile.right-4,tile.bottom-4),pal.accent,16);if(!over)label=tray_items[index].title;}
                    const float lift=over&&!reduced?2:0;if(index<tray_icons.size()&&tray_icons[index]&&tray_icons[index]->bitmap)target->DrawBitmap(tray_icons[index]->bitmap.get(),D2D1::RectF(tile.left+12,tile.top+12-lift,tile.left+32,tile.top+32-lift));}
                if(label.empty()&&tray_items.size()>30)label=L"Scroll for more icons";
                text(label,D2D1::RectF(r.left+16,r.bottom-30,r.right-16,r.bottom-6),0,pal.ink_secondary,true);
            } else {
                text(L"System tray",D2D1::RectF(r.left+20,r.top+15,r.right-20,r.top+44),2,pal.ink);
                text(L"Windows tray is unavailable in this session.",D2D1::RectF(r.left+20,r.top+54,r.right-20,r.top+84),0,pal.ink_secondary);
                text(desktop?L"Revert to access the Windows tray.":L"Your Windows taskbar still hosts them.",D2D1::RectF(r.left+20,r.top+88,r.right-20,r.top+115),1,pal.ink);
                rect(D2D1::RectF(r.left+20,r.top+137,r.right-20,r.top+185),pal.tile,12);
                text(L"Open notification settings  ↗",D2D1::RectF(r.left+34,r.top+137,r.right-34,r.top+185),1,pal.ink);
                if(groups.size()>shown_groups()) {
                    rect(D2D1::RectF(r.left+20,r.top+198,r.right-20,r.top+246),pal.tile,12);
                    text(L"More applications  ↗",D2D1::RectF(r.left+34,r.top+198,r.right-34,r.top+246),1,pal.ink);
                }
            }
            if(fading)target->PopLayer();
            target->SetTransform(panel_transform);
            pointer_x=original_x;pointer_y=original_y;hover_mapped=false;
        }
        if(desktop)target->PopAxisAlignedClip();
    }
};

class Preview {
public:
    Scene scene;
    CalendarPopup calendar;
    MediaController media;
    TrayPopup tray;
    HWND calendar_previous=nullptr;
    DWORD panel_opened=0;
    unsigned popup_keys=0;
    bool popup_watching=false;
    ShellItemMenu* shell_menu=nullptr;
    AppContextMenu* open_menu=nullptr;std::wstring open_menu_path;UINT open_menu_shell=0;POINT open_menu_point{};
    HWND hwnd=nullptr, search=nullptr;
    ComPtr<ID2D1HwndRenderTarget> render;
    ComPtr<IWICBitmap> layer_bitmap;
    ComPtr<ID2D1RenderTarget> layer_render;
    HDC layer_dc=nullptr;
    HBITMAP layer_dib=nullptr;
    void* layer_pixels=nullptr;
    HGDIOBJ layer_previous=nullptr;
    UINT layer_width=0,layer_height=0;
    RECT pending_layer_bounds{};
    bool layer_bounds_pending=false,presenting_layer=false;
    UINT presented_height=0,layer_allocations=0,layer_frames=0,render_failures=0;
    LONG presented_center_y=0;
    bool layer_presented=false,inventory_loaded=false;
    std::jthread worker;
    std::mutex mutex;
    std::unique_ptr<Inventory> pending;
    bool busy=false,scan_full_pending=false;
    bool pending_full=false, rescan=false, tracking=false, animation_timer=false, reconcile_scheduled=false, reconcile_slow=false, media_timer=false;
    std::array<HWINEVENTHOOK,5> hooks{};
    DWORD location_process=0;
    DockVisibility visibility;
    HWND edge=nullptr,last_foreground=nullptr;
    int edge_indicator_alpha=-1;
    HDC edge_dc=nullptr;HBITMAP edge_dib=nullptr;HGDIOBJ edge_previous=nullptr;void* edge_pixels=nullptr;
    int edge_width=0,edge_height=0,edge_indicator=0;
    std::vector<float> edge_coverage;
    bool live_dock=false,auto_hidden=false,edge_tracking=false,reveal_hotkey=false;
    HFONT search_font=nullptr;
    HBRUSH edit_background=nullptr;
    float dpi=96;
    HRESULT desktop_action_result=E_PENDING;
    DesktopToggle desktop_toggle;
    unsigned desktop_activations=0;
    // Composition glass (live dock only). The window keeps its full flyout
    // height; a window region limits hit testing and drawing to what is shown.
    GlassHost glass;
    bool glass_mode=false,glass_bound=false;
    std::vector<RECT> region_rects;
    WindowPreviews previews;
    JumpList jump;
    struct JumpContext {
        Group group;std::wstring app_id;
        std::vector<JumpGroup> lists;std::vector<std::pair<size_t,size_t>> refs; // Entry id -> lists[group].items[item].
        std::vector<RecentItem> recent;ShellItemMenu shell;std::vector<AppContextMenu::WindowCommand> commands;
    };
    std::unique_ptr<JumpContext> jump_context;
    unsigned jump_session=0;
    VolumeControl volume;
    int previews_slot=-1,drag_candidate=-1;
    float drag_down_x=0;
    bool drag_moved=false;
    ULONGLONG previews_leave=0;
    UINT shell_hook=0;
    // Non-fatal diagnostics next to the executable (dock-error.txt is for fatal ones).
    void log_error(const char* where,HRESULT error) {
        wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
        std::ofstream(std::filesystem::path(path).parent_path()/L"dock-warnings.txt",std::ios::app)<<where<<" HRESULT="<<std::hex<<static_cast<unsigned long>(error)<<" glass="<<GlassHost::failure()<<'\n';
    }
    bool flyout_open() const {return scene.panel!=Panel::None||scene.exiting_panel!=Panel::None||calendar.window()!=nullptr||previews.visible();}
    static std::wstring group_key(const Group& g) {
        if(!g.app_id.empty())return L"app:"+g.app_id;
        if(!g.executable.empty())return L"exe:"+g.executable;
        if(!g.launch_path.empty())return L"path:"+g.launch_path;
        return g.windows.empty()?std::wstring{}:L"hwnd:"+std::to_wstring(reinterpret_cast<std::uintptr_t>(g.windows.front()));
    }
    // Screen rectangle of an app slot (accounts for the auto-hide slide).
    RECT slot_screen(size_t i) const {
        const auto r=scene.app_rect(i);const float scale=dpi/96,offset=live_dock?visibility.offset():0;
        POINT a{static_cast<LONG>(std::lround(r.left*scale)),static_cast<LONG>(std::lround((r.top+offset)*scale))},b{static_cast<LONG>(std::lround(r.right*scale)),static_cast<LONG>(std::lround((r.bottom+offset)*scale))};
        ClientToScreen(hwnd,&a);ClientToScreen(hwnd,&b);return {a.x,a.y,b.x,b.y};
    }
    void open_previews(size_t i) {
        if(jump.visible())return; // As in Windows: no previews over an open jump list.
        if(i>=scene.shown_groups()||scene.overflow(i)||scene.groups[i].windows.empty())return;
        const auto& g=scene.groups[i];
        POINT top{0,static_cast<LONG>(std::lround((scene.dock().top+(live_dock?visibility.offset():0))*dpi/96))};ClientToScreen(hwnd,&top);
        try {
            previews.open(hwnd,group_key(g),g.name,g.windows,g.icon,slot_screen(i),top.y,static_cast<UINT>(dpi),scene.pal,scene.transparency&&!scene.high_contrast,scene.reduced);
        } catch(HRESULT error) {
            log_error("previews",error);
            try{previews.close(false);}catch(...){}
            previews_slot=-1;scene.tooltip_target=static_cast<int>(i)+1;schedule();return;
        }
        previews_slot=static_cast<int>(i);previews_leave=0;scene.tooltip_target=-1;
        SetTimer(hwnd,previews_timer,50,nullptr);
        scene.machine.hold(Hold::Flyout,flyout_open(),now());schedule();
    }
    void close_previews(bool animated=true){if(previews.visible())previews.close(animated);}
    bool previews_supported() const{return scene.desktop&&GlassHost::available();}
    // Hover intent: the name shows after 150 ms and window previews after 450 ms.
    // Once a name or previews are showing, moving to another icon switches at once.
    int preview_pending=-1;
    void hover_changed(int slot) {
        const bool named=scene.tooltip_target>=0;
        scene.set_hovered(slot,now());scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);preview_pending=-1;
        if(slot<0||scene.dragging>=0){schedule();return;}
        if(slot>0&&previews.visible()) {
            const auto i=static_cast<size_t>(slot-1);
            if(i<scene.shown_groups()&&!scene.overflow(i)&&!scene.groups[i].windows.empty()){open_previews(i);return;}
            close_previews();
        }
        if(slot==0&&previews.visible())close_previews();
        if(named){scene.tooltip_target=slot;arm_previews(slot);schedule();return;}
        SetTimer(hwnd,hover_timer,150,nullptr);
    }
    void arm_previews(int slot) {
        const auto i=static_cast<size_t>(slot-1);
        if(slot>0&&previews_supported()&&i<scene.shown_groups()&&!scene.overflow(i)&&!scene.groups[i].windows.empty()){preview_pending=slot;SetTimer(hwnd,preview_delay_timer,300,nullptr);}
    }
    // ---- Launcher ----
    LauncherIndex launcher_index;
    bool caret_blinking=false;
    // ---- Customize ----
    DockSettings dock_settings=load_dock_settings();
    CustomizeWindow customize;
    // Full-width bar without auto-hide: maximized windows stop above it. The
    // trial supervisor owns the work area (it journals and restores it), so it
    // asks (WM_APP+120) and keeps Windows' own taskbar reservation meanwhile.
    bool bar_reserved=false;
    void reserve_bar(bool on) {
        if(!live_dock||on==bar_reserved)return;
        bar_reserved=on;
        if(const auto supervisor=FindWindowW(L"MinimaDock.Trial.Control",nullptr))PostMessageW(supervisor,WM_APP+25,0,0);
    }
    void apply_dock_settings(const DockSettings& s) {
        const bool layout_changed=s.media!=dock_settings.media||s.icon!=dock_settings.icon||s.style!=dock_settings.style;
        dock_settings=s;save_dock_settings(s);
        update_preferences();calendar.preferences(scene.reduced);
        scene.painter.palette=scene.pal;
        if(layout_changed){scene.ensure_motion();desktop_position();}
        reserve_bar(s.style==DockSettings::Style::Bar&&!s.bar_autohide);
        fullscreen();refresh_clock();schedule();
    }
    void open_customize() {
        dismiss_panel();
        CustomizeHooks hooks;
        hooks.version=app_version;
        hooks.dock_changed=[this](const DockSettings& s){apply_dock_settings(s);};
        hooks.prayer_changed=[this](const PrayerSettings& s,bool website){
            const auto& o=prayer_settings;
            const bool restart=website||s.source!=o.source||s.locate!=o.locate||s.method!=o.method||s.calculate!=o.calculate||
                               s.calc_method!=o.calc_method||s.asr!=o.asr||!s.same_location(o);
            prayer_settings=s;save_prayer_settings(s);
            if(restart)prayers.start(hwnd,prayer_settings);
            prayer_tick();schedule();
        };
        hooks.prayer_status=[this]{return prayers.snapshot();};
        hooks.check_prayers=[this]{prayers.refresh(true);};
        if(live_dock) {
            hooks.windows_key=[this]{return windows_key.enabled();};
            hooks.set_windows_key=[this](bool on){save_windows_key_setting(on);windows_key.enable(on);scene.windows_key_on=on;};
            hooks.return_to_windows=[this]{customize.close();return_to_windows();};
            hooks.start_with_windows=[]{return starts_with_windows();};
            hooks.set_start_with_windows=[](bool on){set_starts_with_windows(on);};
            hooks.update_status=[this]{return app_updater.status();};
            hooks.check_updates=[this]{app_updater.check();};
            hooks.install_update=[this]{start_update();};
            hooks.auto_update=[]{return load_auto_update_setting();};
            hooks.set_auto_update=[this](bool on){save_auto_update_setting(on);app_updater.set_automatic(on);};
        }
        hooks.clear_history=[this]{RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\MinimaDock\\Launcher\\Usage");scene.launch_usage.clear();};
        hooks.reset_everything=[this]{
            for(const auto key:{L"Software\\MinimaDock\\Settings",L"Software\\MinimaDock\\Pins",L"Software\\MinimaDock\\Launcher\\Usage",L"Software\\MinimaDock\\Prayer"})RegDeleteTreeW(HKEY_CURRENT_USER,key);
            prayer_settings=load_prayer_settings();prayers.start(hwnd,prayer_settings);
            apply_dock_settings(DockSettings{});scan(true);
        };
        customize.open(hwnd,dock_settings,prayer_settings,std::move(hooks),scene.reduced);
    }

    // Explorer's "Pin to taskbar" (desktop icons, File Explorer, Start) also pins to the dock.
    PinWatch pin_watch;
    std::vector<std::wstring> windows_pin_ids;
    static std::vector<std::wstring> pin_ids(const std::vector<Shortcut>& pins){std::vector<std::wstring> ids;for(const auto& p:pins)ids.push_back(pin_identity(p));return ids;}
    void windows_pins_changed() {
        const auto pins=windows_pins();const auto ids=pin_ids(pins);
        // Pinned again in Windows after "Unpin from dock": Windows' newer choice wins.
        for(const auto& id:ids)if(std::find(windows_pin_ids.begin(),windows_pin_ids.end(),id)==windows_pin_ids.end())forget_unpin(id);
        windows_pin_ids=ids;scan(true);
    }
    WindowsKey windows_key;
    // Windows key tapped alone: toggle the launcher, revealing an auto-hidden dock.
    bool launcher_over_fullscreen=false;
    void windows_key_pressed() {
        if(!live_dock)return;
        if(scene.panel==Panel::Launcher){dismiss_panel(true);fullscreen();return;}
        fullscreen();
        // Over a full-screen game or video the dock is suppressed: show it with the
        // launcher anyway; it hides again once the launcher closes.
        if(!scene.machine.visible()){launcher_over_fullscreen=true;scene.machine.suppress(false,now());}
        SetForegroundWindow(hwnd);SetFocus(hwnd);
        scene.machine.hold(Hold::Keyboard,true,now());
        if(scene.panel!=Panel::None)dismiss_panel();
        scene.launcher_pulse.start(now(),scene.reduced);
        set_panel(Panel::Launcher);schedule();
    }
    FileSearch file_search;
    // Runs the search again and asks the file thread for matching files.
    void research() {
        scene.search_launcher();scene.menu_open=false;
        const auto q=Scene::trimmed(scene.query);
        if(q.empty()||(q[0]!=L'>'&&q[0]!=L':'))file_search.search(q);
        request_preview();
    }
    void request_preview() {
        if(const auto* item=scene.selected())if(const auto* file=scene.file_of(*item))file_search.preview(file->path);
    }
    // Re-runs the search when files arrive, keeping the selected result.
    void files_arrived() {
        scene.launch_files=file_search.results();
        if(scene.panel!=Panel::Launcher)return;
        const auto* item=scene.selected();const auto key=item&&scene.launch_sel>0?item->key:std::wstring();
        scene.search_launcher();
        if(!key.empty())for(size_t i=0;i<scene.launch_found.size();++i)if(scene.launch_found[i].key==key){scene.launch_sel=static_cast<int>(i);scene.keep_visible();break;}
        request_preview();schedule();
    }
    void show_actions(bool open,const D2D1_POINT_2F* at=nullptr) {
        if(open){scene.build_menu();if(scene.menu_items.empty())return;}
        scene.menu_anchored=at!=nullptr;if(at)scene.menu_point=*at;
        scene.menu_open=open;scene.menu_sel=0;scene.menu_glide_placed=false;scene.menu_opened=now();schedule();
    }
    void run_verb(Scene::Verb verb) {
        const auto* chosen=scene.selected();if(!chosen)return;
        launching();
        const auto item=*chosen;scene.menu_open=false;
        using V=Scene::Verb;const auto path=scene.selected_path();
        const auto done=[&](bool ok,const wchar_t* message){if(ok)launcher_toast(message);else MessageBeep(MB_ICONWARNING);};
        switch(verb) {
        case V::Open:activate_launch(false);break;
        case V::Admin:activate_launch(true);break;
        case V::ShowInFolder:if(!path.empty()&&show_in_folder(path))set_panel(Panel::None);else MessageBeep(MB_ICONWARNING);break;
        case V::OpenWith:if(!path.empty()){set_panel(Panel::None);open_with(hwnd,path);}break;
        case V::Properties:if(!path.empty()){set_panel(Panel::None);show_properties(hwnd,path);}break;
        case V::Terminal:if(!path.empty()){set_panel(Panel::None);open_terminal(path);}break;
        case V::CopyPath:done(!path.empty()&&copy_text(hwnd,path),L"Path copied");break;
        case V::CopyFile:done(!path.empty()&&copy_file(hwnd,path),L"File copied \x2014 paste it in File Explorer");break;
        case V::CopyName:done(copy_text(hwnd,item.name),L"Name copied");break;
        case V::CopyLink:if(item.kind==Scene::LaunchResult::Kind::Setting)done(copy_text(hwnd,settings_pages()[item.index].uri),L"Link copied");break;
        case V::CopyExpression:done(copy_text(hwnd,Scene::trimmed(scene.query)+L" = "+item.name),L"Copied");break;
        case V::CopyValue:done(copy_text(hwnd,item.value),L"Copied");break;
        case V::PinDock:case V::UnpinDock:
            if(const auto pin=scene.launcher_pin(item)) {
                const auto* existing=scene.pinned_match(*pin);
                const bool pinning=verb==V::PinDock;
                // Unpin the dock's own record of the pin so the identities match.
                const Shortcut record=existing&&!pinning?*existing:*pin;
                done(save_pin_preference(record,pinning),pinning?L"Pinned to dock":L"Unpinned from dock");
                scan(true);
            }
            break;
        case V::SnapLeft:case V::SnapRight:case V::Maximize:case V::Minimize:case V::CloseWindow: {
            const auto how=verb==V::SnapLeft?Arrange::Left:verb==V::SnapRight?Arrange::Right:verb==V::Maximize?Arrange::Maximize:verb==V::Minimize?Arrange::Minimize:Arrange::Close;
            set_panel(Panel::None);
            if(!arrange_window(item.window,how))MessageBeep(MB_ICONWARNING);
            break;
        }
        }
        schedule();
    }
    void launcher_opened() {
        scene.query.clear();scene.launch_usage=load_usage();launcher_index.refresh();scene.launch_apps=launcher_index.apps();
        scene.power_holding=scene.power_pending=-1;scene.launch_toast.clear();scene.launch_files=file_search.results();research();scene.launch_opened=now();
    }
    void launcher_closed() {
        scene.power_holding=scene.power_pending=-1;scene.query.clear();scene.menu_open=false;scene.app_bitmaps.clear(); // A countdown never runs unseen.
        // Opening a result closes the launcher without Escape's dismiss path: end
        // the keyboard hold here too, or the dock stays up over maximized and
        // full-screen windows while it keeps focus.
        scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
    }
    // Lets the app a launcher result opens take the foreground (an already
    // running browser opening a tab otherwise may not), and gives focus back if
    // the dock still has it a moment later.
    void launching() {
        AllowSetForegroundWindow(ASFW_ANY);
        SetTimer(hwnd,return_focus_timer,600,nullptr);
    }
    static float item_step(){return Scene::item_height;}
    void launcher_toast(std::wstring message){scene.launch_toast=std::move(message);scene.toast_since=now();schedule();}
    void move_selection(int delta) {
        if(scene.launch_found.empty())return;
        const int count=static_cast<int>(scene.launch_found.size());
        scene.launch_sel=(scene.launch_sel+delta+count)%count;scene.keep_visible();request_preview();schedule();
    }
    void perform_power(size_t which) {
        set_panel(Panel::None);
        bool ok=false;
        switch(which){case 0:ok=power_lock();break;case 1:ok=power_sleep();break;case 2:ok=power_restart();break;case 3:ok=power_shutdown();break;default:ok=power_sign_out();break;}
        if(!ok)MessageBoxW(hwnd,L"Windows did not allow this power action.",L"MinimaDock",MB_OK|MB_ICONERROR);
    }
    void start_power(size_t which){scene.power_pending=static_cast<int>(which);scene.power_deadline=now()+Scene::countdown_time;schedule();}
    void activate_launch(bool elevated) {
        if(scene.launch_found.empty())return;
        launching();
        const auto item=scene.launch_found[static_cast<size_t>(std::clamp(scene.launch_sel,0,static_cast<int>(scene.launch_found.size())-1))];
        using Kind=Scene::LaunchResult::Kind;
        if(item.kind==Kind::Answer){if(copy_text(hwnd,item.name))launcher_toast(L"Copied "+item.name);return;}
        if(item.kind==Kind::Window) {
            set_panel(Panel::None);
            if(!activate_window(item.window))MessageBeep(MB_ICONWARNING);
            return;
        }
        if(item.kind==Kind::Command) {
            if(run_command(item.value,elevated))set_panel(Panel::None);else MessageBoxW(hwnd,L"Windows could not start PowerShell.",L"MinimaDock",MB_OK|MB_ICONERROR);
            return;
        }
        if(item.kind==Kind::Link){if(open_url(item.value))set_panel(Panel::None);else MessageBeep(MB_ICONWARNING);return;}
        if(!item.key.empty())record_use(item.key);
        if(item.kind==Kind::Emoji){if(copy_text(hwnd,item.value))launcher_toast(L"Copied "+item.value);return;}
        if(item.kind==Kind::App) {
            const auto apps=scene.launch_apps;
            if(item.index<apps->size()&&launch_app_id(hwnd,(*apps)[item.index].id,elevated)){set_panel(Panel::None);return;}
            MessageBoxW(hwnd,L"Windows could not open this app.",L"MinimaDock",MB_OK|MB_ICONERROR);return;
        }
        if(item.kind==Kind::Setting){launch(settings_pages()[item.index].uri);set_panel(Panel::None);return;}
        if(const auto* file=scene.file_of(item)) {
            const auto path=file->path;
            const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,elevated&&!file->folder?L"runas":nullptr,path.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
            if(result>32||(elevated&&result==SE_ERR_ACCESSDENIED))set_panel(Panel::None);else MessageBoxW(hwnd,L"Windows could not open this file.",L"MinimaDock",MB_OK|MB_ICONERROR);
            return;
        }
        using Action=Scene::LaunchAction;
        switch(static_cast<Action>(item.index)) {
        case Action::WifiOn:case Action::WifiOff:connectivity.set_wifi(static_cast<Action>(item.index)==Action::WifiOn);launcher_toast(item.name);break;
        case Action::BluetoothOn:case Action::BluetoothOff:connectivity.set_bluetooth(static_cast<Action>(item.index)==Action::BluetoothOn);launcher_toast(item.name);break;
        case Action::MicMute:case Action::MicUnmute:volume.set_mic_muted(static_cast<Action>(item.index)==Action::MicMute);read_volume();launcher_toast(item.name);break;
        case Action::Capture:set_panel(Panel::None);instant_capture();break;
        case Action::Timer:timer_start(static_cast<std::int64_t>(item.minutes)*60000);launcher_toast(L"Timer started: "+std::to_wstring(item.minutes)+L" min");break;
        case Action::Dark:case Action::Light:set_dark_mode(static_cast<Action>(item.index)==Action::Dark);launcher_toast(item.name);break;
        case Action::Lock:perform_power(0);break;
        case Action::Sleep:start_power(1);break;
        case Action::Restart:start_power(2);break;
        case Action::Shutdown:start_power(3);break;
        case Action::SignOut:start_power(4);break;
        case Action::StartMenu:set_panel(Panel::None);open_start_menu();break;
        case Action::Customize:open_customize();break;
        case Action::WinKeyStart:case Action::WinKeyDock: {
            const bool on=static_cast<Action>(item.index)==Action::WinKeyDock;
            save_windows_key_setting(on);windows_key.enable(on);scene.windows_key_on=on&&live_dock;
            launcher_toast(on?L"Windows key opens MinimaDock":L"Windows key opens Start");research();
            break;
        }
        }
    }
    void launcher_context(D2D1_POINT_2F p) {
        if(scene.menu_open&&inside(scene.menu_rect(),p.x,p.y))return;
        const auto area=scene.launch_list();
        if(inside(area,p.x,p.y))for(const auto& row:scene.launch_rows) {
            if(row.header)continue;
            const float top=area.top+row.top-scene.launch_scroll,bottom=area.top+row.bottom-scene.launch_scroll;
            if(p.y>=top&&p.y<bottom){scene.launch_sel=static_cast<int>(row.result);request_preview();show_actions(true,&p);return;}
        }
        if(scene.menu_open)show_actions(false);
    }
    void launcher_click(float x,float y) {
        if(scene.menu_open) {
            for(size_t i=0;i<scene.menu_items.size();++i)if(inside(scene.menu_row(i),x,y)){run_verb(scene.menu_items[i].verb);return;}
            if(!inside(scene.menu_rect(),x,y))show_actions(false);
            return;
        }
        if(scene.power_pending>=0&&inside(scene.countdown_cancel(),x,y)){scene.power_pending=-1;launcher_toast(L"Cancelled");return;}
        if(inside(scene.power_button(0),x,y)&&scene.power_pending<0){perform_power(0);return;}
        const auto area=scene.launch_list();if(!inside(area,x,y))return;
        for(const auto& row:scene.launch_rows) {
            if(row.header)continue;
            const float top=area.top+row.top-scene.launch_scroll,bottom=area.top+row.bottom-scene.launch_scroll;
            if(y>=top&&y<bottom){scene.launch_sel=static_cast<int>(row.result);request_preview();activate_launch((GetKeyState(VK_SHIFT)&0x8000)!=0);return;}
        }
    }
    void paste_query() {
        if(!OpenClipboard(hwnd))return;
        if(const auto data=GetClipboardData(CF_UNICODETEXT))if(const auto text=static_cast<const wchar_t*>(GlobalLock(data))) {
            for(const wchar_t* c=text;*c&&*c!=L'\r'&&*c!=L'\n'&&scene.query.size()<200;++c)scene.query.push_back(*c==L'\t'?L' ':*c);
            GlobalUnlock(data);
        }
        CloseClipboard();
    }
    // ---- Prayer times (masjid website) ----
    PrayerService prayers;
    AppUpdater app_updater; // MinimaDock's own updates (live dock only).
    PrayerSettings prayer_settings;
    std::vector<int> prayer_fired;
    int prayer_fired_day=0;
    static std::wstring prayer_clock(int minutes,bool marker) {
        if(minutes<0)return L"\x2014";
        SYSTEMTIME t{};t.wHour=static_cast<WORD>((minutes/60)%24);t.wMinute=static_cast<WORD>(minutes%60);
        wchar_t text[32]{};GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS|(marker?0:TIME_NOTIMEMARKER),&t,nullptr,text,32);return text;
    }
    PrayerView prayer_view(const PrayerSnapshot& snap,int now,bool friday,bool today) const {
        PrayerView view;view.visible=snap.have;
        if(prayer_settings.source==PrayerSource::Website)view.website=prayer_settings.website;
        if(!snap.have)return view;
        if(!today){SYSTEMTIME d{};d.wYear=static_cast<WORD>(snap.day.year);d.wMonth=static_cast<WORD>(snap.day.month);d.wDay=static_cast<WORD>(snap.day.day);wchar_t text[64]{};GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,0,&d,L"MMM d",text,64,nullptr);view.status=std::wstring(L"Times from ")+text;}
        else if(prayer_settings.source==PrayerSource::Location)view.status=prayer_settings.place.empty()?L"Calculated":prayer_settings.place; // Calculated times have no "updated".
        else view.status=snap.error.empty()?L"Updated "+snap.updated:L"Offline \x00B7 "+snap.updated;
        const auto next=next_prayer(snap.day,now,friday);const auto order=daily_salah(snap.day,friday);
        const Salah rows[]={order[0],Salah::Sunrise,order[1],order[2],order[3],order[4]};
        bool reached=false;
        for(const auto salah:rows) {
            const auto& t=snap.day[salah];PrayerRow row;row.name=salah_name(salah);
            if(salah==Salah::Sunrise){row.adhan=prayer_clock(t.begin,true);row.iqama=L"\x2014";row.state=now>=t.begin?0:2;view.rows.push_back(row);continue;}
            row.adhan=prayer_clock(t.adhan,true);row.iqama=prayer_clock(t.iqama,true);
            if(!next.tomorrow&&salah==next.salah){row.state=1;reached=true;}else row.state=reached?2:0;
            view.rows.push_back(row);
        }
        return view;
    }
    void prayer_tick() {
        prayers.refresh(false);
        const auto snap=prayers.snapshot();
        SYSTEMTIME t{};GetLocalTime(&t);
        const int now=t.wHour*60+t.wMinute,date=t.wYear*10000+t.wMonth*100+t.wDay;const bool friday=t.wDayOfWeek==5;
        const bool today=snap.have&&snap.day.year==t.wYear&&snap.day.month==t.wMonth&&snap.day.day==t.wDay;
        std::wstring line;
        if(snap.have&&prayer_settings.clock) {
            const auto next=next_prayer(snap.day,now,friday);const std::wstring name=salah_name(next.salah);
            if(next.waiting)line=L"Iqama "+prayer_clock(next.time.iqama,false);
            else {const int until=next.time.adhan-now+(next.tomorrow?1440:0);line=until<=15?name+L" in "+std::to_wstring(until)+L" min":name+L" "+prayer_clock(next.time.adhan,false);}
        }
        if(line!=scene.prayer_line){scene.prayer_line=line;schedule();}
        // Each reminder once a day, and only within two minutes (not after sleep).
        if(date!=prayer_fired_day){prayer_fired.clear();prayer_fired_day=date;}
        if(snap.have&&prayer_settings.reminders)
            for(const auto& event:prayer_events(snap.day,friday,prayer_settings.iqama_lead)) {
                if(now<event.minute||now>event.minute+2||std::find(prayer_fired.begin(),prayer_fired.end(),event.key())!=prayer_fired.end())continue;
                if(event.kind==PrayerEvent::Kind::Sunrise&&!prayer_settings.sunrise)continue;
                prayer_fired.push_back(event.key());notify_prayer(event,snap.day);
            }
        calendar.prayers(prayer_view(snap,now,friday,today));
    }
    // A small card at the top centre of the screen in use, over games too, with a
    // live countdown to Iqama; it shrinks to a pill and never takes focus.
    PrayerToast prayer_toast;
    void notify_prayer(const PrayerEvent& event,const PrayerDay& day) {
        using Kind=PrayerEvent::Kind;const auto& t=day[event.salah];const std::wstring name=salah_name(event.salah);
        // The Adhan card already counts down to Iqama; the earlier reminder only
        // returns if that card was dismissed.
        if(event.kind==Kind::Iqama&&prayer_toast.visible())return;
        SYSTEMTIME local{};GetLocalTime(&local);
        const long long seconds=local.wHour*3600LL+local.wMinute*60LL+local.wSecond;const auto tick=GetTickCount64();
        const auto at=[&](int minute)->ULONGLONG{const long long delta=minute*60LL-seconds;return delta>0?tick+static_cast<ULONGLONG>(delta)*1000:tick;};
        PrayerToastInfo info;info.start_tick=tick;info.pill=name;
        if(event.kind==Kind::Sunrise) {
            const int sunrise=day[Salah::Sunrise].begin;
            info.title=L"Fajr ends at sunrise";info.subtitle=L"Sunrise at "+prayer_clock(sunrise,true);info.pill=L"Sunrise";info.done=L"Sunrise";info.end_tick=at(sunrise);
        } else {
            info.title=name+(event.kind==Kind::Adhan?L" \x00B7 Adhan":L" \x00B7 Iqama soon");
            info.done=L"Iqama now";
            if(t.iqama>=0){info.subtitle=L"Iqama at "+prayer_clock(t.iqama,true);info.end_tick=at(t.iqama);info.start_tick=event.kind==Kind::Adhan?tick:at(t.adhan);}
            else info.subtitle=L"Adhan at "+prayer_clock(t.adhan,true);
        }
        info.chime=prayer_settings.chime!=2&&scene.timer_state!=Scene::TimerState::Ringing; // Never cut off the timer's alarm.
        info.sound=prayer_settings.chime;
        prayer_toast.show(info,scene.pal,scene.reduced);
    }
    // ---- Quick settings ----
    Connectivity connectivity;
    UpdateMonitor updates;
    Brightness brightness;
    bool airplane_wifi=false,airplane_bluetooth=false; // Radios Airplane mode turned off, to restore.
    std::vector<WORD> pending_keys;                    // Shortcut sent once the panel has let go of focus.
    JumpList alarm_card{alarm_event_message};
    unsigned alarm_session=0;
    bool updates_stale=false;
    static bool overlay_running() {
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
        for(bool more=Process32FirstW(snapshot,&entry)!=FALSE;more&&!found;more=Process32NextW(snapshot,&entry)!=FALSE)
            found=_wcsicmp(entry.szExeFile,L"NVIDIA Overlay.exe")==0||_wcsicmp(entry.szExeFile,L"NVIDIA Share.exe")==0;
        CloseHandle(snapshot);return found;
    }
    void clear_password() {
        if(!scene.password.empty())SecureZeroMemory(scene.password.data(),scene.password.size()*sizeof(wchar_t));
        scene.password.clear();scene.password_visible=false;
    }
    // Cheap reads (registry, process list, Win32 queries), so the new tiles open
    // showing the truth. The keyboard layout is the app's from before the panel.
    void read_quick_states() {
        scene.display=display_state();scene.dark=dark_mode();scene.captions_on=process_running(L"LiveCaptions.exe");
        scene.keyboard_count=keyboard_layouts();scene.keyboard_label=keyboard_label(last_foreground);
        scene.energy_on=energy_saver_on();scene.vpn_on=vpn_connected();scene.rotation_sensor=has_rotation_sensor();
        scene.brightness_available=brightness.available();if(!scene.brightness_active)scene.brightness_level=brightness.level();
    }
    void settings_opened() {
        scene.settings_page=Scene::SettingsPage::Main;scene.expanded.clear();scene.list_scroll=0;clear_password();
        scene.net=connectivity.snapshot();scene.updates=updates.status();
        connectivity.refresh(true);updates.refresh(updates_stale);updates_stale=false;
        scene.overlay_running=overlay_running();read_volume();
        read_quick_states();brightness.refresh();
        scene.quick=load_quick_layout();scene.quick_page=0;scene.quick_edit=false;scene.quick_drag=-1;scene.swipe_dx=0;
        scene.tune_motion();scene.quick_slide.snap(0);
        SetTimer(hwnd,settings_timer,3000,nullptr);
    }
    void settings_closed() {
        KillTimer(hwnd,settings_timer);scene.settings_page=Scene::SettingsPage::Main;scene.expanded.clear();clear_password();
        // Closing mid-drag keeps the new order (each change is already saved otherwise).
        if(scene.quick_drag>=0)save_quick_layout(scene.quick);
        scene.quick_drag=-1;scene.quick_edit=false;scene.swipe_dx=0;quick_press=quick_swiping=false;
    }
    // ---- Quick settings pages and edit mode ----
    bool quick_press=false,quick_swiping=false; // Button down over the tiles; dragging sideways to page.
    D2D1_POINT_2F quick_down{};int quick_down_slot=-1;
    QuickLayout drag_before;                     // Order to restore when a tile drag is cancelled.
    int wheel_pages=0;std::int64_t wheel_paged=0; // Wheel and touchpad deltas build up to one page.
    void show_quick_page(long page) {
        const long last=static_cast<long>(scene.quick.pages())-1;
        scene.quick_page=static_cast<size_t>(std::clamp(page,0L,last));scene.tune_motion();
        scene.quick_slide.target(static_cast<float>(scene.quick_page),now(),scene.reduced);schedule();
    }
    void quick_changed(const QuickLayout& before) {
        scene.reflow(before,now());save_quick_layout(scene.quick);
        if(scene.quick_page>=scene.quick.pages())show_quick_page(static_cast<long>(scene.quick.pages())-1);
        schedule();
    }
    void set_quick_edit(bool on) {
        if(!on&&scene.quick_drag>=0)drop_tile(true);
        scene.quick_edit=on;scene.panel_hover=-1;schedule();
    }
    void wheel_page(int delta) { // Positive: next page.
        if((wheel_pages>0)!=(delta>0))wheel_pages=0;
        wheel_pages+=delta;
        if(std::abs(wheel_pages)<WHEEL_DELTA||now()-wheel_paged<250)return;
        show_quick_page(static_cast<long>(scene.quick_page)+(wheel_pages>0?1:-1));wheel_pages=0;wheel_paged=now();
    }
    // Ends a tile drag; the tile springs from where it was let go into its slot.
    void drop_tile(bool keep) {
        scene.quick_drag=-1;
        if(!keep){const auto moved=scene.quick;scene.quick=drag_before;scene.reflow(moved,now());}
        const auto it=std::find(scene.quick.tiles.begin(),scene.quick.tiles.end(),dragged);
        if(it!=scene.quick.tiles.end()) {
            const auto pos=static_cast<size_t>(it-scene.quick.tiles.begin());
            if(pos/tiles_per_page!=scene.quick_page)show_quick_page(static_cast<long>(pos/tiles_per_page));
            const auto home=scene.settings_tile(pos%tiles_per_page);
            auto& dx=scene.tile_dx[Scene::ti(dragged)];auto& dy=scene.tile_dy[Scene::ti(dragged)];
            dx.snap(scene.drag_at.x-scene.drag_grab.x-home.left);dy.snap(scene.drag_at.y-scene.drag_grab.y-home.top);
            dx.target(0,now(),scene.reduced);dy.target(0,now(),scene.reduced);
        }
        if(keep)save_quick_layout(scene.quick);
        schedule();
    }
    Tile dragged=Tile::Count;
    // Pointer moves with the button down over the tiles: reorder (edit mode) or page.
    bool quick_move(D2D1_POINT_2F point) {
        if(!quick_press)return false;
        const float dx=point.x-quick_down.x,dy=point.y-quick_down.y;
        if(scene.quick_drag<0&&!quick_swiping) {
            if(std::hypot(dx,dy)<=6)return false;
            for(auto& spring:scene.tile_press)spring.target(1,now(),scene.reduced);
            const size_t k=scene.quick_page*tiles_per_page+static_cast<size_t>(std::max(quick_down_slot,0));
            if(scene.quick_edit&&quick_down_slot>=0&&k<scene.quick.tiles.size()) {
                const auto home=scene.settings_tile(static_cast<size_t>(quick_down_slot));
                drag_before=scene.quick;dragged=scene.quick.tiles[k];scene.quick_drag=static_cast<int>(k);
                scene.drag_grab=D2D1::Point2F(quick_down.x-home.left,quick_down.y-home.top);
                scene.tile_dx[Scene::ti(dragged)].snap(0);scene.tile_dy[Scene::ti(dragged)].snap(0);
            } else if(!scene.quick_edit&&scene.quick.pages()>1&&std::abs(dx)>std::abs(dy))quick_swiping=true;
            else {quick_press=false;return false;}
        }
        if(quick_swiping) { // Rubber band past the first and last page.
            const bool edge=(dx>0&&scene.quick_page==0)||(dx<0&&scene.quick_page+1>=scene.quick.pages());
            scene.swipe_dx=edge?dx*.3f:dx;schedule();return true;
        }
        scene.drag_at=point;
        // Hovering a page dot moves the drag to that page.
        for(size_t p=0;p<scene.quick.pages();++p)if(p!=scene.quick_page&&inside(scene.quick_dot(p),point.x,point.y))show_quick_page(static_cast<long>(p));
        if(inside(scene.quick_area(),point.x,point.y)) {
            const auto slot=scene.quick_slot_at(point.x-scene.drag_grab.x+50,point.y-scene.drag_grab.y+24);
            const size_t to=std::min(scene.quick_page*tiles_per_page+slot,scene.quick.tiles.size()-1);
            if(to!=static_cast<size_t>(scene.quick_drag)) {
                const auto before=scene.quick;scene.quick.move(static_cast<size_t>(scene.quick_drag),to);scene.quick_drag=static_cast<int>(to);scene.reflow(before,now());
            }
        }
        schedule();return true;
    }
    // Button released: finish a drag or swipe. False when it was a plain click.
    bool quick_release() {
        const bool was=quick_press;quick_press=false;
        if(!was)return false;
        if(scene.quick_drag>=0){drop_tile(true);return true;}
        if(!quick_swiping)return false;
        quick_swiping=false;const float dx=scene.swipe_dx;scene.swipe_dx=0;
        scene.quick_slide.snap(scene.quick_slide.value()-dx/scene.page_width());
        show_quick_page(static_cast<long>(scene.quick_page)+(dx<-40?1:dx>40?-1:0));
        return true;
    }
    void quick_cancel() {
        if(scene.quick_drag>=0)drop_tile(false);
        if(quick_swiping){quick_swiping=false;scene.quick_slide.snap(scene.quick_slide.value()-scene.swipe_dx/scene.page_width());scene.swipe_dx=0;show_quick_page(static_cast<long>(scene.quick_page));}
        quick_press=false;
    }
    // Clicks on Quick settings' main page that belong to paging or edit mode.
    bool quick_click(float x,float y) {
        if(scene.quick.pages()>1)for(size_t p=0;p<scene.quick.pages();++p)if(inside(scene.quick_dot(p),x,y)){show_quick_page(static_cast<long>(p));return true;}
        if(scene.quick.tiles.empty()&&inside(scene.quick_empty_button(),x,y)){set_quick_edit(true);open_page(Scene::SettingsPage::AddTiles);return true;}
        if(!scene.quick_edit) {
            if(inside(scene.quick_edit_button(),x,y)){set_quick_edit(true);return true;}
            return false;
        }
        for(size_t i=0;i<tiles_per_page;++i)if(scene.slot_tile(i)!=Tile::Count&&inside(scene.tile_remove(i),x,y)) {
            const auto before=scene.quick;scene.quick.remove(scene.quick_page*tiles_per_page+i);quick_changed(before);return true;
        }
        if(inside(scene.quick_add_button(),x,y)){if(!scene.quick.full())open_page(Scene::SettingsPage::AddTiles);return true;}
        if(inside(scene.quick_done_button(),x,y)){set_quick_edit(false);return true;}
        // Tiles do nothing while editing; the volume row still works.
        for(size_t i=0;i<tiles_per_page;++i)if(scene.slot_tile(i)!=Tile::Count&&inside(scene.settings_tile(i),x,y))return true;
        return y>=scene.panel_rect().bottom-40;
    }
    void add_page_click(float x,float y) {
        using Page=Scene::SettingsPage;
        if(inside(scene.page_footer_link(),x,y)) {
            const auto before=scene.quick;scene.quick=QuickLayout::defaults();scene.start_page(Page::Main,now());quick_changed(before);show_quick_page(0);return;
        }
        if(!inside(scene.page_list(),x,y)||scene.quick.full())return;
        const auto hidden=scene.addable();
        for(size_t i=0;i<hidden.size();++i)if(inside(scene.add_cell(i),x,y)) {
            const auto before=scene.quick;scene.quick.add(hidden[i]);scene.start_page(Page::Main,now());quick_changed(before);
            show_quick_page(static_cast<long>(scene.quick.pages())-1);return;
        }
    }
    void open_page(Scene::SettingsPage page) {
        scene.start_page(page,now());scene.expanded.clear();scene.list_scroll=0;clear_password();
        if(page==Scene::SettingsPage::Wifi){connectivity.scan();connectivity.refresh(true);}
        else if(page==Scene::SettingsPage::Bluetooth)connectivity.refresh(true);
        schedule();
    }
    // Instant capture: NVIDIA's "save the last minutes" hotkey (Alt+F10), sent
    // once focus is back on the game.
    // Live captions (Win+Ctrl+L) and Cast (Win+K) are Windows shortcuts too, sent
    // the same way so they reach the shell rather than our panel.
    void send_shortcut(std::vector<WORD> codes){pending_keys=std::move(codes);dismiss_panel(true);SetTimer(hwnd,capture_timer,180,nullptr);}
    void instant_capture(){send_shortcut({VK_MENU,VK_F10});}
    void send_capture_keys() {
        KillTimer(hwnd,capture_timer);if(pending_keys.empty())return;
        // Press in order, release in reverse.
        std::vector<INPUT> keys(pending_keys.size()*2);
        for(size_t i=0;i<keys.size();++i) {
            const bool up=i>=pending_keys.size();const WORD code=up?pending_keys[keys.size()-1-i]:pending_keys[i];
            keys[i].type=INPUT_KEYBOARD;keys[i].ki.wVk=code;keys[i].ki.wScan=static_cast<WORD>(MapVirtualKeyW(code,MAPVK_VK_TO_VSC));
            keys[i].ki.dwFlags=(up?KEYEVENTF_KEYUP:0)|(code==VK_LWIN?KEYEVENTF_EXTENDEDKEY:0);
        }
        const bool capture=pending_keys==std::vector<WORD>{VK_MENU,VK_F10};pending_keys.clear();
        SendInput(static_cast<UINT>(keys.size()),keys.data(),sizeof(INPUT));
        if(capture)scene.capture_sent=GetTickCount64();
        schedule();
    }
    void timer_start(std::int64_t ms) {
        stop_alarm();scene.timer_duration=ms;scene.timer_end=GetTickCount64()+static_cast<ULONGLONG>(ms);
        scene.timer_state=Scene::TimerState::Running;SetTimer(hwnd,countdown_timer,250,nullptr);schedule();
    }
    void timer_pause(){scene.timer_left=scene.timer_remaining();scene.timer_state=Scene::TimerState::Paused;KillTimer(hwnd,countdown_timer);schedule();}
    void timer_resume(){scene.timer_end=GetTickCount64()+static_cast<ULONGLONG>(scene.timer_left);scene.timer_state=Scene::TimerState::Running;SetTimer(hwnd,countdown_timer,250,nullptr);schedule();}
    void timer_reset(){KillTimer(hwnd,countdown_timer);stop_alarm();scene.timer_state=Scene::TimerState::Idle;schedule();}
    void timer_primary() {
        using State=Scene::TimerState;
        switch(scene.timer_state){case State::Idle:timer_start(scene.timer_duration);break;case State::Running:timer_pause();break;case State::Paused:timer_resume();break;default:timer_reset();break;}
    }
    void timer_tick() {
        if(scene.timer_state!=Scene::TimerState::Running){KillTimer(hwnd,countdown_timer);return;}
        if(scene.timer_remaining()<=0)ring();else if(scene.panel==Panel::Settings)schedule();
    }
    void stop_alarm(){PlaySoundW(nullptr,nullptr,0);KillTimer(hwnd,alarm_timer);alarm_card.close();}
    // Time's up: a looping alarm (stopped after a minute) and a card above the
    // clock that never takes focus from a game.
    void ring() {
        KillTimer(hwnd,countdown_timer);scene.timer_state=Scene::TimerState::Ringing;
        wchar_t folder[MAX_PATH]{};GetWindowsDirectoryW(folder,MAX_PATH);const auto sound=std::wstring(folder)+L"\\Media\\Alarm01.wav";
        if(GetFileAttributesW(sound.c_str())==INVALID_FILE_ATTRIBUTES||!PlaySoundW(sound.c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_LOOP|SND_NODEFAULT))
            PlaySoundW(L"SystemExclamation",nullptr,SND_ALIAS|SND_ASYNC);
        SetTimer(hwnd,alarm_timer,60000,nullptr);
        JumpSection card;card.fixed=true;card.title=L"Time's up \x00B7 "+Scene::minutes_text(scene.timer_duration)+L" timer";
        {auto& e=card.entries.emplace_back();e.id=1;e.text=L"Dismiss";e.glyph=L'\xE711';}
        {auto& e=card.entries.emplace_back();e.id=2;e.text=L"Snooze 5 minutes";e.glyph=L'\xE916';}
        std::vector<JumpSection> sections;sections.push_back(std::move(card));
        const auto r=scene.clock_rect();const float scale=dpi/96;
        POINT a{static_cast<LONG>(std::lround(r.left*scale)),static_cast<LONG>(std::lround(scene.dock().top*scale))},b{static_cast<LONG>(std::lround(r.right*scale)),static_cast<LONG>(std::lround(r.bottom*scale))};
        ClientToScreen(hwnd,&a);ClientToScreen(hwnd,&b);
        try {alarm_session=alarm_card.open(hwnd,std::move(sections),RECT{a.x,a.y,b.x,b.y},a.y,static_cast<UINT>(dpi),scene.pal,scene.transparency&&!scene.high_contrast,scene.reduced,false,nullptr,nullptr,false);}
        catch(HRESULT error){log_error("alarm",error);}
        schedule();
    }
    void settings_tile_click(Tile tile,bool arrow) {
        using Page=Scene::SettingsPage;
        if(!arrow)scene.tile_motion[Scene::ti(tile)]={now(),true}; // The tile's icon reacts to the click.
        switch(tile) {
        case Tile::Wifi:if(arrow)open_page(Page::Wifi);else if(scene.net.wifi_present){scene.net.wifi_on=!scene.net.wifi_on;connectivity.set_wifi(scene.net.wifi_on);}break;
        case Tile::Bluetooth:if(arrow)open_page(Page::Bluetooth);else if(scene.net.bluetooth_present){scene.net.bluetooth_on=!scene.net.bluetooth_on;connectivity.set_bluetooth(scene.net.bluetooth_on);}break;
        case Tile::Mic:if(volume.mic_available()){volume.set_mic_muted(!volume.mic_muted());read_volume();}break;
        case Tile::Capture:instant_capture();return;
        case Tile::Timer:if(arrow)open_page(Page::Timer);else timer_primary();break;
        case Tile::Updates:updates_stale=true;launch(L"ms-settings:windowsupdate-action");return;
        case Tile::Project:open_page(Page::Project);return;
        case Tile::Airplane:airplane(!scene.airplane_on());break;
        case Tile::DarkMode:set_dark_mode(!scene.dark);scene.dark=dark_mode();break;
        case Tile::Captions:scene.captions_on=!scene.captions_on;send_shortcut({VK_LWIN,VK_CONTROL,'L'});return;
        case Tile::Cast:send_shortcut({VK_LWIN,'K'});return;
        case Tile::Keyboard:if(scene.keyboard_count>1)scene.keyboard_label=next_keyboard_layout(last_foreground);break;
        default:if(const auto page=settings_uri(tile))open_settings(page);return; // Tiles that open their Settings page.
        }
        schedule();
    }
    // Windows has no public toggle for these; see docs/decisions.md.
    static const wchar_t* settings_uri(Tile tile) {
        switch(tile) {
        case Tile::Hotspot:return L"ms-settings:network-mobilehotspot";
        case Tile::EnergySaver:return L"ms-settings:batterysaver";
        case Tile::Vpn:return L"ms-settings:network-vpn";
        case Tile::NightLight:return L"ms-settings:nightlight";
        case Tile::Nearby:return L"ms-settings:crossdevice";
        case Tile::Accessibility:return L"ms-settings:easeofaccess";
        case Tile::ColorFilters:return L"ms-settings:easeofaccess-colorfilter";
        case Tile::Rotation:return L"ms-settings:display";
        case Tile::DoNotDisturb:return L"ms-settings:notifications";
        default:return nullptr;
        }
    }
    // Airplane mode without Windows' private flag: switch off the radios we can
    // reach and remember which, so turning it off restores only those.
    void airplane(bool on) {
        auto& net=scene.net;
        if(on) {
            airplane_wifi=net.wifi_present&&net.wifi_on;airplane_bluetooth=net.bluetooth_present&&net.bluetooth_on;
            if(airplane_wifi){net.wifi_on=false;connectivity.set_wifi(false);}
            if(airplane_bluetooth){net.bluetooth_on=false;connectivity.set_bluetooth(false);}
            return;
        }
        // Nothing remembered (turned off elsewhere, or the dock restarted): bring Wi-Fi back.
        if(!airplane_wifi&&!airplane_bluetooth)airplane_wifi=net.wifi_present;
        if(airplane_wifi&&net.wifi_present){net.wifi_on=true;connectivity.set_wifi(true);}
        if(airplane_bluetooth&&net.bluetooth_present){net.bluetooth_on=true;connectivity.set_bluetooth(true);}
        airplane_wifi=airplane_bluetooth=false;
    }
    void network_click(const Scene::ListRow& row,float x,float y) {
        const auto network=scene.net.networks[row.index];
        if(row.open&&inside(Scene::row_button(row.box),x,y)) {
            if(network.connected)connectivity.disconnect_wifi();
            else if(!network.password_supported)launch(L"ms-settings:network-wifi");
            else connect_network(network);
            return;
        }
        if(row.open&&Scene::row_needs_password(network)) {
            if(inside(Scene::row_eye(row.box),x,y)){scene.password_visible=!scene.password_visible;return;}
            if(inside(Scene::row_field(row.box),x,y))return;
        }
        if(scene.expanded!=network.ssid){scene.expanded=network.ssid;clear_password();scene.net.wifi_message.clear();}
        else if(y<row.box.top+50){scene.expanded.clear();clear_password();}
    }
    void connect_network(const WifiNetwork& network) {
        if(!scene.net.connecting.empty())return;
        if(Scene::row_needs_password(network)&&scene.password.size()<8){scene.net.wifi_message=L"The security key needs at least 8 characters.";schedule();return;}
        connectivity.connect_wifi(network,scene.password);clear_password();scene.net.connecting=network.ssid;schedule();
    }
    void device_click(const Scene::ListRow& row,float x,float y) {
        const auto device=scene.net.devices[row.index];
        if(row.open&&inside(Scene::row_button(row.box),x,y)) {
            if(!device.switchable){launch(L"ms-settings:bluetooth");return;}
            if(scene.net.bluetooth_busy.empty()){connectivity.connect_bluetooth(device.key,!device.connected);scene.net.bluetooth_busy=device.key;}
            return;
        }
        scene.expanded=scene.expanded==device.key?std::wstring():device.key;
    }
    void timer_page_click(float x,float y) {
        using State=Scene::TimerState;
        if(scene.timer_state==State::Idle)
            for(const bool up:{false,true})if(inside(scene.timer_step(up),x,y)){scene.timer_duration=std::clamp<std::int64_t>(scene.timer_duration+(up?60000:-60000),60000,180*60000);schedule();return;}
        for(size_t i=0;i<std::size(Scene::timer_presets);++i)if(inside(scene.timer_chip(i),x,y)){timer_start(Scene::timer_presets[i]*60000LL);return;}
        const auto count=scene.timer_buttons().size();
        for(size_t i=0;i<count;++i)if(inside(scene.timer_button(i,count),x,y)) {
            switch(scene.timer_state) {
            case State::Idle:timer_start(scene.timer_duration);break;
            case State::Running:if(i==0)timer_pause();else timer_reset();break;
            case State::Paused:if(i==0)timer_resume();else timer_reset();break;
            case State::Ringing:if(i==0)timer_reset();else timer_start(5*60000);break;
            }
            return;
        }
    }
    void settings_page_click(float x,float y) {
        using Page=Scene::SettingsPage;
        if(inside(scene.page_back(),x,y)){scene.start_page(Page::Main,now());scene.expanded.clear();clear_password();schedule();return;}
        if(scene.settings_page==Page::Timer){timer_page_click(x,y);return;}
        if(scene.settings_page==Page::AddTiles){add_page_click(x,y);schedule();return;}
        if(scene.settings_page==Page::Project) {
            if(inside(scene.page_footer_link(),x,y)||inside(scene.page_footer_action(),x,y)){open_settings(L"ms-settings:display");return;}
            if(scene.display.displays<2)return;
            // Close first: the screens (and the dock with them) are about to move.
            for(size_t i=0;i<std::size(Scene::project_modes);++i)if(inside(scene.project_cell(i),x,y)){dismiss_panel(true);set_display_mode(Scene::project_modes[i]);return;}
            return;
        }
        const bool wifi=scene.settings_page==Page::Wifi;
        if(inside(scene.page_switch(),x,y)) {
            if(wifi&&scene.net.wifi_present){scene.net.wifi_on=!scene.net.wifi_on;connectivity.set_wifi(scene.net.wifi_on);if(scene.net.wifi_on)connectivity.scan();}
            else if(!wifi&&scene.net.bluetooth_present){scene.net.bluetooth_on=!scene.net.bluetooth_on;connectivity.set_bluetooth(scene.net.bluetooth_on);}
            schedule();return;
        }
        if(inside(scene.page_footer_link(),x,y)){launch(wifi?L"ms-settings:network-wifi":L"ms-settings:bluetooth");return;}
        if(inside(scene.page_footer_action(),x,y)){if(wifi)connectivity.scan();else launch(L"ms-settings-connectabledevices:devicediscovery");schedule();return;}
        if(wifi&&scene.net.location_denied&&inside(scene.page_notice_button(),x,y)){launch(L"ms-settings:privacy-location");return;}
        if(!inside(scene.page_list(),x,y))return;
        for(const auto& row:scene.list_rows())if(inside(row.box,x,y)){if(wifi)network_click(row,x,y);else device_click(row,x,y);schedule();return;}
    }
    // The expanded network that is waiting for a typed security key.
    const WifiNetwork* password_network() const {
        if(scene.panel!=Panel::Settings||scene.settings_page!=Scene::SettingsPage::Wifi||scene.expanded.empty())return nullptr;
        for(const auto& n:scene.net.networks)if(n.ssid==scene.expanded&&Scene::row_needs_password(n))return &n;
        return nullptr;
    }
    void paste_password() {
        if(!OpenClipboard(hwnd))return;
        if(const auto data=GetClipboardData(CF_UNICODETEXT))if(const auto text=static_cast<const wchar_t*>(GlobalLock(data))) {
            for(const wchar_t* c=text;*c&&*c!=L'\r'&&*c!=L'\n'&&scene.password.size()<64;++c)scene.password.push_back(*c);
            GlobalUnlock(data);
        }
        CloseClipboard();
    }
    // While the slider is dragged (and briefly after), the knob shows the pointer's
    // value: echoes of earlier levels from Windows must not pull it back.
    std::int64_t volume_settle_until=0;
    void read_volume() {
        scene.mic_available=volume.mic_available();scene.mic_muted=volume.mic_muted();
        scene.volume_available=volume.available();
        if(scene.slider_active||now()<volume_settle_until)return;
        scene.volume_level=volume.level();scene.volume_muted=volume.muted();
    }
    void show_volume(float value) {
        scene.volume_level=std::clamp(value,0.0f,1.0f);if(scene.volume_level>0)scene.volume_muted=false;
        volume_settle_until=now()+400;volume.set_level_async(scene.volume_level);schedule();
    }
    void wave_volume(){if(now()-scene.volume_wave>500)scene.volume_wave=now();} // Volume icon waves pulse.
    void adjust_volume(float delta) {
        if(!volume.available())return;wave_volume();
        show_volume(scene.volume_level+delta);
    }
    void volume_from(float x) {
        const auto track=scene.volume_track();wave_volume();
        show_volume((x-track.left)/(track.right-track.left));
    }
    void show_brightness(int percent) {
        scene.brightness_level=std::clamp(percent,0,100);
        brightness.set(scene.brightness_level);schedule();
    }
    void brightness_from(float x) {
        const auto track=scene.brightness_track();
        show_brightness(static_cast<int>(std::lround(100*(x-track.left)/(track.right-track.left))));
    }
    bool over_brightness(D2D1_POINT_2F pt) const {
        return scene.brightness_available&&scene.settings_page==Scene::SettingsPage::Main&&inside(scene.brightness_hit(),pt.x,pt.y);
    }
    // Panel-space point (undoes the flyout reveal scale), as click() does.
    D2D1_POINT_2F panel_point(float x,float y) const {
        const auto r=scene.panel_rect();const float center=(r.left+r.right)/2,scale=scene.panel_reveal.scale();
        return D2D1::Point2F(center+(x-center)/scale,r.bottom+(y-r.bottom)/scale);
    }
    size_t pinned_count() const {
        size_t count=0;
        while(count<scene.shown_groups()&&scene.groups[count].pinned&&!scene.overflow(count))++count;
        return count;
    }
    void begin_drag() {
        scene.dragging=drag_candidate;scene.clear_press();scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);
        close_previews(false);scene.set_hovered(-1,now());drag_moved=true;
        scene.machine.hold(Hold::Drag,true,now());
    }
    // Other pins slide aside to show where the dragged one will land.
    void update_drag() {
        const auto count=pinned_count();if(!count)return;scene.ensure_motion();
        const auto from=static_cast<size_t>(scene.dragging);
        const float first=scene.app_x(0);
        scene.drag_slot=static_cast<size_t>(std::clamp<long>(std::lround((scene.drag_x-first)/DockLayout::pitch),0,static_cast<long>(count)-1));
        for(size_t j=0;j<count;++j) {
            if(j==from)continue;
            size_t display=j>from?j-1:j;if(display>=scene.drag_slot)++display;
            scene.shift[j+1].target((static_cast<float>(display)-static_cast<float>(j))*DockLayout::pitch,now(),scene.reduced);
        }
    }
    void end_drag(bool commit) {
        const auto from=static_cast<size_t>(scene.dragging),to=scene.drag_slot;
        scene.ensure_motion();
        if(commit&&to!=from&&from<scene.groups.size()&&to<scene.groups.size()) {
            auto moved=std::move(scene.groups[from]);scene.groups.erase(scene.groups.begin()+static_cast<std::ptrdiff_t>(from));
            scene.groups.insert(scene.groups.begin()+static_cast<std::ptrdiff_t>(to),std::move(moved));
            // Same order for the pins, so window-only rescans keep it; then persist.
            std::vector<Shortcut> ordered,rest=scene.inventory.pinned;
            for(const auto& g:scene.groups) {
                if(!g.pinned)break;
                const auto found=std::find_if(rest.begin(),rest.end(),[&](const Shortcut& pin){
                    return (!pin.app_id.empty()&&pin.app_id==g.app_id)||(!pin.path.empty()&&_wcsicmp(pin.path.c_str(),g.launch_path.c_str())==0)||(!pin.target.empty()&&!g.executable.empty()&&_wcsicmp(pin.target.c_str(),g.executable.c_str())==0);
                });
                if(found!=rest.end()){ordered.push_back(std::move(*found));rest.erase(found);}
            }
            ordered.insert(ordered.end(),std::make_move_iterator(rest.begin()),std::make_move_iterator(rest.end()));
            scene.inventory.pinned=std::move(ordered);
            if(!save_pin_ranks(scene.inventory.pinned))MessageBoxW(hwnd,L"The new pin order could not be saved.",L"MinimaDock",MB_OK|MB_ICONERROR);
            scene.rebuild_icons();scan(true);
        }
        const auto landed=commit?to:from;
        for(auto& spring:scene.shift)spring.snap(0);
        // The dropped icon glides from the pointer into its slot.
        if(landed+1<scene.shift.size()){scene.shift[landed+1].snap(scene.drag_x-scene.app_x(landed));scene.shift[landed+1].target(0,now(),scene.reduced);}
        scene.dragging=-1;drag_candidate=-1;
        scene.machine.hold(Hold::Drag,false,now());schedule();
    }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto self=reinterpret_cast<Preview*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) { self=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->hwnd=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if(!self) return DefWindowProcW(window,message,w,l);
        try { return self->message(message,w,l); }
        catch(HRESULT error) {
            if(self->scene.desktop) {
                wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
                std::ofstream(std::filesystem::path(path).parent_path()/L"dock-error.txt")<<"message="<<message<<" HRESULT="<<std::hex<<static_cast<unsigned long>(error)<<" glass="<<GlassHost::failure()<<'\n';
                PostMessageW(window,WM_CLOSE,0,0);
            }
            return DefWindowProcW(window,message,w,l);
        }
        catch(...) { MessageBoxW(window,L"Preview operation failed. Windows remains active.",L"MinimaDock",MB_OK|MB_ICONERROR); return 0; }
    }
    std::int64_t now() const { return static_cast<std::int64_t>(GetTickCount64()); }
    // Current button/Escape state. `pressed` also reports presses since the
    // previous poll (low bit), so a click shorter than one 16 ms tick counts.
    static unsigned popup_input(unsigned* pressed=nullptr) {
        unsigned state=0,since=0;constexpr int keys[]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_ESCAPE};
        for(size_t i=0;i<std::size(keys);++i){const auto value=GetAsyncKeyState(keys[i]);if(value&0x8000)state|=1u<<i;if(value&1)since|=1u<<i;}
        if(pressed)*pressed=since;
        return state;
    }
    void dismiss_panel(bool restore_focus=false) {
        const auto foreground=GetForegroundWindow();
        const bool owned=foreground==hwnd||foreground==calendar.window()||foreground==search;
        set_panel(Panel::None);scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
        if(restore_focus&&owned&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
        if(launcher_over_fullscreen)fullscreen(); // Back to the game: hide again.
        schedule();
    }
    bool popup_contains(POINT point) const {
        if(calendar.window()){RECT r{};GetWindowRect(calendar.window(),&r);if(PtInRect(&r,point))return true;}
        if(!ScreenToClient(hwnd,&point))return false;
        const float x=point.x*96.0f/dpi,y=point.y*96.0f/dpi-(live_dock?visibility.offset():0);
        return inside(scene.dock(),x,y)||(scene.panel!=Panel::Calendar&&inside(scene.panel_rect(),x,y));
    }
    void poll_popup_input(unsigned state,POINT point,unsigned pressed=0) {
        const auto down=(state&~popup_keys)|pressed;popup_keys=state;
        if(scene.panel==Panel::None||scene.machine.holds()&static_cast<unsigned>(Hold::ContextMenu)||tray.holds_menu())return;
        // Observe only click/Escape edges while a flyout is open. No global hook,
        // capture, input injection, or consumption of the other app's click.
        if(down&8){dismiss_panel(true);return;}
        if((down&7)&&!popup_contains(point))dismiss_panel();
    }
    void watch_popup(bool reset=false) {
        if(scene.panel!=Panel::None) {
            if(reset||!popup_watching)popup_keys=popup_input();
            if(!popup_watching)popup_watching=SetTimer(hwnd,popup_input_timer,16,nullptr)!=0;
        } else if(popup_watching){KillTimer(hwnd,popup_input_timer);popup_watching=false;}
    }
    static LRESULT CALLBACK edge_proc(HWND window,UINT msg,WPARAM w,LPARAM l) {
        auto self=reinterpret_cast<Preview*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,msg,w,l);
        if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(msg==WM_MOUSEMOVE&&self->auto_hidden&&self->scene.machine.visible()) {
            if(!self->edge_tracking){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window,0};TrackMouseEvent(&t);self->edge_tracking=true;}
            self->hover_hold(true);self->schedule();return 0;
        }
        if(msg==WM_MOUSELEAVE){self->edge_tracking=false;self->pointer_hold();self->schedule();return 0;}
        return DefWindowProcW(window,msg,w,l);
    }
    void create_edge() {
        WNDCLASSW klass{};klass.lpfnWndProc=edge_proc;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"MinimaDock.RevealEdge";
        klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&klass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw E_FAIL;
        edge=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,klass.lpszClassName,L"MinimaDock reveal edge",WS_POPUP,0,0,1,1,hwnd,nullptr,klass.hInstance,this);
        if(!edge)throw E_FAIL;
        reveal_hotkey=RegisterHotKey(hwnd,3,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_SPACE)!=FALSE;
    }
    void release_edge_bitmap() {
        if(edge_dc&&edge_previous)SelectObject(edge_dc,edge_previous);
        if(edge_dib)DeleteObject(edge_dib);
        if(edge_dc)DeleteDC(edge_dc);
        edge_dc=nullptr;edge_dib=nullptr;edge_previous=nullptr;edge_pixels=nullptr;edge_width=edge_height=edge_indicator=0;edge_coverage.clear();
    }
    void present_edge(int x,int y,int width,int height,int indicator_height,int alpha) {
        // Nonzero alpha retains a continuous, non-activating pointer bridge.
        // The neutral rounded indicator fades away as the pill reveals.
        // The DIB and the indicator's coverage mask are kept until the size
        // changes; a fade frame only rewrites the indicator rows.
        if(!edge_dc||width!=edge_width||height!=edge_height||indicator_height!=edge_indicator) {
            release_edge_bitmap();
            BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=width;
            bitmap.bmiHeader.biHeight=-height;bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
            edge_dc=CreateCompatibleDC(nullptr);
            edge_dib=edge_dc?CreateDIBSection(edge_dc,&bitmap,DIB_RGB_COLORS,&edge_pixels,nullptr,0):nullptr;
            if(!edge_dc||!edge_dib||!edge_pixels){release_edge_bitmap();throw E_OUTOFMEMORY;}
            edge_previous=SelectObject(edge_dc,edge_dib);edge_width=width;edge_height=height;edge_indicator=indicator_height;
            auto pixels=static_cast<BYTE*>(edge_pixels);
            for(size_t i=0;i<static_cast<size_t>(width)*height;++i){pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=0;pixels[i*4+3]=1;}
            edge_coverage.resize(static_cast<size_t>(width)*indicator_height);
            const float radius=indicator_height*.5f;
            for(int row=0;row<indicator_height;++row)for(int col=0;col<width;++col) {
                const float cy=row+.5f-radius,cx=col+.5f-std::clamp(col+.5f,radius,width-radius);
                edge_coverage[static_cast<size_t>(row)*width+col]=std::clamp(radius+.5f-std::sqrt(cx*cx+cy*cy),0.0f,1.0f);
            }
            edge_indicator_alpha=-1;
        }
        if(edge_indicator_alpha!=alpha) {
            auto pixels=static_cast<BYTE*>(edge_pixels);const int first=height-indicator_height;
            for(int row=0;row<indicator_height;++row) {
                const int shade=row==0?210:150;
                for(int col=0;col<width;++col) {
                    const auto index=(static_cast<size_t>(first+row)*width+col)*4;
                    const BYTE a=static_cast<BYTE>(std::max(1L,std::lround(alpha*edge_coverage[static_cast<size_t>(row)*width+col])));
                    pixels[index]=pixels[index+1]=pixels[index+2]=static_cast<BYTE>(shade*a/255);pixels[index+3]=a;
                }
            }
        }
        POINT destination{x,y},source{0,0};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(edge,nullptr,&destination,&size,edge_dc,&source,0,&blend,ULW_ALPHA))throw E_FAIL;
        edge_indicator_alpha=alpha;
    }
    void pointer_hold() {
        POINT p{};bool over=false;
        if(GetCursorPos(&p)&&ScreenToClient(hwnd,&p)) {
            const float x=p.x*96.0f/dpi,y=p.y*96.0f/dpi;
            const auto r=scene.dock();
            over=visibility.wanted()&&(inside(r,x,y)||inside(r,x,y-visibility.offset())||(scene.panel!=Panel::None&&scene.panel!=Panel::Calendar&&inside(scene.panel_rect(),x,y)));
            scene.pointer_x=x;scene.pointer_y=y-visibility.offset();
        }
        hover_hold(over||edge_tracking);
    }
    // Settings > Dock > Hide delay: a revealed dock (one that hides) stays up this
    // long after the pointer leaves. Revealing is always immediate.
    bool hide_delay_pending=false,hide_delay_done=false;
    void hover_hold(bool over) {
        if(over){if(hide_delay_pending){KillTimer(hwnd,hide_delay_timer);hide_delay_pending=false;}scene.machine.hold(Hold::Pointer,true,now());return;}
        const bool held=(scene.machine.holds()&static_cast<unsigned>(Hold::Pointer))!=0;
        if(!held||hide_delay_done||dock_settings.hide_delay<=0||!auto_hidden){scene.machine.hold(Hold::Pointer,false,now());return;}
        if(!hide_delay_pending)hide_delay_pending=SetTimer(hwnd,hide_delay_timer,static_cast<UINT>(dock_settings.hide_delay),nullptr)!=0;
        if(!hide_delay_pending)scene.machine.hold(Hold::Pointer,false,now());
    }
    void sync_visibility() {
        if(!live_dock)return;
        // The bar's own hold keeps it expanded; it is not a reason to stay revealed.
        visibility.update(auto_hidden,!scene.machine.visible(),(scene.machine.holds()&~static_cast<unsigned>(Hold::Bar))!=0,now());
        auto style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
        const auto wanted=visibility.wanted()?style&~WS_EX_TRANSPARENT:style|WS_EX_TRANSPARENT;
        if(style!=wanted)SetWindowLongPtrW(hwnd,GWL_EXSTYLE,wanted);
        if(edge) {
            MONITORINFO info{sizeof(info),{},{},0};
            if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY),&info)) {
                const auto pill=scene.dock();POINT origin{0,0};
                if(layer_bounds_pending)origin.x=pending_layer_bounds.left;else ClientToScreen(hwnd,&origin);
                const auto x=static_cast<int>(origin.x)+static_cast<int>(std::lround(pill.left*dpi/96));
                const auto width=std::max(1,static_cast<int>(origin.x)+static_cast<int>(std::lround(pill.right*dpi/96))-x);
                const auto indicator_height=std::max(2,static_cast<int>(std::lround(3*dpi/96)));
                const auto height=visibility.wanted()?std::max(indicator_height,static_cast<int>(std::ceil(14*dpi/96))):indicator_height;
                const auto y=info.rcMonitor.bottom-height;
                const auto alpha=static_cast<int>(std::lround(180*(1-visibility.opacity())));
                RECT current{};GetWindowRect(edge,&current);
                if(current.left!=x||current.top!=y||current.right-current.left!=width||current.bottom-current.top!=height||edge_indicator_alpha!=alpha)
                    present_edge(x,y,width,height,indicator_height,alpha);
                const bool shown=auto_hidden&&scene.machine.visible();
                if((IsWindowVisible(edge)!=FALSE)!=shown)ShowWindow(edge,shown?SW_SHOWNOACTIVATE:SW_HIDE);
            }
        }
    }
    void release_layer() {
        if(glass_mode){glass.detach();glass_bound=false;}
        layer_render.reset();layer_bitmap.reset();layer_presented=false;
        if(layer_dc&&layer_previous) SelectObject(layer_dc,layer_previous);
        if(layer_dib) DeleteObject(layer_dib);
        if(layer_dc) DeleteDC(layer_dc);
        layer_dc=nullptr;layer_dib=nullptr;layer_previous=nullptr;layer_pixels=nullptr;
    }
    void desktop_position() {
        POINT origin{0,0};const auto monitor=MonitorFromPoint(origin,MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info),{},{},0};if(!GetMonitorInfoW(monitor,&info)) throw E_FAIL;
        const float scale=dpi/96;
        const auto desired=DockLayout::natural_expanded_width(scene.groups.size())+40;
        const auto shown=scene.drawn_panel();
        // The launcher needs a wider, taller canvas than the other flyouts. Glass
        // windows keep that size all the time: resizing a composition window between
        // flyouts (launcher -> clock or Wi-Fi) showed one stretched, misplaced frame.
        // The window region still limits input to the dock and the open flyout.
        const bool roomy=glass_mode||shown==Panel::Launcher;
        const float canvas=roomy?std::max(desired,Scene::launch_width+80):desired;
        const auto w=scene.bar?info.rcMonitor.right-info.rcMonitor.left:std::min(info.rcMonitor.right-info.rcMonitor.left,static_cast<LONG>(std::ceil(canvas*scale)));
        const float tall=roomy?Scene::launch_height+100:440;
        const auto h=std::min(info.rcMonitor.bottom-info.rcMonitor.top,static_cast<LONG>((!glass_mode&&(shown==Panel::None||shown==Panel::Calendar)?88:tall)*scale));
        // Keep the currently displayed bitmap in place until its replacement is
        // ready. UpdateLayeredWindow commits destination, size and pixels together.
        // Keep the ordered compact group at the monitor center. The larger
        // canvas reserves space for both wings without moving apps on hover.
        const auto left=info.rcMonitor.left+(info.rcMonitor.right-info.rcMonitor.left-w)/2;
        pending_layer_bounds={left,info.rcMonitor.bottom-h,left+w,info.rcMonitor.bottom};layer_bounds_pending=true;
        scene.width=w/scale;scene.height=h/scale;
        if(tracking) {
            POINT pointer{};
            if(GetCursorPos(&pointer)) {
                scene.pointer_x=(pointer.x-left)/scale;
                scene.pointer_y=(pointer.y-pending_layer_bounds.top)/scale-(live_dock?visibility.offset():0);
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    // Only the foreground window's geometry affects auto-hide. A system-wide
    // EVENT_OBJECT_LOCATIONCHANGE hook also fires for every cursor move, so
    // follow just the foreground process and re-target when it changes.
    void watch_location(HWND foreground) {
        DWORD pid=0;if(foreground)GetWindowThreadProcessId(foreground,&pid);
        if(!pid||pid==GetCurrentProcessId()||(pid==location_process&&hooks[3]))return;
        if(hooks[3])UnhookWinEvent(hooks[3]);
        hooks[3]=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,window_event,pid,0,WINEVENT_OUTOFCONTEXT);
        location_process=hooks[3]?pid:0;
    }
    void fullscreen() {
        if(!live_dock)return;
        auto foreground=GetForegroundWindow();RECT bounds{};MONITORINFO info{sizeof(info),{},{},0};
        DWORD owner=0;GetWindowThreadProcessId(foreground,&owner);
        // Activation can briefly report no foreground while an owned popup closes.
        // Retain the application used for visibility and focus return through it.
        if(!foreground||owner==GetCurrentProcessId()||tray.owns(foreground))foreground=last_foreground;else last_foreground=foreground;
        watch_location(foreground);scene.active_window=foreground;
        owner=0;GetWindowThreadProcessId(foreground,&owner);
        const auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY);
        wchar_t klass[128]{};GetClassNameW(foreground,klass,128);
        const bool desktop=wcscmp(klass,L"Progman")==0||wcscmp(klass,L"WorkerW")==0;
        // Browsers keep a maximized window maximized for full-screen video, so a
        // maximized window counts too when its visible frame fills the monitor while
        // the work area is smaller (the persistent bar or another reservation):
        // ordinary maximized windows stop at the work area.
        const auto fills=[&](const RECT& r){return r.left<=info.rcMonitor.left&&r.top<=info.rcMonitor.top&&r.right>=info.rcMonitor.right&&r.bottom>=info.rcMonitor.bottom;};
        bool covers=foreground&&owner!=GetCurrentProcessId()&&!desktop&&!IsIconic(foreground)&&
            MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY)==monitor&&GetWindowRect(foreground,&bounds)&&GetMonitorInfoW(monitor,&info)&&
            (!IsZoomed(foreground)?fills(bounds):[&]{
                RECT frame{};
                return !EqualRect(&info.rcWork,&info.rcMonitor)&&SUCCEEDED(DwmGetWindowAttribute(foreground,DWMWA_EXTENDED_FRAME_BOUNDS,&frame,sizeof(frame)))&&fills(frame);
            }());
        // The launcher opened with the Windows key over a full-screen app stays up until it closes.
        if(scene.panel!=Panel::Launcher)launcher_over_fullscreen=false;
        if(launcher_over_fullscreen)covers=false;
        if(covers) {calendar.close(false);set_panel(Panel::None,false);close_previews(false);scene.stop_feedback();}
        const bool over_window=foreground&&!desktop&&IsWindowVisible(foreground)&&!IsIconic(foreground)&&
            (IsZoomed(foreground)||window_arranged(foreground))&&MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY)==monitor;
        // Customize > Dock > Hide the dock: over maximized windows (default), always, never.
        if(dock_settings.style==DockSettings::Style::Bar)auto_hidden=dock_settings.bar_autohide; // Like the Windows taskbar's auto-hide.
        else auto_hidden=dock_settings.hide==DockSettings::Hide::Always?true:dock_settings.hide==DockSettings::Hide::Never?false:over_window;
        scene.machine.suppress(covers,now());
        if(!covers)scene.machine.hold(Hold::Bar,scene.bar,now()); // Suppression clears holds.
        scene.readability_boost=over_window;
        sync_visibility();
    }
    // Right-click on empty dock space: the dock's own settings and recovery.
    void desktop_menu(float x=-1,bool keyboard=false) {
        dismiss_panel();
        std::vector<JumpSection> sections(live_dock?3:2);
        add_entry(sections[0],1,L"Task Manager",L'\xE9D9');
        add_entry(sections[0],2,L"Show desktop",L'\xE7C4');
        add_entry(sections[0],3,L"Quick settings",L'\xE713');
        add_entry(sections[1],8,L"MinimaDock settings\x2026",L'\xE771');
        if(live_dock)add_entry(sections[1],4,windows_key.enabled()?L"Windows key opens Start":L"Windows key opens MinimaDock",L'\xE765');
        add_entry(sections[1],5,L"Windows taskbar settings",L'\xE8A9');
        if(live_dock) {
            if(supervisor_status()==1)add_entry(sections[2],6,L"Keep MinimaDock for this session",L'\xE718'); // Trial runs only.
            add_entry(sections[2],7,L"Return to the Windows taskbar",L'\xE7A7');
            if(const auto u=app_updater.status();u.available)add_entry(sections[1],9,L"Update to MinimaDock "+u.latest,L'\xE896');
        }
        const auto d=scene.dock();POINT cursor{};GetCursorPos(&cursor);POINT client=cursor;ScreenToClient(hwnd,&client);
        const float at=x>=0?x:static_cast<float>(client.x)*96/dpi;
        glass_menu(std::move(sections),D2D1::RectF(at-1,d.top,at+1,d.bottom),keyboard,[this](UINT id) {
            if(id==1)launch(L"taskmgr.exe");
            else if(id==2)action(static_cast<int>(scene.shown_groups())+4);
            else if(id==3)set_panel(Panel::Settings);
            else if(id==4){const bool on=!windows_key.enabled();save_windows_key_setting(on);windows_key.enable(on);scene.windows_key_on=on;}
            else if(id==5)launch(L"ms-settings:taskbar");
            else if(id==8)open_customize();
            else if(id==9)start_update();
            else if(id==6) {
                wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
                const auto path=(std::filesystem::path(module).parent_path()/L"MinimaDockTrial.exe").wstring();
                ShellExecuteW(hwnd,L"open",path.c_str(),L"--keep",nullptr,SW_HIDE);
            } else if(id==7)return_to_windows();
        });
    }
    // The supervisor (MinimaDockTrial.exe) hides the Windows taskbar and restarts
    // a crashed dock. 0 none, 1 timed trial, 2 kept or installed.
    static HWND supervisor(){return FindWindowW(L"MinimaDock.Trial.Control",nullptr);}
    static int supervisor_status() {
        DWORD_PTR reply=0;const auto control=supervisor();
        return control&&SendMessageTimeoutW(control,WM_APP+22,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,300,&reply)?static_cast<int>(reply):0;
    }
    // Brings the Windows taskbar back for this session (the supervisor restores
    // it and closes the dock; it would otherwise restart a closed dock).
    // Downloads and installs the newer release in a visible PowerShell window.
    void start_update() {
        if(!app_updater.install(hwnd))MessageBoxW(hwnd,L"The update could not be started. You can download it from GitHub instead.",L"MinimaDock",MB_OK|MB_ICONWARNING);
        else customize.close();
    }
    void return_to_windows() {
        if(const auto control=supervisor())PostMessageW(control,WM_APP+20,0,0);else PostMessageW(hwnd,WM_CLOSE,0,0);
    }
    // Sign-in start: HKCU Run "MinimaDock" -> this folder's supervisor with --run.
    static constexpr const wchar_t* run_key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static bool starts_with_windows() {
        DWORD size=0;return RegGetValueW(HKEY_CURRENT_USER,run_key,L"MinimaDock",RRF_RT_REG_SZ,nullptr,nullptr,&size)==ERROR_SUCCESS;
    }
    static void set_starts_with_windows(bool on) {
        if(!on){RegDeleteKeyValueW(HKEY_CURRENT_USER,run_key,L"MinimaDock");return;}
        wchar_t module[32768]{};if(!GetModuleFileNameW(nullptr,module,32768))return;
        const auto command=L"\""+(std::filesystem::path(module).parent_path()/L"MinimaDockTrial.exe").wstring()+L"\" --run";
        RegSetKeyValueW(HKEY_CURRENT_USER,run_key,L"MinimaDock",REG_SZ,command.c_str(),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    }
    // Menus in the same glass style as the app jump lists (and the system theme).
    JumpList control_list{control_list_message};
    unsigned control_session=0;
    std::function<void(UINT)> control_action;
    static void add_entry(JumpSection& section,UINT id,std::wstring text,wchar_t glyph) {
        auto& e=section.entries.emplace_back();e.id=id;e.text=std::move(text);e.glyph=glyph;
    }
    void glass_menu(std::vector<JumpSection> sections,D2D1_RECT_F anchor,bool keyboard,std::function<void(UINT)> action_for) {
        std::erase_if(sections,[](const JumpSection& s){return s.entries.empty();});
        const float scale=dpi/96,offset=live_dock?visibility.offset():0;
        POINT a{static_cast<LONG>(std::lround(anchor.left*scale)),static_cast<LONG>(std::lround((anchor.top+offset)*scale))},b{static_cast<LONG>(std::lround(anchor.right*scale)),static_cast<LONG>(std::lround((anchor.bottom+offset)*scale))};
        ClientToScreen(hwnd,&a);ClientToScreen(hwnd,&b);
        POINT top{0,static_cast<LONG>(std::lround((scene.dock().top+offset)*scale))};ClientToScreen(hwnd,&top);
        scene.machine.hold(Hold::ContextMenu,true,now());
        control_action=std::move(action_for);
        SetForegroundWindow(hwnd);
        try {control_session=control_list.open(hwnd,std::move(sections),RECT{a.x,a.y,b.x,b.y},top.y,static_cast<UINT>(dpi),scene.pal,scene.transparency&&!scene.high_contrast,scene.reduced,keyboard,nullptr,nullptr);}
        catch(HRESULT error){log_error("menu",error);scene.machine.hold(Hold::ContextMenu,false,now());}
        schedule();
    }
    void control_list_event(UINT id,unsigned session) {
        if(session!=control_session)return;
        scene.machine.hold(Hold::ContextMenu,false,now());
        auto run=std::move(control_action);control_action=nullptr;
        if(id&&run)run(id);
        else if(GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
        schedule();
    }
    void windows_key_pressed_or_open() {
        if(scene.panel!=Panel::Launcher)set_panel(Panel::Launcher);
    }
    // Packaged apps must launch through their shell identity, not a protected
    // WindowsApps executable. Preserve original shortcuts and desktop args.
    std::wstring launch_target(const Group& group) const {
        // A helper program never starts its app: launch the app it belongs to.
        if(const auto* app=scene.helper_owner(group.launch_path))return app->program;
        if(!group.app_id.empty()&&(!group.pinned||group.launch_path.empty()||_wcsnicmp(group.launch_path.c_str(),L"shell:AppsFolder\\",17)!=0)) {
            const auto shell_path=L"shell:AppsFolder\\"+group.app_id;
            PIDLIST_ABSOLUTE id=nullptr;
            if(SUCCEEDED(SHParseDisplayName(shell_path.c_str(),nullptr,&id,0,nullptr))){CoTaskMemFree(id);return shell_path;}
        }
        return group.launch_path;
    }
    // Right-click on the jump list's app entry: its native Shell menu, nested
    // like Explorer's (Run as administrator, Open file location, properties).
    void app_menu_right_click(WPARAM position,HMENU source) {
        if(!open_menu||source!=open_menu->menu||static_cast<int>(position)!=open_menu->launch_position)return;
        const bool extended=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(!open_menu->shell.load(hwnd,open_menu_path,extended))return;
        POINT point{};GetCursorPos(&point);
        const auto selected=static_cast<UINT>(TrackPopupMenuEx(open_menu->shell.popup,TPM_RECURSE|TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,hwnd,nullptr));
        if(selected){open_menu_shell=selected;open_menu_point=point;EndMenu();}
    }
    // Windows 11 placement: centred over the icon, just above the dock.
    UINT track_app_menu(HMENU menu,size_t index,POINT point,bool keyboard,UINT flags) {
        TPMPARAMS exclude{sizeof(exclude),slot_screen(index)};
        POINT top{0,static_cast<LONG>(std::lround((scene.dock().top+(live_dock?visibility.offset():0))*dpi/96))};ClientToScreen(hwnd,&top);
        exclude.rcExclude.top=std::min(exclude.rcExclude.top,top.y);
        const LONG gap=static_cast<LONG>(std::lround(8*dpi/96));
        POINT anchor{(exclude.rcExclude.left+exclude.rcExclude.right)/2,exclude.rcExclude.top-gap};
        if(!keyboard&&point.x!=-1)anchor.x=point.x;
        exclude.rcExclude.top-=gap;
        return static_cast<UINT>(TrackPopupMenuEx(menu,flags|TPM_RETURNCMD|TPM_BOTTOMALIGN|TPM_CENTERALIGN|TPM_VERTICAL,anchor.x,anchor.y,hwnd,&exclude));
    }
    // Shared by the jump list and the native menus. Returns false for ids it does not own.
    bool run_app_command(const Group& group,UINT selected,const std::vector<RecentItem>& recent,ShellItemMenu& shell,POINT point) {
        std::vector<HWND> windows;for(const auto window:group.windows)if(IsWindow(window))windows.push_back(window);
        if(selected==AppContextMenu::launch_id)launch_app(group);
        else if(selected==AppContextMenu::pin_id) {
            Shortcut pin{group.name,group.launch_path,false,group.executable,group.icon,group.app_id};
            if(const auto* app=scene.helper_owner(group.executable)){pin.title=app->name;pin.path=app->program;pin.target=app->program;}
            if(save_pin_preference(pin,!group.pinned))scan(true);
            else MessageBoxW(hwnd,L"The app's pin preference could not be saved.",L"MinimaDock",MB_OK|MB_ICONERROR);
        } else if(selected==AppContextMenu::close_id){for(const auto window:windows)PostMessageW(window,WM_CLOSE,0,0);}
        else if(selected==AppContextMenu::minimize_all_id){for(const auto window:windows)if(!IsIconic(window))PostMessageW(window,WM_SYSCOMMAND,SC_MINIMIZE,0);}
        else if(selected==AppContextMenu::restore_all_id){for(const auto window:windows)if(IsIconic(window))ShowWindowAsync(window,SW_RESTORE);}
        else if(selected>=AppContextMenu::recent_first&&selected<AppContextMenu::recent_first+recent.size()) {
            if(!open_recent_item(hwnd,recent[selected-AppContextMenu::recent_first].item.get()))MessageBoxW(hwnd,L"Windows could not open this recent item.",L"MinimaDock",MB_OK|MB_ICONERROR);
        } else if(selected>=ShellItemMenu::first&&selected<=ShellItemMenu::last) {
            const auto result=shell.invoke(hwnd,selected,point);
            if(FAILED(result))MessageBoxW(hwnd,L"Windows could not complete this action.",L"MinimaDock",MB_OK|MB_ICONERROR);
            scan(true);
        } else return false;
        return true;
    }
    // The footer every jump list ends with: the app, Pin/Unpin and Close.
    static JumpSection jump_footer(const Group& group) {
        JumpSection actions;actions.fixed=true;
        const bool launchable=!group.launch_path.empty();
        {
            auto& e=actions.entries.emplace_back();e.id=AppContextMenu::launch_id;e.text=group.name;e.icon=group.icon;e.enabled=launchable;
            if(launchable&&FAILED(SHCreateItemFromParsingName(group.launch_path.c_str(),nullptr,IID_PPV_ARGS(e.item.put()))))e.item.reset();
        }
        {auto& e=actions.entries.emplace_back();e.id=AppContextMenu::pin_id;e.text=group.pinned?L"Unpin from taskbar":L"Pin to taskbar";e.glyph=group.pinned?L'\xE77A':L'\xE718';e.enabled=launchable;}
        const auto open=std::count_if(group.windows.begin(),group.windows.end(),[](HWND window){return IsWindow(window)!=FALSE;});
        if(open>0){auto& e=actions.entries.emplace_back();e.id=AppContextMenu::close_id;e.text=open>1?L"Close all windows":L"Close window";e.glyph=L'\xE8BB';}
        return actions;
    }
    // Shift+right-click: the window menu (or the group commands) in the same flyout.
    static std::vector<JumpSection> window_sections(const AppContextMenu& menu) {
        std::vector<JumpSection> sections(1);
        for(int i=0;i<GetMenuItemCount(menu.menu);++i) {
            wchar_t text[256]{};MENUITEMINFOW info{};info.cbSize=sizeof(info);info.fMask=MIIM_FTYPE|MIIM_ID|MIIM_STATE|MIIM_STRING;info.dwTypeData=text;info.cch=256;
            if(!GetMenuItemInfoW(menu.menu,static_cast<UINT>(i),TRUE,&info))continue;
            if(info.fType&MFT_SEPARATOR){if(!sections.back().entries.empty())sections.emplace_back();continue;}
            std::wstring label;
            for(const wchar_t* c=text;*c&&*c!=L'\t';++c){if(*c==L'&'&&c[1]!=L'&')continue;if(*c==L'&')++c;label+=*c;}
            auto& e=sections.back().entries.emplace_back();e.id=info.wID;e.text=std::move(label);e.enabled=!(info.fState&(MFS_DISABLED|MFS_GRAYED));
            UINT command=0;for(const auto& action:menu.commands)if(action.id==info.wID)command=action.command;
            if(command==SC_RESTORE||info.wID==AppContextMenu::restore_all_id)e.glyph=L'\xE923';
            else if(command==SC_MINIMIZE||info.wID==AppContextMenu::minimize_all_id)e.glyph=L'\xE921';
            else if(command==SC_MAXIMIZE)e.glyph=L'\xE922';
            else if(command==SC_CLOSE||info.wID==AppContextMenu::close_id)e.glyph=L'\xE8BB';
            else e.glyph=L' ';
        }
        return sections;
    }
    void show_jump_list(size_t index,std::unique_ptr<JumpContext> context,std::vector<JumpSection> sections,bool keyboard) {
        POINT top{0,static_cast<LONG>(std::lround((scene.dock().top+(live_dock?visibility.offset():0))*dpi/96))};ClientToScreen(hwnd,&top);
        scene.machine.hold(Hold::ContextMenu,true,now());
        jump_context=std::move(context);
        SetForegroundWindow(hwnd); // Lets the list take foreground for keyboard and dismissal.
        jump_session=jump.open(hwnd,std::move(sections),slot_screen(index),top.y,static_cast<UINT>(dpi),scene.pal,scene.transparency&&!scene.high_contrast,scene.reduced,keyboard,
            [this](UINT id,POINT point,HWND window){return jump_list_context(id,point,window);},
            [this](UINT message,WPARAM w,LPARAM l,LRESULT& result){return jump_context&&jump_context->shell.message(message,w,l,result);});
        schedule();
    }
    // The app's real Windows jump list: Pinned, its own categories, Tasks.
    void open_jump_list(size_t index,bool keyboard) {
        auto context=std::make_unique<JumpContext>();
        context->group=scene.groups[index];auto& group=context->group;group.launch_path=launch_target(group);
        context->app_id=jump_list_app_id(group.windows,group.launch_path,group.executable,group.app_id);
        context->lists=read_jump_list(context->app_id,jump_list_item_limit(),static_cast<UINT>(std::lround(16*dpi/96)));
        std::vector<JumpSection> sections;
        for(size_t g=0;g<context->lists.size();++g) {
            const auto& list=context->lists[g];
            auto& section=sections.emplace_back();section.title=list.name;
            section.fixed=list.kind==JumpGroup::Kind::Tasks||list.kind==JumpGroup::Kind::Pinned;
            for(size_t i=0;i<list.items.size();++i) {
                const auto& item=list.items[i];auto& e=section.entries.emplace_back();
                if(item.kind==JumpItem::Kind::Separator){e.separator=true;continue;}
                e.id=AppContextMenu::recent_first+static_cast<UINT>(context->refs.size());context->refs.push_back({g,i});
                e.text=item.name;e.icon=item.icon;
                if(item.image){item.image->AddRef();*e.item.put()=item.image.get();}
            }
        }
        sections.push_back(jump_footer(group));
        show_jump_list(index,std::move(context),std::move(sections),keyboard);
    }
    void open_window_list(size_t index,bool keyboard) {
        auto context=std::make_unique<JumpContext>();
        context->group=scene.groups[index];auto& group=context->group;group.launch_path=launch_target(group);
        AppContextMenu menu(hwnd,group.name,group.launch_path,group.app_id,nullptr,group.windows,group.pinned,true);
        if(!menu.menu||!menu.window_menu){open_jump_list(index,keyboard);return;}
        context->commands=menu.commands;
        show_jump_list(index,std::move(context),window_sections(menu),keyboard);
    }
    const JumpItem* jump_item(const JumpContext& context,UINT id,JumpGroup::Kind* kind=nullptr) const {
        if(id<AppContextMenu::recent_first||id>=AppContextMenu::recent_first+context.refs.size())return nullptr;
        const auto [g,i]=context.refs[id-AppContextMenu::recent_first];
        if(kind)*kind=context.lists[g].kind;
        return &context.lists[g].items[i];
    }
    // Right-click inside the jump list, as in Explorer: the app entry gets its
    // native Shell menu (Run as administrator, Open file location); list items
    // get Open, Pin to / Unpin from this list and Remove from this list.
    bool jump_list_context(UINT id,POINT point,HWND window) {
        if(!jump_context)return false;auto& context=*jump_context;
        if(id==AppContextMenu::launch_id) {
            if(!context.shell.load(window,context.group.launch_path,(GetKeyState(VK_SHIFT)&0x8000)!=0))return false;
            const auto selected=static_cast<UINT>(TrackPopupMenuEx(context.shell.popup,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window,nullptr));
            PostMessageW(window,WM_NULL,0,0);
            if(!selected)return false;
            if(FAILED(context.shell.invoke(hwnd,selected,point)))MessageBoxW(hwnd,L"Windows could not complete this action.",L"MinimaDock",MB_OK|MB_ICONERROR);
            return true;
        }
        JumpGroup::Kind kind{};const auto* item=jump_item(context,id,&kind);
        if(!item||kind==JumpGroup::Kind::Tasks)return false;
        const auto menu=CreatePopupMenu();if(!menu)return false;
        const bool pinned=kind==JumpGroup::Kind::Pinned;
        AppendMenuW(menu,MF_STRING,1,L"Open");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,2,pinned?L"Unpin from this list":L"Pin to this list");
        if(!pinned)AppendMenuW(menu,MF_STRING,3,L"Remove from this list");
        SetMenuDefaultItem(menu,1,FALSE);
        const auto selected=static_cast<UINT>(TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window,nullptr));
        DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);
        bool done=true;
        if(selected==1)done=invoke_jump_item(hwnd,*item);
        else if(selected==2)done=pin_jump_item(context.app_id,*item,!pinned);
        else if(selected==3)done=remove_jump_item(context.app_id,kind,*item);
        if(!done)MessageBoxW(hwnd,L"Windows could not complete this action.",L"MinimaDock",MB_OK|MB_ICONERROR);
        return selected!=0;
    }
    void jump_list_event(UINT selected,unsigned session) {
        if(session!=jump_session||!jump_context)return;
        const auto context=std::move(jump_context);
        scene.machine.hold(Hold::ContextMenu,false,now());
        const auto foreground=GetForegroundWindow();
        const bool ours=foreground==hwnd||foreground==jump.window();
        const auto* item=jump_item(*context,selected);
        bool opens=selected==AppContextMenu::launch_id||item;
        if(item){if(!invoke_jump_item(hwnd,*item))MessageBoxW(hwnd,L"Windows could not open this item.",L"MinimaDock",MB_OK|MB_ICONERROR);}
        else if(selected&&!run_app_command(context->group,selected,context->recent,context->shell,POINT{})) {
            for(const auto& action:context->commands)if(action.id==selected) {
                if(IsWindow(action.window)){activate_window(action.window);if(action.command)PostMessageW(action.window,WM_SYSCOMMAND,action.command,0);}
                opens=true;break;
            }
        }
        // Launches keep foreground for the new window; everything else hands it back.
        if(ours&&!opens&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
        schedule();
    }
    void app_menu(size_t index,POINT point,bool window_menu=false,bool keyboard=false) {
        if(index>=scene.groups.size()||scene.overflow(index))return;
        if(scene.desktop&&GlassHost::available()) {
            dismiss_panel();close_previews(false);scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);
            try {if(window_menu)open_window_list(index,keyboard);else open_jump_list(index,keyboard);return;}
            catch(HRESULT error) {
                // Never lose the menu: fall back to the native one below.
                log_error("jump list",error);jump.close(false);jump_context.reset();scene.machine.hold(Hold::ContextMenu,false,now());
            }
        }
        auto group=scene.groups[index]; // Nested native menu pumps can rescan inventory.
        group.launch_path=launch_target(group);
        dismiss_panel();close_previews(false);scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);
        scene.machine.hold(Hold::ContextMenu,true,now());
        AppContextMenu context(hwnd,group.name,group.launch_path,group.app_id,static_cast<HICON>(group.icon.get()),group.windows,group.pinned,window_menu);
        if(!context.menu){scene.machine.hold(Hold::ContextMenu,false,now());schedule();return;}
        shell_menu=&context.shell;open_menu=&context;open_menu_path=group.launch_path;open_menu_shell=0;open_menu_point=point;
        SetForegroundWindow(hwnd);
        // The jump list omits TPM_RIGHTBUTTON so a right-click on the app entry
        // reaches WM_MENURBUTTONUP (its Shell menu) instead of launching it.
        auto selected=track_app_menu(context.menu,index,point,keyboard,context.window_menu?TPM_RIGHTBUTTON:0);
        if(open_menu_shell)selected=open_menu_shell;
        point=open_menu_point;shell_menu=nullptr;open_menu=nullptr;PostMessageW(hwnd,WM_NULL,0,0);
        if(!run_app_command(group,selected,context.recent,context.shell,point))for(const auto& action:context.commands)if(action.id==selected) {
            if(IsWindow(action.window)){activate_window(action.window);if(action.command)PostMessageW(action.window,WM_SYSCOMMAND,action.command,0);}break;
        }
        scene.machine.hold(Hold::ContextMenu,false,now());
        if(GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
        schedule();
    }
    void control_menu(POINT point,float x,float y,bool window_menu=false,bool keyboard=false) {
        const auto count=scene.shown_groups();
        for(size_t i=0;i<count;++i)if(inside(scene.app_rect(i),x,y)){if(scene.overflow(i))overflow_menu();else app_menu(i,point,window_menu,keyboard);return;}
        if(inside(scene.launcher_rect(),x,y)) {
            dismiss_panel();
            std::vector<JumpSection> sections(2);
            add_entry(sections[0],1,L"Search apps and files",L'\xE721');
            add_entry(sections[0],2,L"Settings",L'\xE713');add_entry(sections[0],3,L"Task Manager",L'\xE9D9');
            add_entry(sections[0],4,L"File Explorer",L'\xEC50');add_entry(sections[0],5,L"Terminal",L'\xE756');
            if(live_dock)add_entry(sections[1],6,windows_key.enabled()?L"Windows key opens Start":L"Windows key opens MinimaDock",L'\xE765');
            add_entry(sections[1],7,L"Open Start menu",L'\xE8FC');
            add_entry(sections[1],8,L"MinimaDock settings\x2026",L'\xE771');
            glass_menu(std::move(sections),scene.launcher_rect(),keyboard,[this](UINT id) {
                const wchar_t* paths[]={L"ms-settings:",L"taskmgr.exe",L"explorer.exe",L"wt.exe"};
                if(id==1)windows_key_pressed_or_open();
                else if(id>=2&&id<=5)launch(paths[id-2]);
                else if(id==6){const bool on=!windows_key.enabled();save_windows_key_setting(on);windows_key.enable(on);scene.windows_key_on=on;}
                else if(id==7)open_start_menu();
                else if(id==8)open_customize();
            });
            return;
        }
        if(scene.machine.progress()>.85f&&(inside(scene.quick_settings_rect(),x,y)||inside(scene.clock_rect(),x,y))) {
            const bool clock=inside(scene.clock_rect(),x,y);dismiss_panel();
            std::vector<JumpSection> sections(clock?2:1);
            if(clock) {
                add_entry(sections[0],1,L"Adjust date and time",L'\xE787');add_entry(sections[0],2,L"Notification settings",L'\xEA8F');
                add_entry(sections[1],3,L"Show next prayer on clock",prayer_settings.clock?L'\xE73E':L'\xE916');
                add_entry(sections[1],4,L"Prayer reminders",prayer_settings.reminders?L'\xE73E':L'\xEA8F');
                if(prayer_settings.source==PrayerSource::Website&&!prayer_settings.website.empty())add_entry(sections[1],5,L"Open masjid website",L'\xE774');
            } else {add_entry(sections[0],1,L"Network settings",L'\xE701');add_entry(sections[0],2,L"Sound settings",L'\xE767');}
            glass_menu(std::move(sections),clock?scene.clock_rect():scene.quick_settings_rect(),keyboard,[this,clock](UINT id) {
                if(id==1)launch(clock?L"ms-settings:dateandtime":L"ms-settings:network");
                else if(id==2)launch(clock?L"ms-settings:notifications":L"ms-settings:sound");
                else if(clock&&(id==3||id==4)) {
                    auto& value=id==3?prayer_settings.clock:prayer_settings.reminders;value=!value;
                    save_prayer_settings(prayer_settings);prayer_tick();
                } else if(clock&&id==5)launch(prayer_settings.website);
            });
            return;
        }
        desktop_menu(x,keyboard);
    }
    void schedule() {
        sync_visibility();
        scene.measure_media_title();
        // A long title scrolls only while the pointer is over the media controls:
        // a resting dock draws nothing (it was redrawing ~22 times a second).
        const bool over_media=scene.show_media&&inside(scene.media_rect(),scene.pointer_x,scene.pointer_y);
        scene.media_marquee.visible(scene.machine.state()==DockState::Expanded&&visibility.progress()>.99f&&scene.panel==Panel::None&&over_media,scene.reduced,now());
        if(scene.media_marquee.active()&&!media_timer)media_timer=SetTimer(hwnd,media_title_timer,33,nullptr)!=0;
        else if(!scene.media_marquee.active()&&media_timer){KillTimer(hwnd,media_title_timer);media_timer=false;}
        scene.clock_now=now();scene.sync_indicator(scene.clock_now);scene.sync_wings(scene.clock_now);scene.sync_panel_motion(scene.clock_now);scene.sync_launcher(scene.clock_now);
        const bool moving=scene.machine.needs_tick()||visibility.needs_tick()||scene.feedback_moving();
        // The launcher's caret blinks twice a second without the frame loop.
        if(scene.panel==Panel::Launcher&&!caret_blinking)caret_blinking=SetTimer(hwnd,caret_timer,530,nullptr)!=0;
        else if(scene.panel!=Panel::Launcher&&caret_blinking){KillTimer(hwnd,caret_timer);caret_blinking=false;}
        if(moving&&!animation_timer) animation_timer=SetTimer(hwnd,frame_timer,10,nullptr)!=0;
        else if(!moving&&animation_timer) { KillTimer(hwnd,frame_timer); animation_timer=false; }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void update_preferences() {
        BOOL animations=TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast); SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        const auto& s=dock_settings;
        scene.reduced=s.motion==DockSettings::Motion::Reduced||(s.motion==DockSettings::Motion::Windows&&!animations);
        scene.high_contrast=(contrast.dwFlags&HCF_HIGHCONTRASTON)!=0;
        const float icon=static_cast<float>(s.icon);const bool icons_changed=icon!=scene.icon_px;
        scene.icon_px=icon;scene.hover_grow=static_cast<float>(s.magnify)/100;scene.show_media=s.media;scene.show_date=s.date;
        scene.bar=s.style==DockSettings::Style::Bar;scene.bar_left=s.bar_left;
        scene.machine.hold(Hold::Bar,scene.bar,now());
        if(scene.clock24!=s.clock24){scene.clock24=s.clock24;scene.update_clock();}
        if(icons_changed&&scene.target)scene.rebuild_icons();
        scene.machine.reduced_motion(scene.reduced);
        if(scene.reduced){const bool exiting=scene.exiting_panel!=Panel::None;scene.stop_feedback();if(exiting&&scene.desktop)desktop_position();}
        visibility.reduced_motion(scene.reduced);
        calendar.preferences(scene.reduced);
        scene.machine.hold(Hold::Flyout,flyout_open(),now());
        scene.transparency=dock_transparency(dock_settings);
        scene.pal=dock_palette(dock_settings);
    }
    void scan(bool full) {
        // A request made while a scan runs is replayed after it, keeping the
        // strongest requested kind (a pin change needs a full inventory).
        full=full||scan_full_pending;
        if(busy) { rescan=true; scan_full_pending=full; return; }
        scan_full_pending=false;busy=true;
        if(worker.joinable()) worker.join();
        worker=std::jthread([this,full](std::stop_token stop) {
            const auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            std::unique_ptr<Inventory> data;
            try {
                data=std::make_unique<Inventory>();
                if(full) *data=collect_inventory(GetCurrentProcessId()); else data->windows=collect_windows(GetCurrentProcessId());
            } catch(...) {data.reset();}
            if(SUCCEEDED(hr)) CoUninitialize();
            // Always report completion; the GUI thread owns the busy flag.
            if(!stop.stop_requested()) { std::lock_guard lock(mutex); pending=std::move(data); pending_full=full; PostMessageW(hwnd,inventory_ready,0,0); }
        });
    }
    void set_panel(Panel p,bool animated=true) {
        const bool changed=scene.panel!=p;
        if(p!=Panel::None){close_previews();scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);}
        const auto previous=scene.drawn_panel();
        if(changed&&p==Panel::Calendar) {
            const auto foreground=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
            calendar_previous=foreground&&pid!=GetCurrentProcessId()?foreground:last_foreground;
        }
        const auto was=scene.panel;
        scene.panel=p;
        if(changed&&p!=Panel::None)panel_opened=GetTickCount();
        watch_popup(changed);
        if(p!=Panel::Calendar)calendar.close(animated&&scene.machine.visible());
        if(p!=Panel::Tray){tray.close();KillTimer(hwnd,tray_timer);}
        if(changed||!animated) {
            const bool instant=scene.reduced||!animated||!scene.machine.visible();
            if(p==Panel::None&&previous!=Panel::None&&previous!=Panel::Calendar) {
                scene.exiting_panel=previous;scene.panel_reveal.close(now(),instant);
                if(scene.panel_reveal.hidden())scene.exiting_panel=Panel::None;
            } else if(p==Panel::Calendar) {scene.exiting_panel=Panel::None;scene.panel_reveal.settle();}
            else if(p!=Panel::None) {
                if(scene.exiting_panel==p)scene.panel_reveal.open(now(),instant);
                else scene.panel_reveal.start(now(),instant);
                scene.exiting_panel=Panel::None;
            }
        }
        // After the reveal has reversed: its slower reads must not cost a frame.
        if(changed&&p==Panel::Settings)settings_opened();
        else if(changed&&was==Panel::Settings)settings_closed();
        if(changed&&p==Panel::Launcher)launcher_opened();
        else if(changed&&was==Panel::Launcher)launcher_closed();
        if((changed||!animated)&&scene.desktop)desktop_position();
        scene.machine.hold(Hold::Flyout,flyout_open(),now());
        if((p==Panel::Launcher||p==Panel::Settings)&&scene.desktop) { SetForegroundWindow(hwnd);SetFocus(hwnd); }
        else if(p==Panel::Launcher) { layout_search(); ShowWindow(search,SW_SHOW); SetFocus(search); }
        else { ShowWindow(search,SW_HIDE); if(GetFocus()==search) SetFocus(hwnd); }
        schedule();
        if(changed&&scene.desktop&&layer_presented)present_now();
        if(changed&&p==Panel::Calendar) {
            const auto r=scene.clock_rect();POINT origin{};ClientToScreen(hwnd,&origin);const auto scale=dpi/96;
            const RECT anchor{origin.x+static_cast<LONG>(std::lround(r.left*scale)),origin.y+static_cast<LONG>(std::lround(r.top*scale)),origin.x+static_cast<LONG>(std::lround(r.right*scale)),origin.y+static_cast<LONG>(std::lround(r.bottom*scale))};
            calendar.open(hwnd,anchor,scene.reduced);
        }
        if(changed&&p==Panel::Tray&&scene.native_tray) {scene.tray_scroll=0;scene.tray_focus=-1;SetForegroundWindow(hwnd);SetFocus(hwnd);
            const auto r=scene.tray_rect();POINT origin{};ClientToScreen(hwnd,&origin);const auto scale=dpi/96;
            const RECT anchor{origin.x+static_cast<LONG>(std::lround(r.left*scale)),origin.y+static_cast<LONG>(std::lround((scene.dock().top)*scale)),origin.x+static_cast<LONG>(std::lround(r.right*scale)),origin.y+static_cast<LONG>(std::lround(r.bottom*scale))};
            if(!tray.open(anchor,static_cast<UINT>(dpi))) {scene.native_tray=false;desktop_position();schedule();}
        }
    }
    bool activate_tray(unsigned long long key,bool context,POINT point,bool double_click=false,bool keyboard=false) {
        const bool sent=tray.activate(key,context,point,double_click,keyboard);
        // The app's own menu loop keeps its window foreground; watch for its end.
        if(sent&&context)SetTimer(hwnd,tray_timer,50,nullptr);
        return sent;
    }
    void layout_search() {
        const auto r=scene.panel_rect(); const float scale=dpi/96;
        MoveWindow(search,static_cast<int>((r.left+32)*scale),static_cast<int>((r.top+65)*scale),static_cast<int>((r.right-r.left-64)*scale),static_cast<int>(24*scale),TRUE);
    }
    // Fire just after each minute boundary so the displayed minute never lags.
    void arm_clock() {
        SYSTEMTIME time{};GetLocalTime(&time);
        const UINT delay=(60-time.wSecond)*1000u-time.wMilliseconds+50u;
        SetTimer(hwnd,clock_timer,std::clamp(delay,50u,60050u),nullptr);
    }
    void refresh_clock() {
        scene.update_clock();arm_clock();prayer_tick();
        if(scene.machine.progress()>.01f)InvalidateRect(hwnd,nullptr,FALSE);
    }
    void launch(const std::wstring& path,const wchar_t* verb=L"open") {
        SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;info.hwnd=hwnd;info.lpVerb=verb;info.lpFile=path.c_str();info.nShow=SW_SHOWNORMAL;
        if(ShellExecuteExW(&info))return;
        const DWORD error=GetLastError();
        if(error==ERROR_CANCELLED)return; // Declined elevation prompt.
        // Packaged apps: activate by app ID, then let Explorer try.
        constexpr std::wstring_view apps=L"shell:AppsFolder\\";
        if(path.size()>apps.size()&&_wcsnicmp(path.c_str(),apps.data(),apps.size())==0) {
            const auto id=path.substr(apps.size());
            if(id.find(L'!')!=std::wstring::npos&&activate_package(id))return;
            const auto argument=L"\""+path+L"\"";
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",L"explorer.exe",argument.c_str(),nullptr,SW_SHOWNORMAL))>32)return;
        }
        wchar_t code[16]{};swprintf(code,16,L"%lu",error);
        const auto message=L"Windows could not open this item (error "+std::wstring(code)+L").\n\n"+path+L"\n\nIts original shortcut or file association has been preserved.";
        MessageBoxW(hwnd,message.c_str(),L"MinimaDock",MB_OK|MB_ICONERROR);
    }
    static bool activate_package(const std::wstring& app_id) {
        ComPtr<IApplicationActivationManager> manager;DWORD process=0;
        return SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(manager.put())))&&
               SUCCEEDED(manager->ActivateApplication(app_id.c_str(),nullptr,AO_NONE,&process));
    }
    // File Explorer reuses a window already showing the target folder, while
    // the Windows taskbar always opens a new one: start explorer.exe directly.
    static bool explorer_app(const Group& group) {
        const auto name=std::filesystem::path(group.executable).filename().wstring();
        return _wcsicmp(name.c_str(),L"explorer.exe")==0||_wcsicmp(group.app_id.c_str(),L"Microsoft.Windows.Explorer")==0;
    }
    // explorer.exe arguments for a pinned folder shortcut ("" opens the default page).
    static std::wstring explorer_arguments(const std::wstring& path) {
        if(path.size()<4||_wcsicmp(path.c_str()+path.size()-4,L".lnk")!=0)return {};
        ComPtr<IShellLinkW> link;ComPtr<IPersistFile> file;
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.put())))||
           FAILED(link->QueryInterface(IID_PPV_ARGS(file.put())))||FAILED(file->Load(path.c_str(),STGM_READ)))return {};
        wchar_t target[MAX_PATH]{};
        if(SUCCEEDED(link->GetPath(target,MAX_PATH,nullptr,0))&&target[0]) {
            if(_wcsicmp(std::filesystem::path(target).filename().c_str(),L"explorer.exe")==0){wchar_t arguments[1024]{};link->GetArguments(arguments,1024);return arguments;}
            const auto attributes=GetFileAttributesW(target);
            return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)?L"\""+std::wstring(target)+L"\"":std::wstring();
        }
        PIDLIST_ABSOLUTE id=nullptr;std::wstring result;
        if(SUCCEEDED(link->GetIDList(&id))&&id) {
            ComPtr<IShellItem> item;SFGAOF folder=0;PWSTR name=nullptr;
            if(SUCCEEDED(SHCreateItemFromIDList(id,IID_PPV_ARGS(item.put())))&&SUCCEEDED(item->GetAttributes(SFGAO_FOLDER,&folder))&&(folder&SFGAO_FOLDER)&&
               SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&name))){result=L"\""+std::wstring(name)+L"\"";CoTaskMemFree(name);}
            CoTaskMemFree(id);
        }
        return result;
    }
    // Opens a new instance, as the Windows taskbar does (`elevated`: Ctrl+Shift).
    void launch_app(const Group& group,bool elevated=false) {
        const auto target=launch_target(group);
        if(!elevated&&explorer_app(group)) {
            const auto arguments=explorer_arguments(target);
            const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",L"explorer.exe",arguments.empty()?nullptr:arguments.c_str(),nullptr,SW_SHOWNORMAL));
            if(result>32)return;
        }
        launch(target,elevated?L"runas":L"open");
    }
    // Owner-side TrackPopupMenu contract: the dock never activates on click, so
    // take foreground for the menu loop and post WM_NULL afterwards. Otherwise
    // the menu does not dismiss on an outside click.
    UINT track_menu(HMENU menu,UINT flags,POINT point) {
        scene.machine.hold(Hold::ContextMenu,true,now());SetForegroundWindow(hwnd);
        const auto selected=static_cast<UINT>(TrackPopupMenu(menu,flags|TPM_RETURNCMD,point.x,point.y,0,hwnd,nullptr));
        PostMessageW(hwnd,WM_NULL,0,0);scene.machine.hold(Hold::ContextMenu,false,now());
        return selected;
    }
    void return_foreground() {if(GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);}
    // The group's window that currently has foreground (the dock never takes
    // it on click), or null when another app is active.
    HWND active_window_of(const std::vector<HWND>& windows) const {
        auto foreground=GetForegroundWindow();
        DWORD pid=0;if(foreground)GetWindowThreadProcessId(foreground,&pid);
        if(!foreground||pid==GetCurrentProcessId())foreground=last_foreground;
        if(!foreground||IsIconic(foreground))return nullptr;
        return std::find(windows.begin(),windows.end(),foreground)!=windows.end()?foreground:nullptr;
    }
    // Most recently used of the windows: the first one in the desktop z-order.
    static HWND recent_window(const std::vector<HWND>& windows) {
        for(auto window=GetTopWindow(nullptr);window;window=GetWindow(window,GW_HWNDNEXT))
            if(std::find(windows.begin(),windows.end(),window)!=windows.end())return window;
        return windows.front();
    }
    void group_action(size_t group) {
            // Copy: nested native menu pumps can rescan inventory and replace groups.
            const auto g=scene.groups[group];
            if((GetKeyState(VK_SHIFT)&0x8000)&&!g.launch_path.empty()) {
                launch_app(g,(GetKeyState(VK_CONTROL)&0x8000)!=0);close_previews(false);return;
            }
            if(g.windows.empty()) {launch_app(g);if(scene.desktop)set_panel(Panel::None);return;}
            // As the Windows taskbar: an inactive app comes forward on its most
            // recently used window; clicking the active app minimizes its only
            // window, or offers the window previews when it has several.
            const auto active=active_window_of(g.windows);
            if(g.windows.size()==1&&active) PostMessageW(active,WM_SYSCOMMAND,SC_MINIMIZE,0);
            else if(!active) {
                close_previews(false);
                if(!activate_window(recent_window(g.windows))) MessageBoxW(hwnd,L"Windows did not allow activation of this window.",L"MinimaDock",MB_OK);
            }
            else if(previews_supported()) {open_previews(group);return;}
            else {
                auto menu=CreatePopupMenu();
                for(size_t i=0;i<g.windows.size();++i) { wchar_t title[512]{}; GetWindowTextW(g.windows[i],title,512); AppendMenuW(menu,MF_STRING,static_cast<UINT_PTR>(i+1),menu_label(title[0]?title:g.name).c_str()); }
                POINT pt{}; GetCursorPos(&pt); const auto selected=track_menu(menu,TPM_NONOTIFY,pt);
                DestroyMenu(menu);
                if(selected>0&&selected<=g.windows.size()&&IsWindow(g.windows[selected-1])) activate_window(g.windows[selected-1]);
                else return_foreground();
                schedule();
            }
            if(scene.desktop)set_panel(Panel::None);
    }
    void overflow_menu() {
        auto menu=CreatePopupMenu();
        const auto first=scene.shown_groups()-1;
        // Snapshot identities: the group list can change during the menu loop.
        std::vector<Group> entries(scene.groups.begin()+static_cast<std::ptrdiff_t>(first),scene.groups.end());
        for(size_t i=0;i<entries.size();++i)AppendMenuW(menu,MF_STRING,static_cast<UINT_PTR>(i+1),menu_label(entries[i].name).c_str());
        POINT point{};GetCursorPos(&point);const auto selected=track_menu(menu,TPM_NONOTIFY,point);
        DestroyMenu(menu);
        if(selected>0&&selected<=entries.size()) {
            const auto& chosen=entries[selected-1];
            const auto found=std::find_if(scene.groups.begin(),scene.groups.end(),[&](const Group& g){return Scene::same_identity(g,chosen);});
            if(found!=scene.groups.end())group_action(static_cast<size_t>(found-scene.groups.begin()));
            else return_foreground();
        } else return_foreground();
        schedule();
    }
    void desktop_result(HRESULT result) {
        desktop_action_result=result;
        if(result!=E_PENDING&&FAILED(result)) {
            wchar_t details[256]{};
            swprintf(details,256,L"Some windows did not respond to Show Desktop (error 0x%08lX).",static_cast<unsigned long>(result));
            MessageBoxW(hwnd,details,L"MinimaDock",MB_OK|MB_ICONERROR);
        }
    }
    void action(int index) {
        if(!scene.machine.visible())return;
        const auto time=now();
        const auto count=static_cast<int>(scene.shown_groups());
        if(index==count+4&&scene.machine.progress()<=.85f)return;
        if(index>=count+5&&index<=count+7) {
            if(scene.panel!=Panel::None)dismiss_panel();
            const auto control=static_cast<size_t>(index-count-5);
            const auto command=control==1?(scene.media.playing?MediaCommand::Pause:MediaCommand::Play):static_cast<MediaCommand>(control);
            if(scene.media_enabled(control)&&media.command(command,scene.media)){
                scene.media=media.snapshot();scene.media_pulses[control].start(time,scene.reduced);schedule();
                if(scene.desktop&&layer_presented)present_now();
            }
            return;
        }
        scene.tooltip_target=-1;KillTimer(hwnd,hover_timer);KillTimer(hwnd,preview_delay_timer);
        if(!(index>0&&index<=count&&scene.groups[static_cast<size_t>(index-1)].windows.size()>1))close_previews();
        if(index==0)scene.launcher_pulse.start(time,scene.reduced);
        else if(index>0&&index<=count)scene.groups[static_cast<size_t>(index-1)].pulse.start(time,scene.reduced);
        else if(index==count+1)scene.tray_pulse.start(time,scene.reduced);
        else if(index==count+2)scene.settings_pulse.start(time,scene.reduced);
        else if(index==count+3)scene.clock_pulse.start(time,scene.reduced);
        else if(index==count+4)scene.desktop_pulse.start(time,scene.reduced);
        schedule();
        if(scene.desktop&&layer_presented)present_now();
        if(index==0) { set_panel(scene.panel==Panel::Launcher?Panel::None:Panel::Launcher); return; }
        if(index<=count&&index>0) { const auto group=static_cast<size_t>(index-1);if(scene.overflow(group))overflow_menu();else group_action(group); }
        else if(index==count+1) set_panel(scene.panel==Panel::Tray?Panel::None:Panel::Tray);
        else if(index==count+2) set_panel(scene.panel==Panel::Settings?Panel::None:Panel::Settings);
        else if(index==count+3) set_panel(scene.panel==Panel::Calendar?Panel::None:Panel::Calendar);
        else if(index==count+4) {
            ++desktop_activations;
            set_panel(Panel::None);scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
            desktop_result(desktop_toggle.toggle(hwnd,last_foreground));
            if(desktop_action_result==E_PENDING)SetTimer(hwnd,desktop_timer,30,nullptr);
        }
    }
    void click(float x,float y) {
        if(!scene.machine.visible()) return;
        if(scene.panel!=Panel::None&&scene.panel!=Panel::Calendar) {
            const auto r=scene.panel_rect();
            if(inside(r,x,y)) {
                const float center=(r.left+r.right)/2,scale=scene.panel_reveal.scale();
                x=center+(x-center)/scale;y=r.bottom+(y-r.bottom)/scale;
                if(scene.panel==Panel::Launcher) {
                    launcher_click(x,y);
                } else if(scene.panel==Panel::Settings) {
                    if(scene.settings_page!=Scene::SettingsPage::Main){settings_page_click(x,y);return;}
                    if(quick_click(x,y))return;
                    for(size_t i=0;i<tiles_per_page;++i)if(const auto tile=scene.slot_tile(i);tile!=Tile::Count&&inside(scene.settings_tile(i),x,y)){settings_tile_click(tile,Scene::split_tile(tile)&&inside(scene.tile_arrow(i),x,y));return;}
                    if(inside(D2D1::RectF(r.right-46,r.top+245,r.right-18,r.top+277),x,y))launch(L"ms-settings:sound");
                    if(volume.available()&&inside(scene.volume_mute_rect(),x,y)){volume.set_muted(!volume.muted());read_volume();schedule();}
                    if(inside(D2D1::RectF(r.right-45,r.bottom-36,r.right-16,r.bottom-5),x,y))launch(L"ms-settings:");
                    if(!scene.recovery_entry.empty() && inside(D2D1::RectF(r.left+20,r.bottom-33,r.right-88,r.bottom-8),x,y)) {
                        if(scene.desktop) {return_to_windows();return;}
                        const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd,L"open",scene.recovery_entry.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
                        if(result>32) { scene.machine.recover(now()); PostMessageW(hwnd,WM_CLOSE,0,0); }
                        else MessageBoxW(hwnd,L"The recovery launcher could not start. Use the documented Task Manager recovery command.",L"MinimaDock",MB_OK|MB_ICONERROR);
                    }
                } else if(scene.panel==Panel::Tray) {
                    const auto hit=scene.tray_hit(x,y);if(hit>=0){POINT point{static_cast<LONG>(std::lround(x*dpi/96)),static_cast<LONG>(std::lround(y*dpi/96))};ClientToScreen(hwnd,&point);activate_tray(scene.tray_items[static_cast<size_t>(hit)].key,false,point);return;}
                    if(!scene.native_tray&&inside(D2D1::RectF(r.left+20,r.top+137,r.right-20,r.top+185),x,y))launch(L"ms-settings:notifications");
                    if(scene.groups.size()>scene.shown_groups()&&inside(D2D1::RectF(r.left+20,r.top+198,r.right-20,r.top+246),x,y))overflow_menu();
                }
                return;
            }
            if(!inside(scene.dock(),x,y)) { set_panel(Panel::None); return; }
        }
        const auto r=scene.dock();
        if(inside(r,x,y)) {
            if(scene.machine.progress()>.85f&&inside(scene.desktop_button_rect(),x,y)){action(static_cast<int>(scene.shown_groups())+4);return;}
            if(inside(scene.launcher_rect(),x,y)) {action(0);return;}
            for(size_t i=0;i<scene.shown_groups();++i)if(inside(scene.app_rect(i),x,y)){action(static_cast<int>(i)+1);return;}
            if(scene.show_media&&scene.machine.progress()>.01f&&inside(scene.media_rect(),x,y)) {
                for(size_t i=0;i<3;++i)if(inside(scene.media_button(i),x,y)){action(static_cast<int>(scene.shown_groups())+5+static_cast<int>(i));return;}
            }
            if(scene.machine.progress()>.85f&&inside(scene.system_rect(),x,y)) {
                const auto s=scene.system_rect();const auto count=static_cast<int>(scene.shown_groups());
                if(x<s.left+24)action(count+1);
                else if(inside(scene.quick_settings_rect(),x,y))action(count+2);
                else if(inside(scene.clock_rect(),x,y))action(count+3);
                return;
            }
        }
        if(scene.panel!=Panel::None)dismiss_panel();
    }
    void paint() {
        PAINTSTRUCT ps{}; BeginPaint(hwnd,&ps);
        if(scene.desktop) {
            // EndPaint even when a rendering failure triggers supervised shutdown.
            try { paint_layer(); } catch(...) {EndPaint(hwnd,&ps);throw;}
            EndPaint(hwnd,&ps);return;
        }
        if(!render) {
            RECT r{}; GetClientRect(hwnd,&r);
            const auto hr=scene.factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(r.right,r.bottom)),render.put());
            if(FAILED(hr)) { EndPaint(hwnd,&ps); return; }
            render->SetDpi(dpi,dpi); scene.bind(render.get());
        }
        scene.draw(); const auto hr=render->EndDraw();
        if(hr==D2DERR_RECREATE_TARGET) { scene.brush.reset(); scene.target.reset(); render.reset(); }
        EndPaint(hwnd,&ps);
    }
    // Present synchronously for immediate click feedback, then drop the queued
    // WM_PAINT for the same state; later changes invalidate again.
    void present_now() {paint_layer();ValidateRect(hwnd,nullptr);}
    // A single failed frame (device/GDI pressure, a display transition) releases
    // the layer and retries shortly. Only repeated failures end the supervised
    // session, which then restores the Windows taskbar.
    void paint_layer() {
        try {render_layer();render_failures=0;}
        catch(HRESULT) {
            presenting_layer=false;release_layer();
            if(++render_failures>=3)throw;
            SetTimer(hwnd,render_retry_timer,100,nullptr);
        }
    }
    // Hit-test/draw region: everything visible plus its soft shadow. Outside it
    // clicks reach the windows below, as with the layered window's alpha.
    void update_region(const RECT& position) {
        std::vector<RECT> rects;const float scale=dpi/96;
        const bool shown=scene.machine.visible()&&(!live_dock||visibility.wanted()||visibility.needs_tick());
        const float offset=live_dock?visibility.offset():0;
        // Rounded outward to an 8 px grid: a lifting icon or springing tooltip then
        // changes the window region a few times per animation, not every frame
        // (SetWindowRgn is costly for the compositor).
        const auto down=[](float v){return static_cast<LONG>(std::floor(v/8)*8);};
        const auto up=[](float v){return static_cast<LONG>(std::ceil(v/8)*8);};
        const auto add=[&](D2D1_RECT_F r,float side,float top,float bottom,float shift) {
            rects.push_back({down((r.left-side)*scale),down((r.top-top+shift)*scale),up((r.right+side)*scale),up((r.bottom+bottom+shift)*scale)});
        };
        if(shown) {
            add(scene.dock(),14,10,16,offset);
            const auto panel=scene.drawn_panel();
            if(panel!=Panel::None&&panel!=Panel::Calendar)add(scene.panel_rect(),24,18,34,offset);
            for(const auto& r:scene.overlay_rects)add(r,8,6,10,offset);
            // Pointer bridge: the final position accepts the pointer while the
            // pill is still sliding in over a maximized window.
            if(auto_hidden&&visibility.wanted())add(scene.dock(),0,0,0,0);
        }
        const auto same=rects.size()==region_rects.size()&&std::equal(rects.begin(),rects.end(),region_rects.begin(),[](const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;});
        if(same)return;
        region_rects=rects;
        auto region=CreateRectRgn(0,0,0,0);
        for(const auto& r:rects) {
            const int corner=std::min<int>(r.bottom-r.top,static_cast<int>(std::lround(48*scale)));
            if(auto part=CreateRoundRectRgn(r.left,r.top,r.right+1,r.bottom+1,corner,corner)){CombineRgn(region,region,part,RGN_OR);DeleteObject(part);}
        }
        (void)position;
        if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
    }
    // Frame cost diagnostics (microseconds, summed since start): WM_APP+100..104.
    std::array<unsigned long long,5> frame_us{}; // begin, draw, end+backdrops+present, region, total
    static unsigned long long micros(){LARGE_INTEGER c{},f{};QueryPerformanceCounter(&c);QueryPerformanceFrequency(&f);return static_cast<unsigned long long>(c.QuadPart*1000000/f.QuadPart);}
    void render_glass() {
        const auto t0=micros();
        sync_visibility();
        RECT position{};
        if(layer_bounds_pending)position=pending_layer_bounds;else GetWindowRect(hwnd,&position);
        const auto w=static_cast<UINT>(position.right-position.left),h=static_cast<UINT>(position.bottom-position.top);
        if(!w||!h)return;
        RECT current{};GetWindowRect(hwnd,&current);
        if(!EqualRect(&current,&position))SetWindowPos(hwnd,nullptr,position.left,position.top,static_cast<int>(w),static_cast<int>(h),SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER);
        if(!glass.attached())glass.attach(hwnd,scene.factory.get());
        presenting_layer=true;
        const auto t1=micros();
        D2D1_MATRIX_3X2_F origin{};auto context=glass.begin(w,h,dpi,origin);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto t2=micros();frame_us[0]+=t2-t1;
        scene.live_glass=scene.transparency&&!scene.high_contrast;
        if(!glass_bound){scene.bind(context);glass_bound=true;}else scene.retarget(context);
        scene.external_draw=true;scene.base_transform=origin;
        scene.draw();
        scene.external_draw=false;
        const auto t3=micros();frame_us[1]+=t3-t2;
        glass.end();
        glass.backdrops(scene.painter.shapes,dpi);
        glass.present(live_dock?visibility.opacity():1.0f,live_dock?visibility.offset()*dpi/96:0.0f,scene.live_glass);
        const auto t4=micros();frame_us[2]+=t4-t3;
        update_region(position);
        const auto t5=micros();frame_us[3]+=t5-t4;frame_us[4]+=t5-t0;
        presenting_layer=false;
        const POINT destination{position.left,position.top};
        if(scene.panel==Panel::Tray&&scene.native_tray){const auto scale=dpi/96;for(size_t i=0;i<std::min<size_t>(30,scene.tray_items.size()-std::min(scene.tray_scroll,scene.tray_items.size()));++i){const auto r=scene.tray_tile(i);tray.set_bounds(scene.tray_items[i+scene.tray_scroll].key,{destination.x+static_cast<LONG>(std::lround(r.left*scale)),destination.y+static_cast<LONG>(std::lround(r.top*scale)),destination.x+static_cast<LONG>(std::lround(r.right*scale)),destination.y+static_cast<LONG>(std::lround(r.bottom*scale))});}}
        layer_width=w;layer_height=h;layer_bounds_pending=false;layer_presented=true;presented_height=h;++layer_frames;
        presented_center_y=position.top+static_cast<LONG>(std::lround(scene.layout().center_y*dpi/96));
    }
    void render_layer() {
        if(glass_mode){render_glass();return;}
        sync_visibility();
        RECT position{};
        if(layer_bounds_pending)position=pending_layer_bounds;else GetWindowRect(hwnd,&position);
        const auto w=static_cast<UINT>(position.right-position.left),h=static_cast<UINT>(position.bottom-position.top);
        if(!w||!h)return;
        // Reserve room for the largest flyout (the launcher) once; opening/closing
        // panels then retains the render target and its icon resources. Only the
        // visible part is copied and presented.
        const auto reserve=std::max(w,static_cast<UINT>(std::ceil((Scene::launch_width+80)*dpi/96)));
        const auto capacity=std::max(h,static_cast<UINT>(std::ceil((Scene::launch_height+100)*dpi/96)));
        if(!layer_render||reserve>layer_width||capacity>layer_height) {
            release_layer();layer_width=std::max(reserve,layer_width);layer_height=std::max(capacity,layer_height);++layer_allocations;
            BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bitmap.bmiHeader.biWidth=static_cast<LONG>(layer_width);
            bitmap.bmiHeader.biHeight=-static_cast<LONG>(layer_height);bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;bitmap.bmiHeader.biCompression=BI_RGB;
            layer_dc=CreateCompatibleDC(nullptr);layer_dib=CreateDIBSection(layer_dc,&bitmap,DIB_RGB_COLORS,&layer_pixels,nullptr,0);
            if(!layer_dc||!layer_dib||!layer_pixels)throw E_OUTOFMEMORY;
            layer_previous=SelectObject(layer_dc,layer_dib);
            check(scene.wic->CreateBitmap(layer_width,layer_height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,layer_bitmap.put()));
            check(scene.factory->CreateWicBitmapRenderTarget(layer_bitmap.get(),D2D1::RenderTargetProperties(),layer_render.put()));
            layer_render->SetDpi(dpi,dpi);layer_render->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);scene.bind(layer_render.get());
        }
        layer_render->SetTransform(D2D1::Matrix3x2F::Translation(0,live_dock?visibility.offset():0));
        scene.draw();check(layer_render->EndDraw());layer_render->SetTransform(D2D1::Matrix3x2F::Identity());
        WICRect visible{0,0,static_cast<INT>(w),static_cast<INT>(h)};const size_t stride=layer_width; // Pixels per DIB row.
        check(layer_bitmap->CopyPixels(&visible,layer_width*4,layer_width*h*4,static_cast<BYTE*>(layer_pixels)));
        if(live_dock) {
            const auto alpha=visibility.opacity();auto pixels=static_cast<BYTE*>(layer_pixels);
            if(alpha==0)for(size_t y=0;y<h;++y)std::fill_n(pixels+y*stride*4,static_cast<size_t>(w)*4,BYTE{0});
            else if(alpha<1) {
                // Premultiplied pixels scale uniformly. Fixed-point 8.8 keeps this
                // a vectorizable integer loop instead of a lround per byte.
                const unsigned scale=static_cast<unsigned>(std::lround(alpha*256));
                for(size_t y=0;y<h;++y){auto* row=pixels+y*stride*4;for(size_t i=0;i<static_cast<size_t>(w)*4;++i)row[i]=static_cast<BYTE>((row[i]*scale+128)>>8);}
            }
            // A nearly transparent final-position hit surface lets the pointer
            // enter a moving pill without chasing the animated geometry.
            if(auto_hidden&&visibility.wanted()) {
                const auto r=scene.dock();const float scale=dpi/96;
                const auto left=std::clamp(static_cast<int>(r.left*scale),0,static_cast<int>(w)),right=std::clamp(static_cast<int>(r.right*scale),0,static_cast<int>(w));
                const auto top=std::clamp(static_cast<int>(r.top*scale),0,static_cast<int>(h)),bottom=std::clamp(static_cast<int>(r.bottom*scale),0,static_cast<int>(h));
                for(int y=top;y<bottom;++y)for(int x=left;x<right;++x){auto& a=pixels[(static_cast<size_t>(y)*stride+static_cast<size_t>(x))*4+3];if(!a)a=1;}
            }
        }
        POINT destination{position.left,position.top},source{0,0};SIZE size{static_cast<LONG>(w),static_cast<LONG>(h)};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        presenting_layer=true;
        const bool updated=UpdateLayeredWindow(hwnd,nullptr,&destination,&size,layer_dc,&source,0,&blend,ULW_ALPHA)!=FALSE;
        if(updated&&scene.panel==Panel::Tray&&scene.native_tray){const auto scale=dpi/96;for(size_t i=0;i<std::min<size_t>(30,scene.tray_items.size()-std::min(scene.tray_scroll,scene.tray_items.size()));++i){const auto r=scene.tray_tile(i);tray.set_bounds(scene.tray_items[i+scene.tray_scroll].key,{destination.x+static_cast<LONG>(std::lround(r.left*scale)),destination.y+static_cast<LONG>(std::lround(r.top*scale)),destination.x+static_cast<LONG>(std::lround(r.right*scale)),destination.y+static_cast<LONG>(std::lround(r.bottom*scale))});}}
        presenting_layer=false;
        if(!updated)throw E_FAIL;
        layer_bounds_pending=false;layer_presented=true;presented_height=h;++layer_frames;
        presented_center_y=position.top+static_cast<LONG>(std::lround(scene.layout().center_y*dpi/96));
    }
    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        if(shell_menu){LRESULT result=0;if(shell_menu->message(msg,w,l,result))return result;}
        if(msg==WM_MENURBUTTONUP){app_menu_right_click(w,reinterpret_cast<HMENU>(l));return 0;}
        if(shell_hook&&msg==shell_hook) {
            const auto window=reinterpret_cast<HWND>(l);
            if(w==HSHELL_FLASH){if(scene.attention.insert(window).second)scene.attention_start=now();schedule();}
            else if(w==HSHELL_WINDOWACTIVATED||w==HSHELL_RUDEAPPACTIVATED||w==HSHELL_WINDOWDESTROYED){if(scene.attention.erase(window))schedule();}
            return 0;
        }
        switch(msg) {
        case WM_CREATE: {
            scene.initialize(); scene.quick=load_quick_layout(); update_preferences(); dpi=static_cast<float>(GetDpiForWindow(hwnd));
            if(glass_mode)glass.attach(hwnd,scene.factory.get());
            wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
            auto parent=std::filesystem::path(module).parent_path();
            for(unsigned i=0;i<3;++i) {
                const auto candidate=parent/L"recovery-bundle/Restore-Windows.cmd";
                if(std::filesystem::is_regular_file(candidate)) { scene.recovery_entry=candidate.wstring(); break; }
                parent=parent.parent_path();
            }
            if(scene.desktop) scene.recovery_entry=L"session-revert";
            search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,hwnd,reinterpret_cast<HMENU>(101),nullptr,nullptr);
            edit_background=CreateSolidBrush(RGB(20,27,37));
            SendMessageW(search,EM_SETLIMITTEXT,120,0); SendMessageW(search,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(L"Search installed shortcuts"));
            wchar_t family[64]{};scene.fonts[0]->GetFontFamilyName(family,64);
            search_font=CreateFontW(-static_cast<int>(16*dpi/96),0,0,0,FW_LIGHT,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
            SendMessageW(search,WM_SETFONT,reinterpret_cast<WPARAM>(search_font),TRUE);
            event_queued=false;visibility_queued=false;names_queued=false;event_target=hwnd;
            hooks[0]=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            hooks[1]=SetWinEventHook(EVENT_OBJECT_SHOW,EVENT_OBJECT_HIDE,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            hooks[2]=SetWinEventHook(EVENT_OBJECT_NAMECHANGE,EVENT_OBJECT_NAMECHANGE,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
            if(live_dock) {
                hooks[4]=SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND,nullptr,window_event,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
                create_edge();visibility.reduced_motion(true);fullscreen();visibility.reduced_motion(scene.reduced);
            }
            arm_clock();
            media.start(hwnd);
            volume.start(hwnd);read_volume();
            connectivity.start(hwnd);updates.start(hwnd);brightness.start(hwnd);launcher_index.start(hwnd);file_search.start(hwnd);
            if(live_dock){const bool on=load_windows_key_setting();windows_key.enable(on);windows_key.start(hwnd);scene.windows_key_on=on;}
            reserve_bar(dock_settings.style==DockSettings::Style::Bar&&!dock_settings.bar_autohide);
            windows_pin_ids=pin_ids(windows_pins());pin_watch.start(hwnd,pins_folder_message,taskbar_pins_folder());
            prayer_settings=load_prayer_settings();prayers.start(hwnd,prayer_settings);
            if(live_dock)app_updater.start(hwnd,load_auto_update_setting());
            // Attention requests (FlashWindow) arrive as HSHELL_FLASH.
            shell_hook=RegisterWindowMessageW(L"SHELLHOOK");if(shell_hook&&!RegisterShellHookWindow(hwnd))shell_hook=0;
            if(scene.desktop)tray.prepare(hwnd);
            scan(true); return 0;
        }
        // Diagnostic: open previews for app slot w; 1 when the popup window exists.
        case WM_APP+92: open_previews(static_cast<size_t>(w)); return previews.window()&&IsWindow(previews.window())?1:0;
        case WM_APP+89: return static_cast<LRESULT>(scene.exiting_panel);
        case WM_APP+90: return static_cast<LRESULT>(scene.panel_reveal.scale()*1000);
        case WM_APP+91: return scene.control_pressed+1;
        case WM_APP+88: return desktop_activations;
        case WM_APP+87: return static_cast<LRESULT>(scene.dock().left*1000);
        case WM_APP+86: return static_cast<LRESULT>(scene.layout().launcher_x()*1000);
        case WM_APP+40: return layer_presented&&inventory_loaded&&(!scene.desktop||tray.ready())?1:0;
        case WM_APP+120: return bar_reserved?1:0; // Asked by the trial supervisor.
        // Read-only diagnostics used by native component checks; no external input hook.
        case WM_APP+41: return static_cast<LRESULT>(scene.machine.state());
        case WM_APP+42: return static_cast<LRESULT>(scene.shown_groups());
        case WM_APP+43: return static_cast<size_t>(w)<scene.shown_groups()?static_cast<LRESULT>((scene.app_x(static_cast<size_t>(w))-scene.width/2)*1000):0;
        case WM_APP+44: return static_cast<size_t>(w)<scene.shown_groups()?(scene.tile_running(static_cast<size_t>(w))?1:0)|(scene.groups[static_cast<size_t>(w)].pinned?2:0):0;
        case WM_APP+45: return static_cast<LRESULT>(scene.layout().height(scene.machine.progress())*1000);
        case WM_APP+46: {const auto r=scene.desktop_button_rect();return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+47: return static_cast<LRESULT>(scene.layout().center_y*1000);
        case WM_APP+48: return static_cast<LRESULT>(desktop_action_result);
        case WM_APP+49: return static_cast<LRESULT>(visibility.progress()*1000);
        case WM_APP+50: return auto_hidden?1:0;
        case WM_APP+51: return animation_timer?1:0;
        case WM_APP+52: return visibility.wanted()?1:0;
        case WM_APP+53: return reveal_hotkey?1:0;
        case WM_APP+54: {const auto r=scene.dock();return static_cast<LRESULT>((r.right-r.left)*1000);}
        case WM_APP+55: return edge_indicator_alpha;
        case WM_APP+56: return presented_center_y;
        case WM_APP+57: return glass_mode?glass.allocations():layer_allocations;
        case WM_APP+58: return layer_frames;
        case WM_APP+100:case WM_APP+101:case WM_APP+102:case WM_APP+103:case WM_APP+104: return static_cast<LRESULT>(frame_us[msg-(WM_APP+100)]);
        case WM_APP+59: return static_cast<LRESULT>(scene.panel);
        case WM_APP+60: return static_cast<LRESULT>(scene.active_pulses());
        case WM_APP+61: return static_cast<LRESULT>(scene.panel_reveal.opacity()*1000);
        case WM_APP+62: return static_cast<LRESULT>(scene.panel_reveal.offset()*1000);
        case WM_APP+63: return scene.pressed?1:0;
        case WM_APP+64: return scene.reduced?1:0;
        case WM_APP+65: {const auto r=scene.clock_rect();return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+66:return reinterpret_cast<LRESULT>(last_foreground);
        case WM_APP+67:return scene.media.available|(scene.media.playing<<1)|(scene.media.previous<<2)|(scene.media.toggle<<3)|(scene.media.next<<4)|(scene.media.unavailable<<5);
        case WM_APP+68:case WM_APP+69:case WM_APP+70:{const auto r=scene.media_button(msg-(WM_APP+68));return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+71:{const auto r=scene.media_button(0);return static_cast<LRESULT>((r.top+r.bottom)*500);}
        case WM_APP+72:return media_label_hash(scene.media.title);
        case WM_APP+73:return static_cast<LRESULT>(scene.media_marquee.offset()*1000);
        case WM_APP+74:return media_timer?1:0;
        case WM_APP+75:return tray.ready()|(tray.available()<<1)|((tray.window()!=nullptr)<<2);
        case WM_APP+76:return reinterpret_cast<LRESULT>(tray.window());
        case WM_APP+77:{const auto r=scene.tray_rect();return static_cast<LRESULT>((r.left+r.right)*500);}
        case WM_APP+78:return scene.panel==Panel::Tray&&scene.native_tray?1:0;
        case WM_APP+79:return tray.error();
        case WM_APP+80:return static_cast<LRESULT>(scene.tray_items.size());
        case WM_APP+81:return reinterpret_cast<LRESULT>(tray.receiver());
        case WM_APP+82:return static_cast<LRESULT>(scene.panel_rect().left*1000);
        case WM_APP+83:return static_cast<LRESULT>(scene.panel_rect().top*1000);
        case WM_APP+84:return static_cast<LRESULT>((scene.panel_rect().right-scene.panel_rect().left)*1000);
        case WM_APP+85:return static_cast<LRESULT>((scene.panel_rect().bottom-scene.panel_rect().top)*1000);
        case tray_ready_message:{
            const bool was_native=scene.native_tray;scene.native_tray=tray.available();scene.tray_items=tray.snapshot();scene.tray_focus=std::min(scene.tray_focus,static_cast<int>(scene.tray_items.size())-1);scene.tray_scroll=std::min(scene.tray_scroll,scene.tray_items.size()>30?((scene.tray_items.size()-1)/5)*5:0);scene.tray_icons.clear();if(scene.target){for(const auto& i:scene.tray_items)scene.tray_icons.push_back(scene.bitmap_icon(i.icon,20));scene.prune_icon_cache();}
            if(scene.panel==Panel::Tray&&was_native&&!scene.native_tray){KillTimer(hwnd,tray_timer);if(scene.desktop)desktop_position();}
            schedule();return 0;
        }
        case media_changed_message:scene.media=media.snapshot();schedule();return 0;
        case volume_changed_message:if(w==1)volume.refresh();read_volume();schedule();return 0;
        case connectivity_changed_message:scene.net=connectivity.snapshot();schedule();return 0;
        case brightness_changed_message:
            scene.brightness_available=brightness.available();if(!scene.brightness_active)scene.brightness_level=brightness.level();
            schedule();return 0;
        case updates_changed_message:scene.updates=updates.status();schedule();return 0;
        case prayer_times_message: {
            // Following the device: keep the latest fix (the worker never writes the registry).
            const auto located=prayers.snapshot().located;
            if(prayer_settings.source==PrayerSource::Location&&prayer_settings.locate==LocateMode::Device&&located.located&&!located.same_location(prayer_settings)) {
                prayer_settings.latitude=located.latitude;prayer_settings.longitude=located.longitude;prayer_settings.located=true;
                prayer_settings.place=located.place;prayer_settings.country=located.country;prayer_settings.located_day=located.located_day;
                save_prayer_settings(prayer_settings);customize.prayer_location(prayer_settings);
            }
            prayer_tick();customize.prayer_updated();return 0;
        }
        case app_update_message:customize.prayer_updated();return 0; // Redraws the settings window.
        case windows_key_message:windows_key_pressed();return 0;
        case file_search_message:
            if(w==0)files_arrived();
            else {scene.file_preview=file_search.preview_result();scene.preview_since=now();if(scene.panel==Panel::Launcher)schedule();}
            return 0;
        case launcher_index_message:
            scene.launch_apps=launcher_index.apps();scene.app_bitmaps.clear();
            scene.index_programs();scan(true); // Helper windows and pins resolve to their apps.
            if(scene.panel==Panel::Launcher){const auto selected=scene.launch_sel;scene.search_launcher();scene.menu_open=false;scene.launch_sel=std::min(selected,std::max(0,static_cast<int>(scene.launch_found.size())-1));schedule();}
            return 0;
        case alarm_event_message:
            if(static_cast<unsigned>(l)==alarm_session&&w){if(w==2)timer_start(5*60000);else timer_reset();}
            return 0;
        case jump_list_event_message:jump_list_event(static_cast<UINT>(w),static_cast<unsigned>(l));return 0;
        case control_list_message:control_list_event(static_cast<UINT>(w),static_cast<unsigned>(l));return 0;
        case pins_folder_message:SetTimer(hwnd,pins_timer,500,nullptr);return 0; // Settle bursts of changes.
        case previews_event_message:
            if(w==0){KillTimer(hwnd,previews_timer);previews_slot=-1;scene.machine.hold(Hold::Flyout,flyout_open(),now());pointer_hold();schedule();}
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:update_preferences();schedule();return 0;
        case calendar_closed_message:
            if(!calendar.window()&&(scene.panel==Panel::Calendar||scene.panel==Panel::None)) {
                set_panel(Panel::None);scene.focus=-1;scene.machine.hold(Hold::Keyboard,false,now());
                if((l||GetForegroundWindow()==hwnd)&&IsWindow(calendar_previous))SetForegroundWindow(calendar_previous);
                schedule();
            }return 0;
        case foreground_changed:
            if(scene.attention.erase(reinterpret_cast<HWND>(w)))schedule();
            // The event timestamp excludes queued activation from before opening.
            // Querying current foreground here can observe a transient null HWND.
            if(scene.panel!=Panel::None&&static_cast<LONG>(static_cast<DWORD>(l)-panel_opened)>=0) {
                const auto foreground=reinterpret_cast<HWND>(w);DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
                if(pid&&pid!=GetCurrentProcessId()&&!tray.owns(foreground)&&!tray.holds_menu()&&!(scene.machine.holds()&static_cast<unsigned>(Hold::ContextMenu)))dismiss_panel();
            }return 0;
        case WM_DISPLAYCHANGE: if(scene.desktop){release_layer();desktop_position();fullscreen();schedule();}return 0;
        case WM_MOUSEACTIVATE: {
            if(scene.desktop&&scene.panel!=Panel::Launcher) return MA_NOACTIVATE;
            return DefWindowProcW(hwnd,msg,w,l);
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_SIZE: {
            if(scene.desktop&&presenting_layer)return 0;
            scene.width=LOWORD(l)*96.0f/dpi; scene.height=HIWORD(l)*96.0f/dpi;
            if(render) render->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));
            layout_search(); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        case WM_GETMINMAXINFO: {
            if(!scene.desktop) {auto info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize.x=static_cast<LONG>(740*dpi/96); info->ptMinTrackSize.y=static_cast<LONG>(620*dpi/96);} return 0;
        }
        case WM_DPICHANGED: {
            dpi=static_cast<float>(HIWORD(w)); if(render) render->SetDpi(dpi,dpi);
            if(scene.desktop){release_layer();desktop_position();}
            else {const auto r=reinterpret_cast<RECT*>(l); SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);} return 0;
        }
        case WM_SETTINGCHANGE: update_preferences(); refresh_clock(); schedule(); return 0;
        case WM_TIMECHANGE: refresh_clock(); prayers.refresh(); return 0;
        case WM_POWERBROADCAST: if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND){refresh_clock();prayers.refresh();} return TRUE;
        case WM_MOUSEMOVE: {
            const float x=GET_X_LPARAM(l)*96.0f/dpi,raw_y=GET_Y_LPARAM(l)*96.0f/dpi,y=raw_y-(live_dock?visibility.offset():0);
            if(scene.slider_active){volume_from(panel_point(x,y).x);return 0;}
            if(scene.brightness_active){brightness_from(panel_point(x,y).x);return 0;}
            if(quick_move(panel_point(x,y)))return 0;
            if(drag_candidate>=0&&scene.dragging<0&&std::abs(x-drag_down_x)>6)begin_drag();
            if(scene.dragging>=0){scene.pointer_x=x;scene.pointer_y=y;scene.drag_x=x;update_drag();schedule();return 0;}
            if(const int slot=scene.hover_slot(x,y);slot!=scene.hovered)hover_changed(slot);
            const bool fresh=!tracking||scene.pointer_x<0;
            const auto before=scene.hover_signature(scene.pointer_x,scene.pointer_y);
            scene.pointer_x=x; scene.pointer_y=y;
            if(!tracking) { TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hwnd,0}; TrackMouseEvent(&t); tracking=true; }
            const bool over=inside(scene.dock(),x,y)||(live_dock&&visibility.wanted()&&inside(scene.dock(),x,raw_y))||(scene.panel!=Panel::None && scene.panel!=Panel::Calendar && inside(scene.panel_rect(),x,y));
            const auto holds=scene.machine.holds();const auto state=scene.machine.state();
            hover_hold(over);
            // Same hovered targets and dock state: the next frame would be identical.
            if(!fresh&&holds==scene.machine.holds()&&state==scene.machine.state()&&before==scene.hover_signature(x,y))return 0;
            schedule(); return 0;
        }
        case WM_MOUSELEAVE: tracking=false; scene.pointer_x=scene.pointer_y=-1;if(scene.dragging<0&&!scene.slider_active)hover_changed(-1);if(live_dock)pointer_hold();else hover_hold(false);schedule();return 0;
        case WM_LBUTTONDOWN: {
            if(jump.visible())jump.close();
            const float x=GET_X_LPARAM(l)*96.0f/dpi,y=GET_Y_LPARAM(l)*96.0f/dpi-(live_dock?visibility.offset():0);
            SetCapture(hwnd);
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&scene.volume_available&&inside(scene.panel_rect(),x,y)) {
                const auto point=panel_point(x,y);
                if(inside(scene.volume_hit(),point.x,point.y)){scene.slider_active=true;volume_from(point.x);return 0;}
            }
            if(scene.panel==Panel::Settings&&inside(scene.panel_rect(),x,y)&&over_brightness(panel_point(x,y))) {
                scene.brightness_active=true;brightness_from(panel_point(x,y).x);return 0;
            }
            if(scene.panel==Panel::Launcher&&inside(scene.panel_rect(),x,y)&&scene.power_pending<0) {
                const auto point=panel_point(x,y);
                for(size_t i=1;i<5;++i)if(inside(scene.power_button(i),point.x,point.y)){scene.power_holding=static_cast<int>(i);scene.power_hold_start=now();schedule();}
            }
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&inside(scene.panel_rect(),x,y)) {
                const auto point=panel_point(x,y);scene.tune_motion();
                quick_press=inside(scene.quick_area(),point.x,point.y)&&!scene.quick.tiles.empty();quick_swiping=false;quick_down=point;quick_down_slot=-1;
                for(size_t i=0;i<tiles_per_page;++i)if(const auto tile=scene.slot_tile(i);tile!=Tile::Count&&inside(scene.settings_tile(i),point.x,point.y)) {
                    if(scene.quick_edit&&inside(scene.tile_remove(i),point.x,point.y))continue;
                    quick_down_slot=static_cast<int>(i);
                    if(!scene.quick_edit){scene.tile_press[Scene::ti(tile)].target(.95f,now(),scene.reduced);schedule();}
                }
            }
            scene.press_at(x,y);
            // Pinned apps can be dragged to a new position.
            drag_candidate=-1;drag_moved=false;
            if(const int slot=scene.hover_slot(x,y);slot>0&&static_cast<size_t>(slot-1)<pinned_count()){drag_candidate=slot-1;drag_down_x=x;}
            schedule();return 0;
        }
        case WM_LBUTTONUP: {
            for(auto& spring:scene.tile_press)spring.target(1,now(),scene.reduced);
            if(scene.power_holding>=0){scene.power_holding=-1;schedule();} // Released early: nothing happens.
            if(scene.slider_active){scene.slider_active=false;volume_settle_until=now()+400;ReleaseCapture();schedule();return 0;}
            if(scene.brightness_active){scene.brightness_active=false;ReleaseCapture();schedule();return 0;}
            if(quick_release()){ReleaseCapture();return 0;}
            if(scene.dragging>=0){end_drag(true);ReleaseCapture();return 0;}
            drag_candidate=-1;
            scene.clear_press();ReleaseCapture();if(!drag_moved&&(!live_dock||visibility.wanted()))click(GET_X_LPARAM(l)*96.0f/dpi,GET_Y_LPARAM(l)*96.0f/dpi-(live_dock?visibility.offset():0));
            drag_moved=false;schedule();return 0;
        }
        case WM_CAPTURECHANGED:
            if(reinterpret_cast<HWND>(l)!=hwnd) {
                if(scene.dragging>=0)end_drag(false);
                quick_cancel();scene.slider_active=scene.brightness_active=false;drag_candidate=-1;
            }
            scene.clear_press();schedule();return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            const bool keyboard=point.x==-1&&point.y==-1;
            if(keyboard) {
                const auto count=static_cast<int>(scene.shown_groups());D2D1_RECT_F r=scene.launcher_rect();
                if(scene.panel==Panel::Tray&&scene.tray_focus>=0)r=scene.tray_tile(static_cast<size_t>(scene.tray_focus)-scene.tray_scroll);
                else if(scene.focus>0&&scene.focus<=count)r=scene.app_rect(static_cast<size_t>(scene.focus-1));
                else if(scene.focus==count+2)r=scene.quick_settings_rect();else if(scene.focus==count+3)r=scene.clock_rect();
                point={static_cast<LONG>(std::lround((r.left+r.right)*dpi/192)),static_cast<LONG>(std::lround((r.top+r.bottom)*dpi/192+(live_dock?visibility.offset()*dpi/96:0)))};ClientToScreen(hwnd,&point);
            }
            POINT client=point;ScreenToClient(hwnd,&client);
            const float x=client.x*96.0f/dpi,y=client.y*96.0f/dpi-(live_dock?visibility.offset():0);
            if(scene.panel==Panel::Launcher) {
                if(keyboard){show_actions(true);return 0;}
                if(inside(scene.panel_rect(),x,y)){launcher_context(panel_point(x,y));return 0;}
            }
            // Right-click (or the menu key) in Quick settings edits the tiles, as in Windows.
            if(scene.panel==Panel::Settings&&(keyboard||inside(scene.panel_rect(),x,y))) {
                if(scene.settings_page==Scene::SettingsPage::Main&&!scene.quick_edit)set_quick_edit(true);
                return 0;
            }
            const auto hit=scene.tray_hit(x,y);
            if(hit>=0)activate_tray(scene.tray_items[static_cast<size_t>(hit)].key,true,point,false,keyboard);
            else if(scene.desktop)control_menu(point,x,y,!keyboard&&(GetKeyState(VK_SHIFT)&0x8000),keyboard);return 0;
        }
        case WM_RBUTTONUP:return DefWindowProcW(hwnd,msg,w,l);
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(hwnd,&point);
            const float x=point.x*96.0f/dpi,y=point.y*96.0f/dpi-(live_dock?visibility.offset():0);
            const float notches=static_cast<float>(GET_WHEEL_DELTA_WPARAM(w))/WHEEL_DELTA;
            if(scene.panel==Panel::Launcher&&inside(scene.panel_rect(),x,y)&&!scene.launch_rows.empty()) {
                const auto area=scene.launch_list();const float extent=scene.launch_rows.back().bottom;
                scene.launch_scroll=std::clamp(scene.launch_scroll-notches*item_step(),0.0f,std::max(0.0f,extent-(area.bottom-area.top)));schedule();return 0;
            }
            if(scene.panel==Panel::Settings&&(scene.settings_page==Scene::SettingsPage::Wifi||scene.settings_page==Scene::SettingsPage::Bluetooth)&&inside(scene.panel_rect(),x,y)) {
                const auto area=scene.page_list();
                scene.list_scroll=std::clamp(scene.list_scroll-notches*52,0.0f,std::max(0.0f,scene.list_extent()-(area.bottom-area.top)));schedule();return 0;
            }
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::AddTiles&&inside(scene.panel_rect(),x,y)) {
                const auto area=scene.page_list();
                scene.list_scroll=std::clamp(scene.list_scroll-notches*42,0.0f,std::max(0.0f,scene.add_extent()-(area.bottom-area.top)));schedule();return 0;
            }
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&scene.quick.pages()>1&&inside(scene.panel_rect(),x,y)) {
                if(const auto pt=panel_point(x,y);inside(scene.quick_area(),pt.x,pt.y)){wheel_page(-GET_WHEEL_DELTA_WPARAM(w));return 0;}
            }
            if(scene.panel==Panel::Settings&&inside(scene.panel_rect(),x,y)&&over_brightness(panel_point(x,y))){show_brightness(scene.brightness_level+static_cast<int>(std::lround(5*notches)));return 0;}
            const bool over_sound=scene.machine.progress()>.85f&&inside(scene.quick_settings_rect(),x,y)&&x>=(scene.quick_settings_rect().left+scene.quick_settings_rect().right)/2;
            const bool over_slider=scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&inside(scene.panel_rect(),x,y)&&[&]{const auto pt=panel_point(x,y);const auto row=scene.volume_hit();return pt.y>=row.top-14&&pt.y<=row.bottom+14;}();
            if(over_sound||over_slider){adjust_volume(.02f*notches);return 0;}
        }
        if(scene.panel==Panel::Tray&&scene.native_tray&&scene.tray_items.size()>30){if(GET_WHEEL_DELTA_WPARAM(w)<0&&scene.tray_scroll+30<scene.tray_items.size())scene.tray_scroll+=5;else if(GET_WHEEL_DELTA_WPARAM(w)>0&&scene.tray_scroll>=5)scene.tray_scroll-=5;schedule();}return 0;
        case WM_MOUSEHWHEEL: { // Touchpad two-finger swipe: pages Quick settings.
            POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(hwnd,&point);
            const float x=point.x*96.0f/dpi,y=point.y*96.0f/dpi-(live_dock?visibility.offset():0);
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&scene.quick.pages()>1&&inside(scene.panel_rect(),x,y))wheel_page(GET_WHEEL_DELTA_WPARAM(w));
            return 0;
        }
        case WM_CHAR:
            if(const auto network=password_network()) {
                if(w==VK_BACK){if(!scene.password.empty()){scene.password.back()=0;scene.password.pop_back();}}
                else if(w==VK_RETURN)connect_network(*network);
                else if(w==0x16)paste_password(); // Ctrl+V
                else if(w>=32&&w!=127&&scene.password.size()<64)scene.password.push_back(static_cast<wchar_t>(w));
                schedule();return 0;
            }
            if(scene.desktop&&scene.panel==Panel::Launcher) {
                if(w==VK_BACK){if(!scene.query.empty())scene.query.pop_back();}
                else if(w==0x7F){while(!scene.query.empty()&&scene.query.back()==L' ')scene.query.pop_back();while(!scene.query.empty()&&scene.query.back()!=L' ')scene.query.pop_back();} // Ctrl+Backspace
                else if(w==0x16)paste_query(); // Ctrl+V
                else if(w>=32&&scene.query.size()<200)scene.query.push_back(static_cast<wchar_t>(w));
                else return 0;
                scene.power_holding=-1;research();schedule();
            }return 0;
        case WM_LBUTTONDBLCLK: {
            const float x=GET_X_LPARAM(l)*96.0f/dpi,y=GET_Y_LPARAM(l)*96.0f/dpi;
            if(const auto hit=scene.tray_hit(x,y);hit>=0){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(hwnd,&point);activate_tray(scene.tray_items[static_cast<size_t>(hit)].key,false,point,true);return 0;}
            for(size_t i=0;i<std::min<size_t>(8,scene.inventory.desktop.size());++i) if(inside(scene.desktop_item(i),x,y)) launch(scene.inventory.desktop[i].path);
            return 0;
        }
        case WM_SYSKEYDOWN:
            // Alt+Enter: properties of the selected file.
            if(w==VK_RETURN&&scene.desktop&&scene.panel==Panel::Launcher&&!scene.menu_open){run_verb(Scene::Verb::Properties);return 0;}
            return DefWindowProcW(hwnd,msg,w,l);
        case WM_KEYDOWN: {
            if(scene.panel==Panel::Tray&&scene.native_tray&&!scene.tray_items.empty()&&w!=VK_ESCAPE){
                const auto count=static_cast<int>(scene.tray_items.size());
                if(w==VK_TAB||w==VK_LEFT||w==VK_RIGHT||w==VK_UP||w==VK_DOWN){const int delta=w==VK_UP?-5:w==VK_DOWN?5:(w==VK_LEFT||(w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000)))?-1:1;scene.tray_focus=scene.tray_focus<0?0:((scene.tray_focus+delta)%count+count)%count;if(scene.tray_focus<static_cast<int>(scene.tray_scroll))scene.tray_scroll=static_cast<size_t>(scene.tray_focus/5)*5;else if(scene.tray_focus>=static_cast<int>(scene.tray_scroll+30))scene.tray_scroll=static_cast<size_t>(scene.tray_focus/5-5)*5;schedule();return 0;}
                if((w==VK_RETURN||w==VK_SPACE||(w==VK_F10&&(GetKeyState(VK_SHIFT)&0x8000)))&&scene.tray_focus>=0){const auto r=scene.tray_tile(static_cast<size_t>(scene.tray_focus)-scene.tray_scroll);POINT point{static_cast<LONG>(std::lround((r.left+r.right)*dpi/192)),static_cast<LONG>(std::lround((r.top+r.bottom)*dpi/192))};ClientToScreen(hwnd,&point);activate_tray(scene.tray_items[static_cast<size_t>(scene.tray_focus)].key,w==VK_F10,point,false,true);return 0;}
            }
            if(w==VK_ESCAPE&&scene.dragging>=0){end_drag(false);ReleaseCapture();return 0;}
            if(w==VK_ESCAPE&&scene.panel==Panel::Settings&&(quick_press||scene.quick_drag>=0)){quick_cancel();ReleaseCapture();schedule();return 0;}
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main) {
                const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
                if(w==VK_ESCAPE&&scene.quick_edit){set_quick_edit(false);return 0;}
                if((ctrl&&(w==VK_LEFT||w==VK_RIGHT))||w==VK_PRIOR||w==VK_NEXT){show_quick_page(static_cast<long>(scene.quick_page)+(w==VK_RIGHT||w==VK_NEXT?1:-1));return 0;}
            }
            if(w==VK_ESCAPE&&scene.panel==Panel::Settings&&scene.settings_page!=Scene::SettingsPage::Main){scene.start_page(Scene::SettingsPage::Main,now());scene.expanded.clear();clear_password();schedule();return 0;}
            if(password_network())return 0; // Keys become WM_CHAR for the security key.
            if(scene.panel==Panel::Launcher&&scene.desktop) {
                const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
                if(scene.menu_open) {
                    const int count=static_cast<int>(scene.menu_items.size());
                    if(w==VK_DOWN||w==VK_UP){scene.menu_sel=(scene.menu_sel+(w==VK_DOWN?1:-1)+count)%count;schedule();return 0;}
                    if(w==VK_RETURN&&count){run_verb(scene.menu_items[static_cast<size_t>(std::clamp(scene.menu_sel,0,count-1))].verb);return 0;}
                    if(w==VK_ESCAPE||w==VK_TAB){show_actions(false);return 0;}
                    return 0;
                }
                if(w==VK_TAB){show_actions(true);return 0;}
                if(w==VK_RETURN&&ctrl){run_verb(Scene::Verb::ShowInFolder);return 0;}
                if(w=='C'&&ctrl&&shift){run_verb(Scene::Verb::CopyPath);return 0;}
                if(w==VK_DOWN||w==VK_UP){move_selection(w==VK_DOWN?1:-1);return 0;}
                if(w==VK_ESCAPE&&scene.power_pending>=0){scene.power_pending=-1;launcher_toast(L"Cancelled");return 0;}
                if(w==VK_ESCAPE&&!scene.query.empty()){scene.query.clear();research();schedule();return 0;}
            }
            if(scene.panel==Panel::Settings&&scene.settings_page==Scene::SettingsPage::Main&&(w==VK_UP||w==VK_DOWN)){adjust_volume(w==VK_UP?.02f:-.02f);return 0;}
            if(w==VK_ESCAPE&&previews.visible()&&scene.panel==Panel::None){close_previews();return 0;}
            if(w==VK_ESCAPE) { if(scene.panel!=Panel::None) dismiss_panel(true); else { scene.focus=-1; scene.machine.hold(Hold::Keyboard,false,now());if(live_dock&&GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);schedule(); } }
            else if(w==VK_APPS||(w==VK_F10&&(GetKeyState(VK_SHIFT)&0x8000)))SendMessageW(hwnd,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(hwnd),MAKELPARAM(-1,-1));
            else if(w=='F'&&!scene.desktop) { set_panel(Panel::None); scene.machine.suppress(scene.machine.visible(),now()); schedule(); }
            else if(w=='R'&&!scene.desktop) { scene.reduced=!scene.reduced; scene.machine.reduced_motion(scene.reduced); calendar.preferences(scene.reduced);if(scene.reduced)scene.stop_feedback();scene.machine.hold(Hold::Flyout,flyout_open(),now()); schedule(); }
            else if(w==VK_RETURN&&scene.desktop&&scene.panel==Panel::Launcher)activate_launch((GetKeyState(VK_SHIFT)&0x8000)!=0);
            else if(w==VK_TAB||w==VK_RIGHT||w==VK_LEFT) {
                const int count=static_cast<int>(scene.shown_groups())+8;
                const int direction=(w==VK_LEFT||(w==VK_TAB&&(GetKeyState(VK_SHIFT)&0x8000)))?-1:1;
                scene.focus=(scene.focus+direction+count)%count; scene.machine.hold(Hold::Keyboard,true,now()); schedule();
            } else if((w==VK_RETURN||w==VK_SPACE)&&!(scene.desktop&&scene.panel==Panel::Launcher)) action(std::max(0,scene.focus));
            return 0;
        }
        case WM_HOTKEY: if(w==3){fullscreen();if(scene.machine.visible()){SetForegroundWindow(hwnd);SetFocus(hwnd);scene.focus=0;scene.machine.hold(Hold::Keyboard,true,now());schedule();}}return 0;
        case WM_KILLFOCUS: if(reinterpret_cast<HWND>(w)!=search&&reinterpret_cast<HWND>(w)!=calendar.window()) { if(scene.desktop&&!(scene.panel==Panel::Tray&&tray.holds_menu()))set_panel(Panel::None);scene.focus=-1; scene.machine.hold(Hold::Keyboard,false,now()); schedule(); } return 0;
        case WM_ACTIVATEAPP:if(!w&&scene.panel!=Panel::None&&!tray.holds_menu()&&!(scene.machine.holds()&static_cast<unsigned>(Hold::ContextMenu)))dismiss_panel();return 0;
        case WM_COMMAND:
            if(LOWORD(w)==101&&HIWORD(w)==EN_CHANGE) { wchar_t value[128]{}; GetWindowTextW(search,value,128); scene.query=value; research(); InvalidateRect(hwnd,nullptr,FALSE); } return 0;
        case WM_CTLCOLOREDIT:
            if(reinterpret_cast<HWND>(l)==search) { SetTextColor(reinterpret_cast<HDC>(w),RGB(242,244,250)); SetBkColor(reinterpret_cast<HDC>(w),RGB(20,27,37)); return reinterpret_cast<LRESULT>(edit_background); }
            return DefWindowProcW(hwnd,msg,w,l);
        case WM_TIMER:
            if(w==hide_delay_timer) { // The delay passed: hide unless the pointer came back.
                KillTimer(hwnd,hide_delay_timer);hide_delay_pending=false;
                hide_delay_done=true;
                if(live_dock)pointer_hold();else hover_hold(false);
                hide_delay_done=false;schedule();
            }
            else if(w==popup_input_timer){POINT point{};unsigned pressed=0;const auto state=popup_input(&pressed);if(GetCursorPos(&point))poll_popup_input(state,point,pressed);}
            else if(w==frame_timer) {
                const auto time=now();const bool exiting=scene.exiting_panel!=Panel::None;
                scene.machine.tick(time);visibility.tick(time);scene.tick_feedback(time);
                if(scene.panel==Panel::Launcher)if(const int due=scene.tick_launcher(time);due>=0)perform_power(static_cast<size_t>(due));
                scene.machine.hold(Hold::Flyout,flyout_open(),time);
                if(exiting&&scene.exiting_panel==Panel::None&&scene.desktop)desktop_position();
                schedule();
            }
            else if(w==media_title_timer){scene.media_marquee.tick(now());InvalidateRect(hwnd,nullptr,FALSE);}
            else if(w==desktop_timer){const auto result=desktop_toggle.tick();if(result!=E_PENDING){KillTimer(hwnd,desktop_timer);desktop_result(result);fullscreen();schedule();}}
            else if(w==tray_timer){
                // Runs only while an app's tray menu may be open; foreground
                // changes outside a menu are handled by foreground_changed.
                if(tray.tick()&&tray.holds_menu())return 0;
                KillTimer(hwnd,tray_timer);
                if(!tray.tick()||GetForegroundWindow()!=hwnd){set_panel(Panel::None);fullscreen();schedule();}
            }
            else if(w==settings_timer){if(scene.panel==Panel::Settings)connectivity.refresh(scene.settings_page!=Scene::SettingsPage::Main);else KillTimer(hwnd,settings_timer);}
            else if(w==countdown_timer)timer_tick();
            else if(w==alarm_timer){KillTimer(hwnd,alarm_timer);PlaySoundW(nullptr,nullptr,0);}
            else if(w==capture_timer)send_capture_keys();
            else if(w==render_retry_timer) {KillTimer(hwnd,render_retry_timer);InvalidateRect(hwnd,nullptr,FALSE);}
            else if(w==hover_timer) {
                KillTimer(hwnd,hover_timer);
                const int slot=scene.hovered;
                if(slot<0||scene.dragging>=0||scene.panel!=Panel::None||!scene.machine.visible())return 0;
                scene.tooltip_target=slot;arm_previews(slot);schedule();
            }
            else if(w==preview_delay_timer) {
                KillTimer(hwnd,preview_delay_timer);
                const int slot=preview_pending;preview_pending=-1;
                if(slot>0&&slot==scene.hovered&&scene.dragging<0&&scene.panel==Panel::None&&scene.machine.visible())open_previews(static_cast<size_t>(slot-1));
            }
            else if(w==previews_timer) {
                // Close after the pointer has left both the icon and the card for 300 ms.
                if(!previews.visible()){KillTimer(hwnd,previews_timer);return 0;}
                POINT point{};GetCursorPos(&point);
                bool over=previews.contains(point);
                if(!over&&previews_slot>=0&&static_cast<size_t>(previews_slot)<scene.shown_groups()){auto r=slot_screen(static_cast<size_t>(previews_slot));InflateRect(&r,0,static_cast<int>(16*dpi/96));over=PtInRect(&r,point)!=FALSE;}
                if(over)previews_leave=0;
                else if(!previews_leave)previews_leave=GetTickCount64();
                else if(GetTickCount64()-previews_leave>300){previews_leave=0;close_previews();}
            }
            else if(w==reconcile_timer) { KillTimer(hwnd,reconcile_timer);reconcile_scheduled=reconcile_slow=false; scan(false); }
            else if(w==clock_timer) {refresh_clock();}
            else if(w==pins_timer){KillTimer(hwnd,pins_timer);windows_pins_changed();}
            else if(w==caret_timer){scene.clock_now=now();InvalidateRect(hwnd,nullptr,FALSE);}
            else if(w==return_focus_timer) {
                KillTimer(hwnd,return_focus_timer);
                if(scene.panel==Panel::None&&GetForegroundWindow()==hwnd&&IsWindow(last_foreground))SetForegroundWindow(last_foreground);
                fullscreen();schedule();
            }
            return 0;
        case visibility_changed: {
            visibility_queued=false;const auto before=scene.machine.state();const bool maximized=auto_hidden,wanted=visibility.wanted();
            fullscreen();if(before!=scene.machine.state()||maximized!=auto_hidden||wanted!=visibility.wanted())schedule();return 0;
        }
        case windows_changed: {
            event_queued=false;
            fullscreen();schedule();
            // A pending slow (title) reconcile is shortened for real show/hide events.
            if(!reconcile_scheduled||reconcile_slow){reconcile_scheduled=SetTimer(hwnd,reconcile_timer,180,nullptr)!=0;reconcile_slow=false;}
            return 0;
        }
        case names_changed:
            names_queued=false;
            if(!reconcile_scheduled){reconcile_scheduled=SetTimer(hwnd,reconcile_timer,1000,nullptr)!=0;reconcile_slow=reconcile_scheduled;}
            return 0;
        case inventory_ready: {
            std::unique_ptr<Inventory> data; bool full;
            { std::lock_guard lock(mutex); data=std::move(pending); full=pending_full; }
            if(worker.joinable())worker.join();
            busy=false;
            bool changed=false;
            if(data&&scene.dragging>=0){end_drag(false);ReleaseCapture();}
            if(data) {scene.clock_now=now();changed=scene.accept(std::move(*data),full);inventory_loaded=true;
                scene.ensure_motion();
                if(previews.visible()) {
                    bool kept=false;
                    for(size_t i=0;i<scene.shown_groups();++i)if(previews.open_for(group_key(scene.groups[i]))){if(!scene.groups[i].windows.empty()){open_previews(i);kept=true;}break;}
                    if(!kept)close_previews();
                }
                known_windows.clear();for(const auto& window:scene.inventory.windows)known_windows.insert(window.hwnd);}
            if(rescan) { rescan=false; SetTimer(hwnd,reconcile_timer,180,nullptr); }
            if(changed){if(scene.desktop)desktop_position();InvalidateRect(hwnd,nullptr,FALSE);} return 0;
        }
        case WM_DESTROY:
            jump.close(false);jump_context.reset();
            KillTimer(hwnd,popup_input_timer);popup_watching=false;
            KillTimer(hwnd,desktop_timer);desktop_toggle.restore_on_exit();
            KillTimer(hwnd,tray_timer);tray.stop();
            KillTimer(hwnd,hover_timer);KillTimer(hwnd,previews_timer);previews.close(false);
            volume.stop();if(shell_hook)DeregisterShellHookWindow(hwnd);
            KillTimer(hwnd,settings_timer);KillTimer(hwnd,countdown_timer);KillTimer(hwnd,capture_timer);stop_alarm();alarm_card.close(false);
            connectivity.stop();updates.stop();brightness.stop();customize.close();app_updater.stop();reserve_bar(false);launcher_index.stop();file_search.stop();windows_key.stop();pin_watch.stop();KillTimer(hwnd,pins_timer);
            prayer_toast.close();prayers.stop();
            media.stop();
            calendar.close(false);
            event_target=nullptr; for(auto hook:hooks) if(hook) UnhookWinEvent(hook);known_windows.clear();
            if(edge)DestroyWindow(edge);release_edge_bitmap();UnregisterHotKey(hwnd,3);glass.detach();
            KillTimer(hwnd,frame_timer); KillTimer(hwnd,reconcile_timer);KillTimer(hwnd,clock_timer);KillTimer(hwnd,render_retry_timer);release_layer();
            worker.request_stop(); if(worker.joinable()) worker.join();
            if(search_font) DeleteObject(search_font); if(edit_background) DeleteObject(edit_background); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd,msg,w,l);
        }
    }
};
}
int run_preview(HINSTANCE instance, int show) {
    Preview preview;
    WNDCLASSW klass{}; klass.style=CS_DBLCLKS; klass.lpfnWndProc=Preview::proc; klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW); klass.lpszClassName=L"MinimaDock.Preview";
    if(!RegisterClassW(&klass)) return 1;
    const auto hwnd=CreateWindowExW(0,klass.lpszClassName,L"MinimaDock — Native design preview",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1180,790,nullptr,nullptr,instance,&preview);
    if(!hwnd) return 1;
    BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
    ShowWindow(hwnd,show); MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0) {
        if(msg.hwnd==preview.search&&msg.message==WM_KEYDOWN) {
            if(msg.wParam==VK_ESCAPE) { preview.set_panel(Panel::None); continue; }
            if(msg.wParam==VK_RETURN) { preview.activate_launch(false); continue; }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
int run_desktop_dock(HINSTANCE instance) {
    Preview dock;dock.scene.desktop=true;dock.live_dock=true;dock.glass_mode=GlassHost::available();
    WNDCLASSW klass{};klass.style=CS_DBLCLKS;klass.lpfnWndProc=Preview::proc;klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.lpszClassName=L"MinimaDock.DesktopDock";
    if(!RegisterClassW(&klass))return 1;
    const DWORD surface=dock.glass_mode?WS_EX_NOREDIRECTIONBITMAP:WS_EX_LAYERED;
    const auto hwnd=CreateWindowExW(surface|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,klass.lpszClassName,L"MinimaDock",WS_POPUP,0,0,1000,440,nullptr,nullptr,instance,&dock);
    if(!hwnd)return 1;
    dock.desktop_position();ShowWindow(hwnd,SW_SHOWNOACTIVATE);InvalidateRect(hwnd,nullptr,FALSE);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return static_cast<int>(message.wParam);
}
int benchmark_dock_render(HINSTANCE instance,const std::wstring& path) {
    try {
        Preview dock;dock.scene.desktop=true;
        WNDCLASSW klass{};klass.lpfnWndProc=Preview::proc;klass.hInstance=instance;klass.lpszClassName=L"MinimaDock.RenderBenchmark";
        if(!RegisterClassW(&klass))return 1;
        const auto window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,klass.lpszClassName,L"MinimaDock renderer measurement",WS_POPUP,0,0,1000,440,nullptr,nullptr,instance,&dock);
        if(!window)return 1;
        dock.desktop_position();
        const auto deadline=GetTickCount64()+15000;
        while(!dock.inventory_loaded&&GetTickCount64()<deadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);
        }
        if(!dock.inventory_loaded){DestroyWindow(window);return 1;}
        dock.scene.machine.reduced_motion(true);dock.scene.machine.hold(Hold::Keyboard,true,0);
        std::vector<double> times;
        for(unsigned i=0;i<104;++i) {
            dock.scene.pointer_x=dock.scene.layout().launcher_x();dock.scene.pointer_y=dock.scene.height-34;
            const auto start=std::chrono::steady_clock::now();dock.paint_layer();
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(i>=8)times.push_back(ms);
        }
        std::sort(times.begin(),times.end());
        std::ofstream output{std::filesystem::path(path)};
        output<<"{\n  \"Kind\": \"cached native frame microbenchmark; hidden test window, not end-to-end pointer latency\",\n  \"Samples\": "<<times.size()<<",\n  \"LayerWidthPx\": "<<dock.layer_width<<",\n  \"LayerHeightPx\": "<<dock.presented_height<<",\n  \"MedianMs\": "<<times[times.size()/2]<<",\n  \"P95Ms\": "<<times[static_cast<size_t>(times.size()*.95)]<<",\n  \"MaxMs\": "<<times.back()<<",\n  \"Groups\": [";
        for(size_t i=0;i<dock.scene.groups.size();++i) {
            const auto& group=dock.scene.groups[i];
            if(i)output<<',';
            output<<"\n    {\"Name\": "<<json_text(group.name)<<", \"AppId\": "<<json_text(group.app_id)<<", \"Pinned\": "<<(group.pinned?"true":"false")<<", \"Running\": "<<(!group.windows.empty()?"true":"false")<<", \"HasIcon\": "<<(group.icon?"true":"false");
            if(i<dock.scene.shown_groups())output<<", \"Slot\": "<<i;
            output<<'}';
        }
        output<<"\n  ],\n  \"WindowIconSources\": [";
        for(size_t i=0;i<dock.scene.inventory.windows.size();++i) {
            const auto& app=dock.scene.inventory.windows[i];if(i)output<<',';
            output<<"\n    {\"Title\": "<<json_text(app.title)<<", \"AppId\": "<<json_text(app.app_id)<<", \"Source\": "<<json_text(app.icon_source)<<'}';
        }
        output<<"\n  ]\n}\n";
        // Export this app's rendered pixels for visual QA, never capture the desktop.
        ComPtr<IWICBitmap> image;
        check(dock.scene.wic->CreateBitmapFromMemory(dock.layer_width,dock.presented_height,GUID_WICPixelFormat32bppPBGRA,dock.layer_width*4,dock.layer_width*dock.presented_height*4,static_cast<BYTE*>(dock.layer_pixels),image.put()));
        auto png=std::filesystem::path(path);png.replace_extension(L".png");
        ComPtr<IWICStream> stream;check(dock.scene.wic->CreateStream(stream.put()));check(stream->InitializeFromFilename(png.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;check(dock.scene.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));check(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(frame.put(),nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(dock.layer_width,dock.presented_height));
        auto format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format));check(frame->WriteSource(image.get(),nullptr));check(frame->Commit());check(encoder->Commit());
        DestroyWindow(window);return output?0:1;
    }catch(...){return 1;}
}
static bool snapshot_desktop=false;
void snapshot_over_desktop(bool on){snapshot_desktop=on;}
int save_preview_snapshot(const std::wstring& path, bool expanded,int panel) {
    try {
        Scene scene; scene.initialize();scene.desktop=(panel>=3&&panel<=13)||panel==25||snapshot_desktop;
        if(panel==25){scene.bar=true;scene.machine.hold(Hold::Bar,true,0);} // Full-width bar style.
        const bool small=scene.desktop&&!snapshot_desktop;
        const UINT image_width=small?1000:1160,image_height=(panel>=4&&panel<=6)||(panel>=8&&panel<=13)?88:(small?440:740);
        scene.width=static_cast<float>(image_width);scene.height=static_cast<float>(image_height);
        ComPtr<IWICImagingFactory> wic;
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(wic.put())));
        ComPtr<IWICBitmap> bitmap; check(wic->CreateBitmap(image_width,image_height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
        ComPtr<ID2D1RenderTarget> rt; check(scene.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),rt.put()));
        scene.bind(rt.get()); scene.accept(collect_inventory(GetCurrentProcessId()),true);
        if(expanded) { scene.machine.reduced_motion(true); scene.machine.hold(Hold::Keyboard,true,0); }
        if(panel==1||(panel>=26&&panel<=28)){scene.brightness_available=true;scene.brightness_level=70;} // Fixed sample, not this PC's panel.
        if(panel==1)scene.panel=Panel::Settings;else if(panel==2)scene.panel=Panel::Tray;
        else if(panel>=26&&panel<=28) { // Quick settings: edit mode, second page, Add tiles.
            scene.panel=Panel::Settings;
            scene.dark=true;scene.display={2,DisplayMode::Extend};scene.keyboard_count=2;scene.keyboard_label=L"ENG";
            if(panel==26)scene.quick_edit=true;
            if(panel==27){scene.quick.tiles.clear();for(size_t i=0;i<Scene::tile_count;++i)scene.quick.add(static_cast<Tile>(i));scene.quick_page=std::min<size_t>(1,scene.quick.pages()-1);}
            if(panel==28){scene.quick_edit=true;scene.quick.remove(1);scene.quick.remove(2);scene.settings_page=Scene::SettingsPage::AddTiles;}
        }
        else if(panel==4){const auto button=scene.desktop_button_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==5){const auto button=scene.launcher_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==6){scene.launcher_pulse.start(0);scene.launcher_pulse.tick(60);const auto button=scene.launcher_rect();scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        else if(panel==7){scene.panel=Panel::Settings;scene.panel_reveal.start(0);scene.panel_reveal.tick(60);}
        else if(panel==17)scene.prayer_line=L"Maghrib in 15 min"; // Longest clock line.
        else if(panel==18&&scene.shown_groups()>2){ // Hovered app, springs settled, name shown.
            const auto button=scene.app_rect(2);scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;
            scene.set_hovered(3,0);scene.tooltip_target=3;scene.sync_tooltip(0);scene.stop_feedback();
        }
        else if(panel>=14&&panel<=16) {
            // Quick settings pages with sample data (no live services in snapshots).
            using Page=Scene::SettingsPage;scene.panel=Panel::Settings;
            scene.settings_page=panel==14?Page::Wifi:panel==15?Page::Bluetooth:Page::Timer;
            auto& n=scene.net;n.wifi_present=n.wifi_on=n.wifi_connected=n.bluetooth_present=n.bluetooth_on=true;n.wifi_name=L"Home Network";
            const auto network=[&](const wchar_t* ssid,int signal,bool secured,bool connected){WifiNetwork w;w.ssid=ssid;w.signal=signal;w.secured=secured;w.connected=w.saved=connected;w.auth=secured?7:1;n.networks.push_back(w);};
            network(L"Home Network",92,true,true);network(L"Neighbor 5G",64,true,false);network(L"Cafe Guest",48,false,false);network(L"Office",22,true,false);
            const auto device=[&](const wchar_t* name,BluetoothDevice::Kind kind,bool connected,bool audio){BluetoothDevice d;d.name=name;d.key=name;d.kind=kind;d.connected=connected;d.switchable=audio;n.devices.push_back(d);};
            device(L"Headphones",BluetoothDevice::Kind::Audio,true,true);device(L"Mouse",BluetoothDevice::Kind::Mouse,true,false);device(L"Phone",BluetoothDevice::Kind::Phone,false,false);
            scene.expanded=panel==14?L"Neighbor 5G":L"Headphones";scene.password=L"secret12";
            scene.timer_state=Scene::TimerState::Running;scene.timer_duration=25*60000;scene.timer_end=GetTickCount64()+754000;
        }
        else if(panel>=19&&panel<=24) {
            // Launcher with this PC's real apps and files: 19 search "read", 20 its actions menu, 21 home.
            LauncherIndex index;index.start(nullptr);
            for(int i=0;i<80&&index.apps()->empty();++i)Sleep(100);
            scene.panel=Panel::Launcher;scene.launch_apps=index.apps();scene.launch_usage=load_usage();
            scene.query=panel==21?L"":panel==22?L":heart":panel==23?L"> Get-ChildItem ~/Documents | Select -First 5":L"read";
            if(panel==24&&!scene.inventory.windows.empty())scene.query=Scene::app_label(scene.inventory.windows.front().executable);
            auto files=std::make_shared<FileResults>();files->query=scene.query;files->hits=find_files(scene.query);scene.launch_files=files;
            scene.search_launcher();
            const auto wanted=panel==24?Scene::LaunchResult::Kind::Window:Scene::LaunchResult::Kind::File;
            if(panel<=20||panel==24)for(size_t i=0;i<scene.launch_found.size();++i)if(scene.launch_found[i].kind==wanted){scene.launch_sel=static_cast<int>(i);break;}
            scene.keep_visible();
            if(const auto* item=scene.selected())if(const auto* file=scene.file_of(*item))scene.file_preview=std::make_shared<FilePreview>(make_file_preview(file->path));
            if(panel==20){scene.build_menu();scene.menu_open=true;scene.menu_sel=2;}
            if(panel==24)if(const auto* row=scene.row_of(static_cast<size_t>(scene.launch_sel))) {
                const auto area=scene.launch_list();scene.build_menu();scene.menu_open=scene.menu_anchored=true;scene.menu_sel=1;
                scene.menu_point=D2D1::Point2F(area.left+180,area.top+(row->top+row->bottom)/2-scene.launch_scroll);
            }
            scene.tick_launcher(0);index.stop();
        }
        else if(panel>=8&&panel<=10){const auto system=scene.system_rect();constexpr float offsets[]={11,38,63};scene.pointer_x=system.left+offsets[panel-8];scene.pointer_y=scene.layout().center_y;}
        else if(panel==11){const auto clock=scene.clock_rect();scene.pointer_x=(clock.left+clock.right)/2;scene.pointer_y=scene.layout().center_y;}
        else if(panel==12||panel==13){scene.media.available=scene.media.previous=scene.media.toggle=scene.media.next=true;scene.media.playing=panel==12;scene.media.title=L"Midnight Drive";scene.media.artist=L"Local player";const auto button=scene.media_button(1);scene.pointer_x=(button.left+button.right)/2;scene.pointer_y=(button.top+button.bottom)/2;}
        scene.draw(); check(rt->EndDraw());
        ComPtr<IWICStream> stream; check(wic->CreateStream(stream.put())); check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder; check(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put())); check(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(frame.put(),nullptr)); check(frame->Initialize(nullptr)); check(frame->SetSize(image_width,image_height));
        auto format=GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format)); check(frame->WriteSource(bitmap.get(),nullptr)); check(frame->Commit()); check(encoder->Commit()); return 0;
    } catch(...) { return 1; }
}
}
