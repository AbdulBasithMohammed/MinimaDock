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
| Windows media sessions | Native silent SMTC fixture and current-session handoff checked | Shared controller reads title/artist/status and sends supported transport requests; participating players/browsers required |
| Spotify | No Spotify process observed during initial inventory | Uses the general Windows media controller; specific Spotify installation remains untested |
| Quick settings | Wi-Fi/Bluetooth radios (Windows.Devices.Radios), WLAN networks, Bluetooth classic/LE devices and audio connect, microphone mute, Windows Update Agent status | Checked on this PC's adapters; Wi-Fi network names need Windows location permission; other adapters/drivers untested |
| Custom tray on Explorer-backed trial | Version 0.2.31 receives native app icons and draws its own glass panel; original callback/menu protocols, coordinates and crash mirror fixture pass; repeated toggles show no Explorer overflow | App re-registration required; installed-app menu commands, elevated apps, Explorer restart, mixed DPI and multiple monitors still need validation |
| Explorer-free tray / toasts | Own notification receiver implemented; trial startup still requires Explorer for mirroring and rollback; no independent notification implementation | Explorer-free startup/recovery and notifications remain separate gates |
| Fullscreen | State-machine suppression cancels hover/animation | `F` is a simulation only; maximized versus borderless game classifier pending |
| DPI / monitors | One 2560×1440 display; stock work area bottom 1392 | Preview resize/Per Monitor V2 implemented; scale/refresh/hotplug/sleep tests pending |
| Launcher | `shell:AppsFolder` (desktop and Store apps), Recent items, Windows Search index over OLE DB, open windows | File search depends on which folders Windows indexes; window snapping uses the visible DWM frame; tested on one display |
| Windows key | Low-level keyboard hook in the live dock; mask key keeps Start closed; shortcuts pass through; full-screen apps keep Start | Needs live verification across games, elevated foreground apps (Windows does not deliver their input to a standard-user hook, so Start opens) and Explorer-free sessions |
| Prayer times | WinHTTP read of the configured masjid site (GalaxyStream widget markup) with a local cache | A site redesign can break parsing; failures keep the cached day and show an error |
| Resources | Raw stock capture pilot available | No A/B, GPU, latency or leak acceptance evidence yet |

Primary references checked during implementation:

- [Microsoft CustomShell policy documentation](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-admx-winlogon#customshell) establishes direction for user-scoped activation on Pro, not proof of this machine's policy precedence.
- [Wallpaper Engine standalone window commands](https://help.wallpaperengine.io/en/functionality/cli.html) expose an experimental playback path; they do not guarantee an embedded Explorer-free desktop.
- [SendMessageTimeout](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw) provides the guard's bounded UI-thread liveness probe.

Managed policy assessment is deliberately unresolved. Capturing registry values cannot establish their source/precedence. No machine policy, security setting, Windows component or unrelated startup entry was changed.
