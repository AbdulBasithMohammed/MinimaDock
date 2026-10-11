# Compatibility status

October 10 development update: user-approved temporary taskbar testing uses an independent supervisor, durable taskbar visibility/work-area journal and per-pixel-alpha native dock. Explorer remains running; default shell, startup and wallpaper settings are untouched. Work areas release the taskbar's bottom reservation only during the trial and are restored on revert. Full-height maximize and live recovery checks pass on this single display; multiple displays/hotplug and app-specific compatibility remain pending. This exception does not establish Explorer-free compatibility. The dock is primary-monitor-only; secondary Explorer taskbars are captured and reverted. See [temporary test details](taskbar-test.md).

Recorded on October 9, 2026 (America/New_York), Windows 11 Pro 25H2, build 26200.9457, x64. Tested support currently means the framed preview and independent recovery tests on this build. A proposed wider Windows 11 build range is not a support claim.

| Component | Evidence | Status / next validation |
|---|---|---|
| Native GUI | C++20 Win32, Direct2D/DirectWrite build and WIC snapshots | Framed preview only; real UI automation and multi-DPI trials pending |
| Recovery baseline | Current user captured; valid bundle; zero mutations | Isolated registry tests pass; Explorer-free restoration and next-login still pending |
| Rollback guard | Crash/hang/timeout/revert/keep/identity fixture tests | Integrated activation coordinator and startup-failure path pending |
| Pro Custom User Interface | Microsoft documents user scope and Pro applicability | Registry/RSOP/MDM precedence and actual sign-in not tested |
| Explorer absent | No replacement performed | Mandatory isolated trial; no hidden Explorer workaround |
| File browsing / This PC | Original shortcuts and ShellExecute semantics retained in preview | Ensure opening folders, This PC and Recycle Bin never revives taskbar after replacement |
| Wallpaper Engine | Running `C:\Games\Steam\steamapps\common\wallpaper_engine\wallpaper64.exe` observed | Explorer-free stacking, input, startup and GPU cost unproven; user settings untouched |
| Spotify | No Spotify process observed during initial inventory | Store/desktop installation and media-session capability not established; adapter pending |
| Windows settings | Exact `ms-settings:network-wifi`, `bluetooth`, `sound` links | Links provided; real device toggles and audio callbacks pending |
| Tray / toasts | No replacement tray implementation | Hosting/licensing spike mandatory; tray and notifications are separate gates |
| Fullscreen | State-machine suppression cancels hover/animation | `F` is a simulation only; maximized versus borderless game classifier pending |
| DPI / monitors | One 2560×1440 display; stock work area bottom 1392 | Preview resize/Per Monitor V2 implemented; scale/refresh/hotplug/sleep tests pending |
| Launcher | Actual Start Menu shortcuts scanned; search opens original paths | Packaged apps and deduplication require explicit identity implementation |
| Resources | Raw stock capture pilot available | No A/B, GPU, latency or leak acceptance evidence yet |

Primary references checked during implementation:

- [Microsoft CustomShell policy documentation](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-admx-winlogon#customshell) establishes direction for user-scoped activation on Pro, not proof of this machine's policy precedence.
- [Wallpaper Engine standalone window commands](https://help.wallpaperengine.io/en/functionality/cli.html) expose an experimental playback path; they do not guarantee an embedded Explorer-free desktop.
- [SendMessageTimeout](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw) provides the guard's bounded UI-thread liveness probe.

Managed policy assessment is deliberately unresolved. Capturing registry values cannot establish their source/precedence. No machine policy, security setting, Windows component or unrelated startup entry was changed.
