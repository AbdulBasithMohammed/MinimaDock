# Development

`src/windows/tray.cpp` owns a zero-size hidden notification receiver and stores the actual app-provided HICON, tooltip, GUID/owner identity, callback and negotiated version. Its worker accepts bounded Windows 11 notification wire records, merges only supplied fields, copies icon handles, and removes dead owners. GUI snapshots retain shared icon ownership; unchanged icon handles reuse the bitmap cache. No UI Automation or Explorer overflow window is used for the tray. The current private wire format was checked using an owned Shell_NotifyIcon fixture on this Windows build; it is not a Microsoft stability guarantee. The parser accepts known 32-bit-handle wire sizes and bounds all copied data. No third-party host library, process injection or remote-memory read is used.

For the authorized Explorer-backed trial, raw messages are forwarded with a 200 ms timeout to the captured Explorer process, authenticated by image, session and creation time. The independent supervisor ignores our receiver because it authenticates actual Explorer taskbars. TaskbarCreated requests app registration; the hidden receiver restores its routing position if displaced and prunes dead owners at a two-second interval. Callback dispatch revalidates owner identity and preserves legacy/version-4 packing. Icon rectangle queries return our tile coordinates. The actual menu is drawn by the app and retains its original style. Startup cancellation and close release the worker, receiver and icons. The default taskbar receives the same icon changes throughout the trial, including a dock crash.

Run `build\portable\tray_tests.exe` explicitly on the interactive desktop, outside an existing trial. It sends real Shell_NotifyIcon calls for two owned fixtures and checks capture, callback identities, distinct original menus, GUID-only updates, hidden state, custom coordinates, malformed input, deletion, startup cancellation and an owned receiver crash with Explorer mirror recovery. `tests\integration\Dock.Tray.ps1` checks the rendered custom panel, anchor/gap, finite reveal, keyboard focus, repeated Escape/toggle, and observes native popup show events to catch the old flash. Its optional Screenshot stays local. ExistingPid/Executable target an exact guarded deployment. Run UI checks sequentially; installed-app menu commands are not automated. The callback contract is documented by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw).

The compact and expanded states share one centered app group: launcher first, saved pin order, then unpinned running apps in their established order. `DockLayout::app_x` has no hover or running-state input. Hover extends both capsule edges symmetrically to reveal media on the left and system controls on the right. The native canvas and compact group remain centered on the monitor; adding/removing an unpinned app recenters the group while retaining slot order. Opening a pin only updates its running indicator. Compact width follows app count; idle pins stay clickable in both states.

Use `tools\Package.ps1` after the portable build to reproduce the binaries and Deploy/Keep/Revert launchers in `out`. Active deployment folders and journals are retained.

To install a build, run `tools\Build-Portable.ps1`, then `out\Install-MinimaDock.cmd` (it replaces the running version safely). For a timed trial of an uninstalled candidate, `tools\Taskbar-Test.ps1 -Action Deploy` still works; revert it with `-Action Revert`. The deployment coordinator copies each build into a retained directory so the currently running recovery binary is never overwritten. See `docs/taskbar-test.md`. `tools\Test-DesktopDeployment.ps1` changes live taskbar visibility for bounded test cycles and requires the user's temporary-test authorization; it is not part of an ordinary build.

Version 0.2.6 also changes the live trial's monitor work area with journaled recovery. `tools\Test-DesktopDeployment.ps1` verifies a real already-maximized fixture reaches the monitor bottom and preserves its restore placement, then checks restoration after timeout, Keep/Revert, supervisor termination, dock crash and UI hang. Its geometry/probe helpers are built by `tools\Build-Portable.ps1`; the geometry test changes no live desktop state. Work-area recovery requires the updated supervisor; packaging will reject an older retained supervisor when implementation differs.

`--snapshot-dock <absolute-path.png>` exports the native dock on a transparent canvas. The framed preview keeps the geometric backdrop. The live layer has no artificial desktop background. Rendering is idle outside input/inventory updates and finite animation; a minute timer updates the clock and the separate guard checks health every two seconds.

`--snapshot-tray-hover`, `--snapshot-wifi-hover` and `--snapshot-sound-hover` export each system icon's hover state with the same absolute PNG path argument.

`--snapshot-clock-hover`, `--snapshot-calendar` and `--snapshot-calendar-motion` export the clock button, calendar and a 60 ms month-slide frame. Calendar snapshots use the current local date.

