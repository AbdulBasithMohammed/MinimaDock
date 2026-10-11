# Development

The compact and expanded states share one balanced app rank per side. `DockLayout::app_x` has no hover or running-state input; expansion changes only the capsule edges and utility visibility. Compact width is derived from the full visible app count. Idle pins render and receive press/click input at full opacity in both states. Native layout checks compare exact coordinates on reveal/collapse and exercise every collapsed pinned/running press target.

Use `tools\Package.ps1` after the portable build to reproduce the binaries and Deploy/Keep/Revert launchers in `out`. Active deployment folders and journals are retained.

For the temporary desktop taskbar test, run `tools\Build-Portable.ps1`, then `out\Deploy-Taskbar.cmd`. Revert before changing builds with `out\Revert-Taskbar.cmd`. The deployment coordinator copies each build into a retained directory so the currently running recovery binary is never overwritten. See `docs/taskbar-test.md`. `tools\Test-DesktopDeployment.ps1` changes live taskbar visibility for bounded test cycles and requires the user's temporary-test authorization; it is not part of an ordinary build.

Version 0.2.6 also changes the live trial's monitor work area with journaled recovery. `tools\Test-DesktopDeployment.ps1` verifies a real already-maximized fixture reaches the monitor bottom and preserves its restore placement, then checks restoration after timeout, Keep/Revert, supervisor termination, dock crash and UI hang. Its geometry/probe helpers are built by `tools\Build-Portable.ps1`; the geometry test changes no live desktop state. Work-area recovery requires the updated supervisor; packaging will reject an older retained supervisor when implementation differs.

`--snapshot-dock <absolute-path.png>` exports the native dock on a transparent canvas. The framed preview keeps the geometric backdrop. The live layer has no artificial desktop background. Rendering is idle outside input/inventory updates and finite animation; a minute timer updates the clock and the separate guard checks health every two seconds.

`--snapshot-tray-hover`, `--snapshot-wifi-hover` and `--snapshot-sound-hover` export each system icon's hover state with the same absolute PNG path argument.

`--snapshot-clock-hover`, `--snapshot-calendar` and `--snapshot-calendar-motion` export the clock button, calendar and a 60 ms month-slide frame. Calendar snapshots use the current local date.

`--snapshot-media` and `--snapshot-media-paused` export native playing/paused media visuals with explicitly illustrative "Midnight Drive / Local player" metadata; they do not control a player. Ordinary snapshots show the empty media state.

## Preferred MSVC build

Use Visual Studio 2022 with Desktop development with C++ and a Windows 11 SDK. CMake 3.24+ is required. Choose `msvc-x64` for this machine (AMD64); ARM64 presets are provided but have not been compiled/tested. Standard-user execution and Per Monitor V2 DPI awareness are declared in the manifest.

```powershell
cmake --preset msvc-x64
cmake --build --preset release-x64
ctest --preset test-x64
cmake --install build\msvc-x64 --config Release --prefix out
```

The current environment has no MSVC/Windows SDK/CMake installation. MSVC presets have not been executed here. The source was compiled with portable LLVM/MinGW against its bundled Windows headers/import libraries. No managed/browser runtime is used by the GUI or guard.

## Portable verification toolchain

Used release: [llvm-mingw 20261006 / LLVM 23.1.3](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006). Archive: `llvm-mingw-20261006-ucrt-x86_64.zip`. GitHub asset SHA-256 verified before extraction:

```text
317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a
```

It lives in `.tools\llvm-mingw-20261006-ucrt-x86_64`; no service or global PATH was installed. It is development-only and excluded from packaged runtime files. Build with:

```powershell
.\tools\Build-Portable.ps1
.\tools\Test-Guard.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\recovery\Recovery.Tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\integration\Preview.Smoke.ps1
```

`tests\integration\Desktop.Action.ps1` additionally activates the Show desktop sliver, briefly shows the desktop and restores windows using a real test fixture. Run this check in the interactive desktop outside games; it verifies window state as well as the Shell call's HRESULT, and fails if Windows reports success without changing the fixture.

