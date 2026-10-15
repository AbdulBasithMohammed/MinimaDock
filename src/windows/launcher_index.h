#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minima {
// Posted to the notify window when the app list has been (re)loaded.
inline constexpr UINT launcher_index_message=WM_APP+27;
struct LauncherApp {
    std::wstring name;
    std::wstring id;                // Parsing name in shell:AppsFolder (AUMID or path).
    std::shared_ptr<void> image;    // 64 px HBITMAP (premultiplied), or null.
};
// Windows' own "All apps" list (shell:AppsFolder: Start Menu shortcuts and
// Store apps alike), read on a background thread with icons.
class LauncherIndex {
public:
    LauncherIndex();
    ~LauncherIndex();
    LauncherIndex(const LauncherIndex&)=delete;
    LauncherIndex& operator=(const LauncherIndex&)=delete;
    void start(HWND notify);
    void stop();
    void refresh();  // Reload if the list is older than ten minutes.
    std::shared_ptr<const std::vector<LauncherApp>> apps() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool launch_app_id(HWND owner,const std::wstring& id,bool elevated=false);

// How often and how recently each result was opened (HKCU\Software\MinimaDock\
// Launcher\Usage), for ranking. Keys: "app:<id>", "setting:<name>", "action:<name>".
struct Usage {std::uint32_t count=0;double days=0;};
std::map<std::wstring,Usage> load_usage();
void record_use(const std::wstring& key);

struct SettingsPage {const wchar_t* name;const wchar_t* keywords;const wchar_t* uri;};
const std::vector<SettingsPage>& settings_pages();

// Power. Restart, shut down and sign out let apps save first (no force).
bool power_lock();
bool power_sleep();
bool power_restart();
bool power_shutdown();
bool power_sign_out();
// Windows' app and system light/dark theme.
bool set_dark_mode(bool dark);
bool copy_text(HWND owner,const std::wstring& text);
// "> command": runs in a new PowerShell window (Windows Terminal when installed).
bool run_command(const std::wstring& command,bool elevated);
bool open_url(const std::wstring& url);
// Window arrangement from the launcher.
enum class Arrange {Left,Right,Maximize,Minimize,Close};
bool arrange_window(HWND window,Arrange how);
}