`--snapshot-media` and `--snapshot-media-paused` export native playing/paused media visuals with explicitly illustrative "Midnight Drive / Local player" metadata; they do not control a player. Ordinary snapshots show the empty media state.

`--snapshot-launcher`, `--snapshot-launcher-actions`, `--snapshot-launcher-home`, `--snapshot-launcher-emoji`, `--snapshot-launcher-command` and `--snapshot-launcher-windows` render the launcher with this PC's real apps, files and windows (so they can contain personal file names; keep them local). `--snapshot-wifi`, `--snapshot-bluetooth`, `--snapshot-timer`, `--snapshot-prayer-clock`, `--snapshot-hover` and `--snapshot-calendar-prayers` use sample data. `--quick-settings-report <file>` and `--jump-list-report <file>` write read-only diagnostics.

Launcher sources: `src/taskbar/launcher_model.h` (matching, ranking, answers; tested by `launcher_tests`), `src/taskbar/emoji_data.h`, `src/windows/launcher_index.*` (All apps, usage, Settings pages, power, commands, window arrangement), `src/windows/file_search.*` (Recent items, Windows Search index, previews, file actions) and `src/windows/windows_key.*` (Windows-key hook). Icons are Lucide (ISC) paths generated by `tools/Generate-Icons.py` into `src/rendering/icon_paths.h`.

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

`windows/desktop.h` replaces the Shell ToggleDesktop call with an owned native minimize/restore cycle. It enumerates visible, non-tool app windows outside the dock process, skips cloaked/pre-minimized windows, and records process creation time, thread and window class before acting. ShowWindowAsync queues work; a finite 30 ms dock timer confirms actual iconic state with a 1.5 second timeout. Previously minimized windows never enter the restore set. Manual app interaction starts a new cycle; dead/replaced windows are excluded. Normal dock exit restores the current cycle. Crash recovery does not replay this in-memory window list; apps remain accessible through the restored Windows taskbar.

`tests\integration\desktop_tests.cpp` uses owned real native windows to verify minimize/restore, normal placement, maximized state, exclusion of previously minimized and closed windows, manual interaction, exit cleanup and empty desktops. `Desktop.Action.ps1` exercises the actual sliver and checks visible fixture state; the fixture does not need foreground eligibility. A fullscreen foreground app intentionally suppresses this integration path, so run it outside fullscreen. The click diagnostic requires a new action sequence and completed native state, avoiding stale success from a previous click.

`tests\integration\Dock.AutoHide.ps1` checks maximize/restore, actual pointer reveal at both ends of the pill-width indicator, indicator fade, sensor alignment/width, flyout/keyboard holds, click-through style, focus return, finite animation and a real fullscreen fixture. It temporarily activates only its own blank fixture and moves the pointer across its own dock windows, then restores focus/cursor. Both checks share `Desktop.Fixture.cs`; they never automate application content.

`tests\integration\Dock.Popups.ps1` sends actual hit-tested clicks to the dock's launcher and volume regions for 64 open/close transitions. It verifies a freshly presented frame before each operation returns, stable screen anchoring, fixed pill height/opacity and no rendering-resource allocations during toggles. Timings describe this native component check, not physical pointer-to-display latency. Read-only frame diagnostics do not install an external input hook.

`tests\integration\Dock.Feedback.ps1` checks real launcher press/release, immediate action dispatch, intermediate opening/closing fade/slide/scale frames, finite timer shutdown and rapid reversal. `media_ui_tests` pixel-checks press and bubble feedback on all seven utility targets; `calendar_ui_tests` verifies that actual calendar glyphs translate vertically without changing columns. Click pulses are carried by app identity through inventory updates; `pin_state_tests` checks their survival on changed and unchanged scans. `--snapshot-click` and `--snapshot-panel-reveal` export deterministic 60 ms frames using the native scene. Panels use a cached Direct2D layer for opacity and keep the canvas until their exit completes; closed panels stop accepting input immediately. Reduced-motion settings settle feedback. External application activation keeps the existing `ShowWindowAsync(SW_RESTORE)`/`SetForegroundWindow` and Shell launch behavior.

`tests\integration\Dock.Calendar.ps1` checks actual clock/calendar button clicks, partial scroll-wheel accumulation, animated month navigation, rapid year rollover, keyboard selection/Today, dismissal, retained rendering resources and fixed dock bounds. It temporarily activates only its own blank outside-click fixture and restores focus/cursor afterward. Run UI checks sequentially. The platform-independent `calendar_tests` cover Gregorian grids, leap years, supported date bounds, selection and finite/reduced-motion slides. `Dock.AutoHide.ps1` also checks calendar visibility holds and focus return on close.