`tests\integration\Dock.AutoHide.ps1` checks maximize/restore, actual pointer reveal at both ends of the pill-width indicator, indicator fade, sensor alignment/width, flyout/keyboard holds, click-through style, focus return, finite animation and a real fullscreen fixture. It temporarily activates only its own blank fixture and moves the pointer across its own dock windows, then restores focus/cursor. Both checks share `Desktop.Fixture.cs`; they never automate application content.

`tests\integration\Dock.Popups.ps1` sends actual hit-tested clicks to the dock's launcher and volume regions for 64 open/close transitions. It verifies a freshly presented frame before each operation returns, stable screen anchoring, fixed pill height/opacity and no rendering-resource allocations during toggles. Timings describe this native component check, not physical pointer-to-display latency. Read-only frame diagnostics do not install an external input hook.

`tests\integration\Dock.Feedback.ps1` checks real launcher press/release, action dispatch without waiting for animation, intermediate panel fade/slide frames, finite timer shutdown and rapid keyboard reveal/cancellation. Click pulses are carried by app identity through inventory updates; `pin_state_tests` checks their survival on changed and unchanged scans. `--snapshot-click` and `--snapshot-panel-reveal` export deterministic 60 ms frames using the actual native scene. The desktop panels use a cached Direct2D layer for opacity; framed preview panels remain immediate so native child edit controls stay synchronized. Reduced-motion settings settle all feedback. External application activation keeps the existing `ShowWindowAsync(SW_RESTORE)`/`SetForegroundWindow` and Shell launch behavior.

`tests\integration\Dock.Calendar.ps1` checks actual clock/calendar button clicks, partial scroll-wheel accumulation, animated month navigation, rapid year rollover, keyboard selection/Today, dismissal, retained rendering resources and fixed dock bounds. It temporarily activates only its own blank outside-click fixture and restores focus/cursor afterward. Run UI checks sequentially. The platform-independent `calendar_tests` cover Gregorian grids, leap years, supported date bounds, selection and finite/reduced-motion slides. `Dock.AutoHide.ps1` also checks calendar visibility holds and focus return on close.

The calendar is a separate owned native layered window on the GUI thread, with cached Direct2D/DirectWrite resources. Opening rises eight DIP over 180 ms, closing fades over 140 ms, and month grids slide over 200 ms. Motion timers stop when settled; a minute timer updates today's marker. Browsing or selecting dates does not change the Windows date or create calendar events.

Calendar placement is centered on the clock button, with a 12 DIP visible gap above the pill and monitor work-area clamping. The calendar and dock share neutral glass gradient stops and Bahnschrift Light typography through `rendering/theme.h`; fonts fall back to Segoe UI Variable Text, then Segoe UI. Symbol glyphs keep Segoe Fluent Icons. Calendar integration checks placement as well as interaction behavior.

`src/windows/media.cpp` uses Windows' GlobalSystemMediaTransportControlsSessionManager on a separate WinRT MTA worker. It observes current-session, session-list, playback and metadata events; a five-second reconciliation catches omitted notifications. Commands wake the worker immediately, are bounded to eight queued requests, and retain both source identity and a session generation so a handoff cannot redirect an old click. Capabilities are checked again before dispatch. A one-time 150 ms post-command read covers providers that acknowledge before applying state. Async waits are bounded and cancellation-aware. The GUI thread performs no WinRT media calls. A retained DirectWrite layout measures and clips the title; `MediaMarquee` scrolls only visible overflowing titles at a separate 33 ms timer, with pauses at both ends. Collapse, hiding, fullscreen, flyouts, fitting titles and reduced motion stop that timer. Unchanged title/artist updates retain the layout. Minimal SDK ABI declarations allow the existing LLVM/MinGW build without a new runtime dependency; only Windows' RuntimeObject imports are added.

