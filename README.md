# MinimaDock

A native C++20 Windows 11 shell project: a small centered glass pill that expands on hover, with a minimal desktop and familiar app access. [The project brief](WINDOWS_SHELL_PROJECT_BRIEF.md) is the product specification.

**Current build: native design preview and guarded temporary desktop taskbar test.** The user approved an Explorer-backed temporary test on October 10, 2026. Explorer-free shell replacement and automatic startup remain unavailable. See [deploy/keep/revert instructions](docs/taskbar-test.md).

Run `out\Deploy-Taskbar.cmd` for a ten-minute trial. **Ctrl+Alt+Backspace** or `out\Revert-Taskbar.cmd` restores the Windows taskbar. **Ctrl+Alt+K** or `out\Keep-Taskbar.cmd` keeps this session's test, with crash/hang supervision active.

Maximized windows and full-height snapped panes extend to the bottom behind the floating pill. Snapped panes retain their chosen widths and original restore placement. The trial journals the original taskbar work area and restores it with Revert, timeout or crash recovery.

With a maximized or snapped app, the pill slides/fades out so it cannot cover app controls. A subtle rounded line at the bottom marks the hidden pill's full width; hover anywhere along it or press **Ctrl+Alt+Space** to reveal. The line fades away as the pill appears, and its activation area follows the pill's width as it expands. Open menus keep it visible; Escape closes keyboard interaction and returns focus. The desktop and ordinary windows keep the collapsed dock visible.

The expanded pill's Show desktop sliver delegates desktop/restore to Windows during the Explorer-backed trial, including fullscreen games. It is hidden and inactive in the compact pill, which has equal padding around the first and last icons. The Explorer-free fallback uses an in-memory list of minimized windows. The Windows command adapter remains a compatibility dependency that needs validation on other Windows builds.

App icons, media buttons, system tray, Wi-Fi/sound, clock and Show Desktop share a soft 240 ms press and bubble pop while actions proceed immediately. Flyouts lift, scale and fade in over 180 ms and close over 150 ms; clicking again reverses the transition. Outside clicks, Escape and switching apps also dismiss flyouts, including nonactivating interactions. Effects respect reduced motion and stop repainting when finished. Other applications keep Windows' native opening and restore behavior.

Right-click an app for a Windows-style Jump List with that app's own tasks and recent/pinned items, plus launch, pin/unpin and close; it follows the system light or dark theme. Shift+right-click opens the window's native commands. Pin overrides are saved under `HKCU\Software\MinimaDock\Pins`; they affect MinimaDock without changing Explorer's pins. Right-click the launcher for Settings, Task Manager, File Explorer and Terminal; Wi-Fi/sound for network/audio settings; the clock for date/time and notifications; empty pill space for Task Manager, Windows taskbar settings and trial recovery. Shift+F10 or the Menu key also opens the focused item's menu. Tray icons continue to call their original app-specific handlers.

The tray arrow uses the app icons' rounded hover highlight and two DIP lift. Wi-Fi and sound share one quick settings button: hovering either icon highlights the pair and lifts both together. Reduced motion keeps the highlight without lifting.

Quick settings has Wi-Fi and Bluetooth toggles with › pages for networks and devices (as in Windows), microphone mute, NVIDIA Instant Capture, a timer with an alarm, Windows Update status and the volume slider. The clock's second line shows the next prayer from the configured masjid website; the calendar adds that day's Adhan and Iqama times, and a chime and reminder mark each Adhan.

## Launcher