The calendar is a separate owned native layered window on the GUI thread, with cached Direct2D/DirectWrite resources. Shared flyout motion combines an eight DIP lift, two percent scale and fade over 180 ms when opening and 150 ms when closing; reopening reverses from the current frame. Month grids slide vertically over 200 ms and hit testing follows the animated grid and flyout scale. Motion timers stop when settled; a minute timer updates today's marker. Browsing or selecting dates does not change the Windows date or create calendar events.

Calendar placement is centered on the clock button, with a 12 DIP visible gap above the pill and monitor work-area clamping. The calendar and dock share neutral glass gradient stops and Bahnschrift Light typography through `rendering/theme.h`; fonts fall back to Segoe UI Variable Text, then Segoe UI. Symbol glyphs keep Segoe Fluent Icons. Calendar integration checks placement as well as interaction behavior.

`src/windows/media.cpp` uses Windows' GlobalSystemMediaTransportControlsSessionManager on a separate WinRT MTA worker. The restored 0.2.37 adapter observes current-session, session-list, playback and metadata events, with five-second reconciliation. Commands wake the worker, retain source/session identity, and use an eight-request queue. Play/pause updates the glyph immediately and shields that intent from stale notifications for up to 1.5 seconds, with finite 40 ms pending reconciliation. Commands use explicit Play/Pause where available; capabilities are checked again before dispatch. Slow metadata waits can be interrupted by queued commands. The newer 0.2.38 latest-state transport and session lease were rolled back at the user's request; rapid clicks remain a known limitation of this older adapter. The GUI thread performs no WinRT media calls. Retained DirectWrite title measurement and the separate 33 ms marquee timer remain unchanged.

Build `media_tests`, then run `build\portable\media_tests.exe build\portable\MinimaDock.exe` on the interactive desktop. It registers a silent owned SMTC fixture and verifies title/artist/status/capabilities, commands, events and actual dock hit targets. It never sends transport requests to other players. If Windows selects a competing playing session when the fixture pauses, it verifies handoff/stale-command rejection and explicitly reports that the remaining fixture sequence was skipped. Stop competing playback manually before running when full fixture coverage is desired. The fixture closes its session and restores focus. It is built by the portable build but is not an automatic headless CTest case.

`media_tests --inspect` reads the current source identity, capabilities and session generation without transport commands or track metadata. The fixture tests Next/Previous before pausing, ensuring those commands are covered even if pausing causes a handoff. `media_marquee_tests` checks timing, bounds and accessibility/visibility reset. `media_ui_tests` uses actual DirectWrite/WIC rendering to check retained layout, clipping, fixed height and separated hit geometry.

`media_intent_tests` verifies the restored immediate play/pause feedback, stale provider updates, confirmation/rejection, sequence protection, finite timeout and session changes. The native SMTC fixture withholds its own command-message processing for 250 ms to check delayed playback feedback, and checks two queued Pause/Play requests. Run it on the interactive desktop when Windows selects the owned fixture; a competing active player is reported as a skip. The newer worker scenarios are preserved only in the private 0.2.38 rollback backup.

`desktop_tests --shell` is an explicit interactive test of Windows' real Show Desktop action and its paired restore; it affects visible applications and is not an automatic CTest case. `Desktop.Action.ps1 -Framed` tests the product's sliver/action path through the framed preview when foreground restrictions prevent the live dock test. It requires actual desktop foreground after the first click and restored fixture state after the second. Repeat with a guarded trial active to check hidden Explorer-taskbar delegation. Native candidate/minimize tests use only owned fixture windows; elevated-app behavior is delegated to Windows in temporary Explorer-backed mode.

Play/Pause button clicks queue explicit intents based on the displayed playing state. Playback state/capabilities are published before metadata retrieval with the matching retained title/artist. Native rendering tests compare transport-region pixels across playing/paused/playing states and verify the shared press/bubble effect on all seven utility targets.

`tests\integration\pin_state_tests.cpp` exercises the actual Scene inventory merge and layout with opaque fixture identities, without opening applications or changing system pins. It verifies stable expanded coordinates and width, one merged pinned icon, and running indicator state through repeated open/group/minimize/close transitions. It is built/run by the portable build and registered with CTest. Model checks cover ordered slots, unchanged coordinates while adding apps/revealing utilities, fixed height, content-derived width and monitor capacity. Inventory checks also verify saved pin reorder and stable unpinned order across foreground changes.

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
