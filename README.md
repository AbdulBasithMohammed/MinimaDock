# MinimaDock

A native C++20 Windows 11 shell project: a small centered glass pill that expands on hover, with a minimal desktop and familiar app access. [The project brief](WINDOWS_SHELL_PROJECT_BRIEF.md) is the product specification.

**Current build: native design preview and guarded temporary desktop taskbar test.** The user approved an Explorer-backed temporary test on October 10, 2026. Explorer-free shell replacement and automatic startup remain unavailable. See [deploy/keep/revert instructions](docs/taskbar-test.md).

Run `out\Deploy-Taskbar.cmd` for a ten-minute trial. **Ctrl+Alt+Backspace** or `out\Revert-Taskbar.cmd` restores the Windows taskbar. **Ctrl+Alt+K** or `out\Keep-Taskbar.cmd` keeps this session's test, with crash/hang supervision active.

Maximized windows and full-height snapped panes extend to the bottom behind the floating pill. Snapped panes retain their chosen widths and original restore placement. The trial journals the original taskbar work area and restores it with Revert, timeout or crash recovery.

With a maximized or snapped app, the pill slides/fades out so it cannot cover app controls. A subtle rounded line at the bottom marks the hidden pill's full width; hover anywhere along it or press **Ctrl+Alt+Space** to reveal. The line fades away as the pill appears, and its activation area follows the pill's width as it expands. Open menus keep it visible; Escape closes keyboard interaction and returns focus. The desktop and ordinary windows keep the collapsed dock visible.

Icon clicks give a soft 240 ms bubble pop while launching/activation proceeds immediately. Native dock panels fade in and rise subtly over 180 ms. Effects respect reduced motion and stop repainting when finished. Other applications keep Windows' native opening and restore behavior.

The tray arrow uses the app icons' rounded hover highlight and two DIP lift. Wi-Fi and sound share one quick settings button: hovering either icon highlights the pair and lifts both together. Reduced motion keeps the highlight without lifting.

The left wing controls the current [Windows media session](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager): previous, play/pause and next, with the current title in Bahnschrift Light. It works with participating players and browsers without a Spotify account or API key. Unsupported buttons are disabled and explain their unavailable state on hover; no active session shows "No media." Previous/play/next have separate, wider click areas beneath the title. Overflowing titles scroll gently while the pill is fully expanded, with reading pauses at both ends; reduced motion keeps a static trimmed title. Hover the title to see the artist. Media updates run on a background worker; the title timer runs only for visible overflowing text.

Click the time/date to open a calendar centered above that button, with the same charcoal glass as the dock's other flyouts. Dock and calendar text use the clock's Bahnschrift Light font, with a shared fallback when unavailable. Month arrows and the scroll wheel navigate with a short slide animation; Today returns to the current local date. Page Up/Down changes months, arrow keys select dates, Home returns to today, and Escape or clicking outside closes it. The calendar holds the dock visible while open and respects reduced motion.

## Run the preview

Open `out\MinimaDock.exe` for the framed preview. The compact dock shows all pinned and open/minimized apps, deduplicated around the fixed crossed-swords launcher. Hover expands only the media and system/time/calendar wings; every app stays fully visible and at the same coordinates. Click the swords to search real Start Menu shortcuts. The original geometric sword mark is drawn as a cached 24 DIP native vector inside the existing 44 DIP button. Click apps to launch/activate; grouped windows use a selector. The right wing opens tray/quick settings panels with Settings links.

Preview desktop entries come from actual Desktop/public Desktop folders; double-click opens the original item. It displays up to eight desktop items and eight app slots with overflow. Native icons are extracted when available. Traditional pins merge with matching running executables. Complete packaged identity, desktop keyboard navigation and UI Automation remain unfinished.

Keyboard: Tab/left/right focus dock controls, including media buttons; Enter/Space activate, Escape closes the flyout first. `F` simulates fullscreen suppression inside the preview; `R` toggles reduced motion. The preview does not hook the Windows key. Third-party tray support is pending; disabled/pending features never issue fake OS operations.

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

MinimaDock's source, documentation and original geometric sword artwork are licensed under [MIT](LICENSE), copyright 2026 AbdulBasithMohammed. Third-party components retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). Regenerate the logo SVG and native header with `python tools/Generate-LauncherLogo.py`; no reference image or tracing is used.

Before publishing this checkout, review [public repository preparation](docs/publishing.md). Personal recovery bundles, captures and deployments must stay local.
