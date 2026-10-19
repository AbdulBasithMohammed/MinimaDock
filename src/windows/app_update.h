#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace minima {
inline constexpr UINT app_update_message=WM_APP+32;
// MinimaDock's own version, shown in settings and compared with GitHub releases.
inline constexpr wchar_t app_version[]=L"1.0.3";

struct AppUpdateStatus {
    bool checking=false,checked=false,available=false;
    std::wstring latest;      // Newest released version, e.g. "1.0.2".
    std::wstring checked_at;  // Local time of the last successful check.
    std::wstring error;       // Why the last check failed.
};

// Checks GitHub for a newer MinimaDock release (on request and, when automatic,
// shortly after start and then daily) on a worker thread. Contacts only
// api.github.com and the release's download host. install() downloads the
// release zip in a visible PowerShell window, verifies its SHA-256 and runs the
// zip's own installer, which replaces this version only if Windows allows it.
class AppUpdater {
public:
    AppUpdater();~AppUpdater();
    AppUpdater(const AppUpdater&)=delete;AppUpdater& operator=(const AppUpdater&)=delete;
    void start(HWND notify,bool automatic);
    void stop();
    void set_automatic(bool automatic);
    void check();
    [[nodiscard]] AppUpdateStatus status() const;
    bool install(HWND owner) const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool load_auto_update_setting();
void save_auto_update_setting(bool on);
// Pure helpers (tested): numeric dotted-version comparison and release parsing.
int compare_versions(const std::wstring& a,const std::wstring& b);
}