Click the MinimaDock button, or tap the Windows key in the live dock, to open the launcher centered above the dock. It searches locally only: your apps (Windows' All apps list), open windows, files (Recent items and the Windows Search index), dock actions, Settings pages and quick answers such as `12*7`, `15% of 80`, `5 km in mi` or `days until dec 25`. Typing never sends anything online and no web results or ads appear. `>` runs a PowerShell command, `:` finds emoji, and `yt`, `g`, `gh`, `wiki` or `maps` followed by words (or a typed web address) open the browser when you press Enter.

Up/Down move, Enter opens, Shift+Enter runs as administrator, Tab or right-click shows actions (show in folder, copy path, properties, snap or close a window, and more), Esc clears then closes. The empty launcher shows quick actions, recent apps and files, and a glance at Wi-Fi, Bluetooth, volume, battery, updates and the next prayer. Lock acts at once; Sleep, Restart, Shut down and Sign out need a short hold and give a five-second countdown you can cancel. Windows-key shortcuts such as Win+E keep working, full-screen games keep Windows' Start, and the launcher can switch the key back to Start ("Windows key opens Start"); Ctrl+Esc always opens Start.

The tray arrow opens MinimaDock's own animated dark glass panel above the button. It receives actual notification icons and calls each app's original activation/context-menu handler, including custom tray utilities. Its title and hover labels share Bahnschrift Light with the clock and calendar. Tab/arrows select icons; Enter/Space activates, Shift+F10 requests the original menu, Escape closes, and large icon sets scroll. During temporary trials, notifications are mirrored to Explorer to preserve Revert and crash recovery. Applications re-register through TaskbarCreated; an app that ignores that message may need to be restarted. Explorer-free startup and independent notifications remain separate work.

The left wing controls the current [Windows media session](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager): previous, play/pause and next, with the current title in Bahnschrift Light. It works with participating players and browsers without a Spotify account or API key. Unsupported buttons are disabled and explain their unavailable state on hover; no active session shows "No media." Previous/play/next have separate, wider click areas beneath the title. Overflowing titles scroll gently while the pill is fully expanded, with reading pauses at both ends; reduced motion keeps a static trimmed title. Hover the title to see the artist. Media updates run on a background worker; the title timer runs only for visible overflowing text.

Click the time/date to open a calendar centered above that button, with the same charcoal glass as the dock's other flyouts. Dock and calendar text use the clock's Bahnschrift Light font, with a shared fallback when unavailable. Up/down month arrows and the scroll wheel navigate with a short vertical slide animation; Today returns to the current local date. Page Up/Down changes months, arrow keys select dates, Home returns to today, and Escape or clicking outside closes it. The calendar holds the dock visible through its closing animation and respects reduced motion.

## Run the preview

Open `out\MinimaDock.exe` for the framed preview. The compact dock shows all pinned and open/minimized apps, deduplicated after the MinimaDock launcher at the start of the centered app group, with pinned apps in their saved order followed by unpinned running apps. Hover expands both sides to show media on the left and system/time/calendar controls on the right; every app stays fully visible and at the same coordinates. Click the launcher to search apps, files and settings (see Launcher above). The original MinimaDock mark (three rounded bars forming an M, standing on a dock bar) is drawn as a cached 24 DIP native vector inside the existing 44 DIP button. Click apps to launch/activate; grouped windows use a selector. The right wing opens quick settings; the deployed desktop dock also includes the custom tray.

Preview desktop entries come from actual Desktop/public Desktop folders; double-click opens the original item. It displays up to eight desktop items; app capacity follows monitor width with overflow only at the physical limit. Native icons are extracted when available. Traditional pins merge with matching running executables. Complete packaged identity, desktop keyboard navigation and UI Automation remain unfinished.

Keyboard: Tab/left/right focus dock controls, including media buttons; Enter/Space activate, Escape closes the flyout first. `F` simulates fullscreen suppression inside the preview; `R` toggles reduced motion. The framed preview does not hook the Windows key; only the live dock does. Explorer-free third-party tray hosting is pending; disabled/pending features never issue fake OS operations.

## Recovery first

The existing baseline is at `recovery-bundle\baseline.json`. Keep the entire bundle, including the manifest, journal, scripts and launcher. It must stay on local storage and survive builds/cleanup. A fresh backup can be prepared in a new, stable directory:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Prepare-Recovery.ps1 -Destination 'C:\YourLocalFolder\MinimaDock-Recovery'
```

This captures state and creates files; it does not activate the shell. Existing backups are never overwritten. See [recovery instructions](docs/recovery.md) for the Task Manager route, verification and current limits.

## Build and validation

Preferred toolchain: Visual Studio 2022 C++ build tools, Windows SDK, CMake 3.24+, x64 or ARM64 selected for the target machine. [Development instructions](docs/development.md) also document the verified portable LLVM toolchain used for this first x64 build.

```powershell
cmake --preset msvc-x64
cmake --build --preset release-x64
ctest --preset test-x64
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\recovery\Recovery.Tests.ps1
```

Recovery tests modify only unique disposable `HKCU\Software\MinimaDock.RecoveryTests` keys. The native guard test uses a hidden fixture and a harmless callback, never a live shell switch. See [compatibility](docs/compatibility.md), [performance](docs/performance.md) and [current progress](docs/progress.md) before any replacement experiment.

MinimaDock's source, documentation and original launcher mark are licensed under [MIT](LICENSE), copyright 2026 AbdulBasithMohammed. Third-party components retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). Regenerate the logo SVG and native header with `python tools/Generate-LauncherLogo.py`; no reference image or tracing is used.

Before publishing this checkout, review [public repository preparation](docs/publishing.md). Personal recovery bundles, captures and deployments must stay local.