Build `media_tests`, then run `build\portable\media_tests.exe build\portable\MinimaDock.exe` on the interactive desktop. It registers a silent owned SMTC fixture and verifies title/artist/status/capabilities, commands, events and actual dock hit targets. It never sends transport requests to other players. If Windows selects a competing playing session when the fixture pauses, it verifies handoff/stale-command rejection and explicitly reports that the remaining fixture sequence was skipped. Stop competing playback manually before running when full fixture coverage is desired. The fixture closes its session and restores focus. It is built by the portable build but is not an automatic headless CTest case.

`media_tests --inspect` reads the current source identity, capabilities and session generation without transport commands or track metadata. The fixture tests Next/Previous before pausing, ensuring those commands are covered even if pausing causes a handoff. `media_marquee_tests` checks timing, bounds and accessibility/visibility reset. `media_ui_tests` uses actual DirectWrite/WIC rendering to check retained layout, clipping, fixed height and separated hit geometry.

Play/Pause button clicks queue explicit intents. Dispatch checks the fresh state and never toggles an already-correct state; toggle-only providers are used only when a state change is needed. Playback state/capabilities are published before metadata retrieval with the matching retained title/artist. Native rendering tests compare transport-region pixels across playing/paused/playing states to verify the actual symbol swap and unchanged neighboring controls.

`tests\integration\pin_state_tests.cpp` exercises the actual Scene inventory merge and layout with opaque fixture identities, without opening applications or changing system pins. It verifies stable expanded coordinates and width, one merged pinned icon, and running indicator state through repeated open/group/minimize/close transitions. It is built/run by the portable build and registered with CTest. Model checks cover all pinned running subsets, preservation when counts remain balanced, no side crossing on hover, and minimum movement against exhaustive small-layout solutions when rebalancing is required.

`tests\integration\Dock.Snapping.ps1` validates native Windows snapping on an owned blank fixture: pre-existing left pane and a new right pane reach the bottom while widths, arranged state and restore placement remain unchanged. It checks prompt location-event repair of a stale bottom reservation, snapped auto-hide, restored-window visibility, Revert, timeout and standalone recovery after supervisor termination. `Snap.Fixture.cs` authenticates its owned test window and foreground before sending native snap keys; it does not send input to application content. Focus/cursor and the guarded trial are restored afterward. Runtime detection uses [IsWindowArranged](https://learn.microsoft.com/en-us/windows/win32/winmsg/winuser/nf-winuser-iswindowarranged), resolved from User32 once because SDK declarations vary.

The portable build uses C++20, warnings as errors, optimization and static C++ runtime linkage. Runtime dependencies are Windows DLLs. The x64 executables can be copied out of `build\portable`; test binaries must not be shipped. Windows' system UCRT is available on Windows 11.

## Preview snapshots

The same Direct2D scene can render once into a WIC PNG, making layout review possible without desktop overlays or a running preview process:

```powershell
.\build\portable\MinimaDock.exe --snapshot C:\YourFolder\expanded.png
.\build\portable\MinimaDock.exe --snapshot-collapsed C:\YourFolder\collapsed.png
.\build\portable\MinimaDock.exe --snapshot-desktop-hover C:\YourFolder\desktop-hover.png
```

Snapshots use real shortcut/window enumeration from the current session. The geometric background is a static design backdrop, not a wallpaper replacement or Wallpaper Engine integration.

## Architecture

`src/taskbar` is a platform-independent state machine with an explicit monotonic clock. The Win32 message loop owns D2D/DirectWrite resources. A single bounded worker enumerates shortcuts/windows; WinEvent notifications debounce updates. Fonts and brushes are cached; rendering stops at the animation's terminal frame. Shell hover is simulated within a framed window and never hooks global input, reserves appbar space or changes system timer resolution.

Current limitations: filesystem enumeration cancellation only takes effect between jobs; cloud/network redirected folders can stall a job and shutdown. Desktop file-change events, real icon caches, overflow, full app identity, UI Automation, per-monitor runtime windows, fullscreen classification, shell activation, and native device adapters are future work. Media controls depend on the player exposing a Windows session; app-specific integrations and a player selector are not implemented. These limitations must be resolved before treating the preview as the shipped shell.
