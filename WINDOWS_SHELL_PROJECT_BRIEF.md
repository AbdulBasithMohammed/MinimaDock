# Windows 11 Custom Shell — Product and Engineering Brief

> Development exception approved by the user, October 10, 2026: temporary taskbar tests may retain Explorer and hide its taskbar while MinimaDock runs, with an independent guard and convenient revert. The user subsequently requested full-height maximized windows; temporary trials may release the hidden taskbar's work-area reservation with journaled restoration. This approval does not change the finished Explorer-free replacement requirements or authorize permanent startup/default-shell activation. See `docs/taskbar-test.md`.

This document is the handoff specification for a new Codex coding project. Read it completely before implementation. Build recovery first. Prove compatibility and resource usage before making the custom shell the user's default desktop.

## 1. Objective and non-negotiable requirements

Build a lightweight native desktop shell for **Windows 11 Pro** that replaces the Explorer desktop shell and its taskbar with our own desktop, launcher, and expanding taskbar.

- The original Windows taskbar must **not be running underneath, hidden, moved off-screen, or made transparent**. A cosmetic overlay on the existing shell does not meet the requirement.
- File Explorer remains available for browsing files, This PC, and Recycle Bin. An `explorer.exe` process hosting folder windows is acceptable; the Explorer taskbar/desktop shell returning is not.
- Maximized application windows extend to the bottom of the monitor. Our taskbar reserves no work-area strip and overlays ordinary maximized windows.
- Fullscreen games and videos must not be covered by our button, pill, flyouts, tooltips, or desktop UI.
- Preserve Wallpaper Engine if technically feasible with the Explorer desktop absent. Compatibility is unproven and must be investigated early; do not promise it or silently substitute another wallpaper product.
- **The replacement must not consume more resources than the current Windows desktop under comparable workloads.** This is an acceptance gate, not an aspiration. If required functionality cannot meet it, explain the conflict and keep the normal shell as the default.
- Recovery to the user's previous Windows desktop must exist and be tested **before any shell replacement test**.
- Do not delete Windows components, replace system binaries, disable security features, or change unrelated Windows services to manufacture compatibility or benchmark savings.

No implementation, shell change, recovery utility, performance measurement, or compatibility test has been completed merely by creating this brief.

## 2. Confirmed product design

### 2.1 Collapsed taskbar

- Bottom-center placement, with a Windows-symbol button on each monitor.
- With no running apps, the button stands alone.
- Running and minimized applications appear beside the Windows button even while the taskbar is collapsed.
- Pinned applications that are not running are revealed when the taskbar expands.
- Keep the collapsed appearance visually light; avoid drawing an empty full-width panel.
- The complete visible group is centered as its contents change. Treat this as a provisional layout default; do not force the symbol itself to remain at the exact center while all icons grow to one side.

### 2.2 Expanded pill

Hovering over the collapsed button/app group expands it into a rounded glass pill containing:

1. Windows/launcher button.
2. Running apps and pinned apps, deduplicated where they represent the same app.
3. Compact Spotify playback controls.
4. Access to quick settings and third-party system tray icons.

Detailed controls can open anchored flyouts above the pill. Avoid cramming connection lists and tray menus into one horizontal strip. Adapt to available monitor width with overflow instead of clipping.

Default interaction proposals, adjustable after the prototype:

- Expand after approximately 100–150 ms of deliberate hover; animate over 160–220 ms.
- Collapse after 300–450 ms outside the whole interaction region.
- Keep the pill open while a descendant flyout, context menu, drag operation, keyboard interaction, or pointer transition to a flyout is active.
- Reversing direction during animation continues smoothly from the current state.
- Hovering must not steal keyboard focus from the active application.
- Clicking outside or pressing Escape closes the topmost flyout first.
- Keyboard access and touch/click activation must work without requiring hover.
- Transparent space outside the actual control must pass pointer input through to the application underneath.

### 2.3 Visual language

- Dark theme with an Apple iOS / Liquid Glass-inspired appearance: translucent dark surfaces, rounded contours, restrained highlights, subtle depth, and smooth motion.
- The look is inspiration, not a requirement for expensive real-time refraction or a pixel-for-pixel reproduction.
- Suggested initial tokens: 44–48 DIP button size, 26–30 DIP app icons, 6–8 DIP spacing, 8–12 DIP bottom margin, 1 DIP low-opacity border, and capsule radius equal to half the surface height.
- Suggested colors: dark charcoal tinted glass, soft white primary content, muted secondary content, and one restrained accent for active controls. Exact colors remain tunable.
- Use small shadows and one subtle edge highlight. Avoid continuous shimmer, bouncing, moving gradients, or idle animation.
- Prefer supported compositor materials after profiling. Provide a translucent/solid fallback where blur is unavailable, disabled by accessibility settings, or too costly.
- Honor reduced-motion, high-contrast, transparency settings, DPI scaling, and visible keyboard focus.
- Animated wallpapers behind translucent UI may increase compositor work even when our process is idle. Include this cost in performance measurements.
- Theme editing can come later. Centralize tokens now so it does not require a rendering rewrite.

### 2.4 Running applications and launcher

Initial defaults (proposed during planning, not individually confirmed by the user):

- Group windows by application, with a small indicator for running state and multiple windows.
- Clicking a minimized app restores it; clicking another app activates it. For multiple windows, show a compact selector.
- Define focused-app click-to-minimize behavior explicitly before shipping; do not assume every app behaves identically.
- Begin with a lightweight text/icon window selector. Live thumbnails are optional and must satisfy performance limits.
- On multiple monitors, show windows on their current monitor, with the same pinned apps available everywhere. Shared-all-windows mode can be added later.
- Clicking the Windows symbol or pressing the Windows key opens a simple installed-app/shortcut launcher with search.
- Preserve meaningful Windows-key combinations where possible. Do not consume every Win chord just to implement a standalone Win press.
- No web search, content feeds, full-disk indexing, or elaborate launcher features in the initial version.
- Resolve app identity using appropriate Windows metadata and explicit fallbacks; titles and executable names alone are insufficient for every app.

### 2.5 Spotify

Initial scope is **basic controls**:

- Album artwork, track title, artist if space permits.
- Previous, play/pause, next.
- Disabled controls when the active session does not support an operation.
- Useful stopped/not-running state; do not automatically launch Spotify just to populate the pill.

No playlist browser, account login, separate Spotify volume, or scrubbing timeline is required initially. Prefer local Windows media-session integration with Spotify selected explicitly. Do not accidentally control a browser's media session instead. Treat Store and traditional desktop builds as separate compatibility cases.

### 2.6 Quick settings and system tray

The user's reference is the Windows 11 Quick Settings panel: Wi-Fi and Bluetooth tiles with state labels and secondary arrows, a volume slider, an audio-output chooser, and a settings entry. Reproduce that level of convenience in our visual language.

- Wi-Fi: available state/connection status, supported toggle behavior, a compact connection view where feasible, and a direct route to the exact Windows Wi-Fi settings page.
- Bluetooth: status/toggle where supported, limited device interaction, and a direct route to Bluetooth settings for pairing or advanced work.
- Audio: volume, mute, output-device selection, and a direct link to sound settings.
- System tray: third-party icons, tooltips, activation, and context menus, with overflow where necessary.
- A clock and power menu are reasonable proposed utilities; their exact placement is not yet confirmed.
- Airplane mode, Accessibility, Energy Saver, and Live Captions appear in the reference image but were **not explicitly requested as mandatory initial features**.
- Surface permission/device errors truthfully; never display a successful toggle before the OS confirms it.
- This is our panel with real OS integration. Do not assume Explorer's existing Quick Settings flyout is available without its shell.
- System tray hosting and Windows toast/notification-center behavior are separate problems. Assess and document each; do not claim notifications are preserved because tray icons are visible.

### 2.7 Desktop and shortcuts

- Familiar desktop icon arrangement; names hidden at rest for a minimal appearance.
- Subtle enlargement/lift on hover with the name revealed. Also reveal the name on keyboard focus/selection and expose it to accessibility tools.
- Default double-click activation, matching the current desktop. Keep hover animation from moving the click target away from the pointer.
- Read the user's actual Desktop known folder (including redirection/OneDrive) and the public Desktop rather than assuming fixed paths.
- Preserve existing files and shortcuts. Store our own layout separately; do not rearrange, delete, rename, or rewrite the original files for styling.
- Custom items may include Recycle Bin, This PC, games, apps, folders, and files.
- Recycle Bin and This PC must open their standard File Explorer views.
- Launch game shortcuts with their existing target, arguments, working directory, and launcher/protocol semantics. Do not bypass launchers or DRM.
- Open documents with normal Windows file associations.
- Support useful keyboard navigation and a basic context menu. Full Explorer drag/drop, shell-extension menus, file-management commands, and exact icon-layout migration require explicit implementation; they are not automatic consequences of drawing icons.

### 2.8 Maximized versus fullscreen windows

- Reserve zero taskbar space. Ordinary maximized windows should fill the monitor's usable area down to the bottom while the small control remains accessible above them.
- Do not implement this by injecting into applications or continually forcing their window rectangles.
- Fullscreen browser video, F11 browser mode, exclusive fullscreen, and borderless fullscreen games should suppress all shell overlays on the affected monitor.
- Suppress hover activation in fullscreen; reaching the bottom edge during gameplay/video must not reopen the pill.
- Restore controls when the fullscreen state ends or focus returns to an ordinary application, with appropriate per-monitor behavior.
- **Window bounds alone cannot classify fullscreen:** in this product ordinary maximized windows can also cover the full monitor.
- Build a tested classifier using window state/style, foreground transitions, monitor bounds, application evidence, and explicit per-app overrides where necessary. No universal perfect detector should be assumed.
- Test Alt+Tab, minimizing/restoring, display changes, and fullscreen on secondary monitors. Fullscreen on one display should not unnecessarily suppress controls on another.
- Define deliberate launcher activation during fullscreen separately; default to keeping overlays suppressed unless the user explicitly invokes the launcher.

## 3. Recovery must be the first implemented deliverable

### 3.1 User-facing recovery contract

Provide a single obvious recovery entry point, provisionally **`Restore-Windows.cmd`**, that can be launched without our desktop or taskbar. A native recovery executable is also acceptable. The entry point must depend only on Windows-shipped facilities or an independently packaged recovery binary, not the custom shell being healthy.

The preferred experience is one file the user runs to restore the original Windows desktop. It may use a colocated, versioned backup manifest and helper internally. If so, distribute them as one clearly identified recovery bundle and explain that the launcher alone is not the backup.

Recovery must:

1. Disable our automatic startup/restart behavior so it cannot fight recovery.
2. Restore the exact pre-install shell configuration and other settings that our activation changed.
3. Stop only our authenticated/identified processes for the affected user session.
4. Start the normal Explorer desktop in that user's interactive session where appropriate.
5. Verify that the Windows taskbar and desktop have returned, and that the next sign-in will use the restored configuration.
6. Report failures and any required sign-out clearly; do not force logoff/reboot or terminate unrelated apps.

Successful same-session restoration is the target. If a Windows build requires sign-out for a setting to take effect, preserve the next-login restoration, report it, and allow the user to save work first.

### 3.2 Capture actual state before changing anything

Implement a read-only preflight and backup transaction first. Capture:

- Windows version/build/architecture, user SID, session, and the effective shell configuration.
- Every registry value the activation will touch: path, value name, exact type, data, and whether the value/key originally existed.
- Relevant policy source and precedence. If managed policy controls the shell, report the conflict instead of rewriting machine-wide or organizational policy.
- Only our relevant startup entries, scheduled tasks, or services, including previous state if modifying an existing owned entry. Prefer creating uniquely named entries rather than repurposing anything existing.
- Display configuration and work-area state relevant to rollback. Do not replay stale monitor coordinates blindly after a display change.
- Any wallpaper/startup changes we actually intend to make. Avoid changing Wallpaper Engine's own settings unless needed and backed up.

Record a versioned manifest, integrity checks, original user identity, and a transaction journal. Write atomically and verify readback before proceeding. Protect the original baseline against overwriting on reinstall or repeated tests. Retain it until the user deliberately removes the recovery bundle.

Do not use a broad registry import that overwrites unrelated changes. Restore only settings we own or changed, including removing values that were originally absent. Detect unexpected later edits and report conflicts rather than silently destroying them.

If the backup is missing, invalid, or mismatched to the user, **do not claim exact restoration**. An explicit emergency “use standard Explorer” fallback may be offered separately, with a clear explanation that it does not reconstruct the captured prior state.

### 3.3 Recovery must not depend on the UI being tested

- Store the recovery bundle at a stable local path outside temporary build folders and network/OneDrive dependencies.
- Provide the exact Task Manager → Run new task command, including quoting paths with spaces, and a documented Ctrl+Alt+Delete recovery route.
- Test recovery while the shell is running, crashed, hung, absent, and during failed startup.
- Do not elevate by default. If elevation is necessary, preserve the intended user's SID and interactive session; an administrator's HKCU must not be mistaken for the user's settings.
- No permanent PowerShell execution-policy change. If PowerShell is used, handle `-NoProfile`, errors, Unicode, quoting, and constrained environments deliberately.
- Recovery must be idempotent and rerunnable after partial failure.
- Never indiscriminately kill every `explorer.exe` process or every process matching a loosely chosen name.
- Restore should preserve open games, documents, and ordinary application processes.

### 3.4 Guarded testing and crash recovery

Before testing a shell replacement, arm an independent rollback guard. A small native supervisor may wait on the shell process and use bounded liveness checks. It must not run a hot polling loop or relaunch a crashing shell indefinitely.

- Use an explicit trial state with a timeout and an authenticated keep/revert decision. Trial acceptance must not automatically enable permanent startup.
- A shell crash, repeated startup failure, or trial timeout triggers recovery for the correct session.
- Handle a hung process as well as a terminated process; the UI thread must acknowledge liveness with bounded timeouts.
- Make the rollback guard survive the shell process dying. Count its memory/CPU in the product budget.
- Persist transaction state so a power interruption during activation can be detected at the next launch/sign-in.
- Test the fail-safe in a VM or disposable test account before using it on the main account.
- Include a deliberate “Restore Windows desktop” UI action backed by the same recovery engine.

### 3.5 Recovery acceptance tests — required before proceeding

- Existing value restored with its original type/data; originally absent value removed.
- Second restore produces no damage or new changes.
- Missing/corrupt backup fails truthfully and leaves an explicit emergency route.
- Shell startup failure, crash, hang, timeout, and interrupted activation recover correctly.
- Paths with spaces and Unicode work.
- Wrong-user/elevated-account invocation cannot restore the wrong profile.
- Desktop, taskbar, Start, task switching, work area, original startup behavior, and Wallpaper Engine baseline behavior are checked after restoration.
- Next sign-in uses the previous shell configuration.
- Recovery entry point is usable from Task Manager without our interface.

**Do not start by editing the live shell setting or killing Explorer. Implement and validate the above first.**

## 4. Recommended coding architecture

### 4.1 Stack and packaging

- C++20 with Win32, RAII, Unicode, and an MSVC/CMake build. Detect the actual target architecture before selecting x64 or ARM64 binaries.
- Direct2D/DirectWrite for lightweight drawing/text; use DirectComposition or supported DWM materials only where appropriate and measured.
- C++/WinRT for Windows APIs that require it; avoid adding a managed runtime just for a small integration.
- No Electron, Chromium, WebView UI, embedded browser, or continuously running scripting interpreter for the shell.
- No background web server, telemetry service, updater service, or persistent development tooling in the shipped runtime.
- Keep runtime dependencies minimal and reproducible. State the supported Windows build range and check API availability at runtime.
- A single shell process with multiple monitor windows is the default. An independent minimal recovery supervisor is justified; do not spawn a full process per monitor or widget.
- The shell runs as the normal user. Never require administrator mode for routine use or disable UAC.

### 4.2 Suggested repository layout

```text
CMakeLists.txt
src/
  app/                 # lifecycle, configuration, single-instance/session guard
  recovery/            # capture, journal, restore, verification, supervisor
  platform/            # Win32/COM/WinRT wrappers, handles, errors, identity
  shell/               # activation adapter, startup coordination, work areas
  windows/             # window inventory, grouping, activation, fullscreen
  desktop/             # known folders, shortcut model, layout, launching
  taskbar/             # collapsed/expanded state machine and per-monitor views
  launcher/            # installed apps, shortcuts, incremental search
  tray/                # isolated compatibility adapter and overflow UI
  media/               # Spotify session selection, metadata, controls
  settings/            # audio, network, Bluetooth, settings links
  wallpaper/           # Wallpaper Engine experiment and lifecycle adapter
  rendering/           # surfaces, text, cached resources, animation scheduler
  accessibility/       # UI Automation and keyboard behavior
tools/
  recovery/            # recovery entry point and standalone packaging
  benchmark/           # reproducible capture and comparison tooling
tests/
  recovery/
  state_machines/
  integration/
docs/
  recovery.md
  compatibility.md
  performance.md
  decisions.md
```

### 4.3 Runtime design

- UI thread runs a normal Windows message loop and owns its windows/rendering objects.
- Keep COM apartment requirements explicit. Use a small bounded worker pool for slow shell metadata, icons, artwork, and device enumeration.
- Never block the UI thread on network/device calls, shell extensions, process launch, or filesystem scans.
- Separate OS adapters, application models, and rendering. Render immutable snapshots where useful; marshal async completions to the correct thread.
- Prefer subscriptions: window events, foreground changes, directory change notifications, media-session events, and device/audio callbacks.
- Debounce event bursts and reconcile state after missed events or resume. Avoid continuous full-process/full-window scans.
- Cancel stale async requests; an old media/device response must not overwrite newer state.
- Bound queues, cache sizes, and retry counts. Stop listeners/timers cleanly during recovery.
- Treat icon extraction and shell extensions as potentially slow/unreliable. Use timeouts/fallback icons and isolate only where evidence warrants the additional process cost.
- Keep logs bounded and low-volume; release builds must not write on each frame, pointer move, or idle tick.

### 4.4 Explicit UI state machines

Taskbar states: `Collapsed`, `Expanding`, `Expanded`, `Collapsing`, `SuppressedFullscreen`, `Recovering`.

Launcher/flyout state is tracked separately. Track why expansion is held open (pointer, keyboard focus, context menu, dragging) rather than scattering booleans across handlers. Fullscreen suppression overrides ordinary hover, closes transient surfaces, and cancels animation work. Recovery overrides all ordinary UI behavior.

Use a finite animation scheduler: request frames only while values are changing, stop at the terminal frame, and destroy/release unnecessary surfaces when appropriate. Do not maintain a perpetual 60/120/144 Hz render loop.

### 4.5 Windows integration boundaries

- **Shell activation:** Windows 11 Pro supports the user-scoped Custom User Interface policy. Determine local policy/registry behavior on the installed build and handle effective policy state correctly. Preserve captured values and use the recovery transaction for every mutation.
- **App launching:** use Windows shell activation APIs and known-folder identities where appropriate. Preserve `.lnk` and `.url` behavior. Validate that opening folder views does not recreate the Explorer taskbar.
- **Window inventory:** exclude our windows, tool windows, cloaked/ineligible windows, and unsuitable owned dialogs with tested rules. Account for packaged apps, elevated apps, virtual desktops, and monitor changes. Do not rely solely on process names.
- **Audio:** use Core Audio interfaces and change notifications for volume, mute, and endpoints. Verify whether default-output switching requires an unsupported interface on the target build; do not pretend endpoint enumeration alone provides supported switching.
- **Wi-Fi/Bluetooth:** use documented native/WinRT APIs as applicable and test permissions in the selected packaging model. Exact Settings links are valid fallbacks for unsupported operations, visibly labeled.
- **Spotify:** investigate `GlobalSystemMediaTransportControlsSessionManager` and its event model. Validate API access/package capabilities and Spotify session identity rather than assuming universal availability.
- **System tray:** `Shell_NotifyIcon` documents how apps submit icons to the Windows shell; it is not a turnkey public API for creating a replacement tray host. Hosting compatibility may need a separately reviewed compatibility implementation. Investigate existing maintained implementations and licensing before reuse. Handle icon lifetimes, GUID/ID identity, updates/removal, callback versions, menus, coordinates/DPI, restart notification, and malformed cross-process data. Never revive hidden Explorer just to claim success.
- **Keyboard:** implement launcher invocation without swallowing ordinary shortcuts. Assess Win+E, Win+D, Alt+Tab, snapping, and virtual desktops explicitly; do not promise Explorer-dependent shortcuts will survive automatically.
- **Wallpaper:** normal Wallpaper Engine desktop integration depends on Explorer's desktop. Its documented standalone window playback is an experimental route, not proof of embedded compatibility. Test stacking, focus, interaction, monitor/DPI changes, startup order, pause/resume, GPU cost, and restoration. Avoid fragile undocumented desktop-window impersonation as the assumed architecture.

Keep unsupported or build-sensitive integrations behind adapters, with a compatibility matrix and visible fallback behavior. If a mandatory feature requires violating a non-negotiable requirement, report the conflict; do not quietly change the product definition.

## 5. Performance is a release gate

### 5.1 Establish the actual baseline

Measure the user's existing Windows desktop **before replacing or modifying it**. There are no trustworthy baseline numbers yet. Do not invent a RAM target and call it equivalent to the current machine.

Record Windows build, CPU/GPU, RAM, monitor count/resolutions/scaling/refresh rates, power mode, active wallpaper and Wallpaper Engine settings, app versions, and representative startup/tray apps. Use the same conditions for stock and custom runs.

Measure both whole-system effects and attributable components:

- Private committed memory and working set of our full process tree, recovery guard, and helpers; compare with the relevant stock shell components.
- Total system committed memory and physical-memory pressure to catch shifted costs and shared-memory effects.
- CPU time over a fixed interval, with explicit units; report idle and active scenarios separately.
- GPU engine utilization/time and dedicated/shared GPU memory, including DWM and Wallpaper Engine changes.
- Wakeups/timer activity, steady-state disk/network activity, launch/interaction latency, and sustained-operation leaks.

Do not add process working sets naively as if shared pages were unique. Do not claim savings merely because work moved into DWM, Wallpaper Engine, a service, or a helper.

### 5.2 Benchmark scenarios

Run warmed-up, repeated A/B trials with matched apps, wallpaper, settings, and duration. Include cold startup separately. Record medians and spread, not a single favorable screenshot.

1. Desktop idle with the user's animated wallpaper running.
2. Normal browsing/work with windows maximized to the bottom.
3. Repeated pill expansion/collapse and desktop-icon hover.
4. Spotify playing, track changes, and controls visible/hidden.
5. Quick settings and third-party tray interactions.
6. Fullscreen browser video and representative borderless/exclusive games.
7. Mixed monitor usage, sleep/resume, and display hotplug.
8. Extended operation and repeated open/close cycles for leaks.
9. File Explorer open in both baseline and replacement runs.

Use ETW/WPR/WPA or equivalent native tooling for targeted diagnosis; benchmark collection tooling must not ship as an always-on product dependency. Apply identical instrumentation overhead to both sides.

### 5.3 Pass/fail policy

- Every measured resource dimension must show no reproducible regression under matched supported scenarios. Lower RAM does not excuse higher idle CPU/GPU or poorer gaming performance.
- Define measurement noise/repeatability before judging the result. Results within uncertainty are inconclusive, not proof of improvement; repeat targeted measurements.
- Idle UI aims for effectively zero ongoing work beyond necessary OS subscriptions and coarse, relevant updates. No fixed numeric RAM/CPU promise until measured.
- Budget includes the recovery supervisor, wallpaper integration, tray compatibility, flyouts, and any helper processes.
- Prefer lower-cost visual effects when needed. If the agreed functionality still exceeds baseline, do not enable it as the permanent replacement or label it compliant.
- Store raw captures, build identifiers, test configuration, and a concise comparison report so results are reviewable and reproducible.
- A finite test set cannot prove every future workload. Clearly state tested workloads/builds and revalidate after changes affecting resource use.

### 5.4 Coding rules for efficiency

- No per-frame allocations, synchronous icon extraction, or full-layout rebuilds for a small state change.
- Cache icons/text/artwork at the sizes actually used; cap caches and release unused GPU resources.
- Load artwork only when useful, downsample once, and reuse it across monitor views.
- No high-frequency polling for Spotify, device state, windows, or mouse position where events suffice.
- Do not raise system timer resolution globally for cosmetic animation.
- Minimize translucent surface area; transparent monitor-sized windows can still incur composition cost.
- Suspend invisible/fullscreen-suppressed UI work. Event reception does not require continuous rendering.
- Use bounded retries and backoff after failing devices/sessions, with explicit user-visible state.
- Optimize measured hot paths, not speculative complexity. Correct lifetime handling, ownership, and recovery come before micro-optimizations.

## 6. Implementation sequence and milestone gates

### Milestone 0 — Inventory and baseline

Read repository instructions. Inspect toolchain and Windows build, monitor topology, existing shell customization, relevant startup apps, and Wallpaper Engine/Spotify installation details. Gather only task-relevant state. Capture the stock resource baseline without modifying the desktop.

Deliver: environment report, supported-build proposal, initial compatibility matrix, baseline captures.

### Milestone 1 — Recovery first

Implement capture/restore, the standalone entry point, transaction journal, rollback guard, and recovery documentation. Exercise real restoration on an isolated account/VM with representative configuration. Prove Task Manager invocation and next-sign-in restoration.

**Gate:** recovery tests pass and the user has an accessible recovery bundle. No live shell switch before this gate.

### Milestone 2 — Compatibility spike

With guarded temporary activation, test:

- Custom shell starts in place of Explorer on Windows 11 Pro.
- Original taskbar is absent, including secondary taskbars, throughout the test.
- This PC, Recycle Bin, and folder windows open normally without reintroducing it.
- Wallpaper Engine can function in the proposed architecture.
- Tray hosting works with representative installed apps.
- Fullscreen suppression and zero-reservation maximization coexist.
- Essential startup apps, settings access, and keyboard navigation remain usable.

Deliver a factual pass/fail/partial report with build-specific limitations. Stop architectural expansion if a mandatory dependency fails; present alternatives with their resource/feature consequences.

### Milestone 3 — Native functional desktop

Implement per-monitor controls, window switching, pinned apps, a simple launcher, and desktop shortcuts with basic styling. Include restore access. Validate DPI, accessibility, keyboard behavior, and event lifetimes.

### Milestone 4 — Integrations

Add basic Spotify, quick settings, tray overflow/menus, wallpaper lifecycle, and startup coordination. Avoid duplicate launches of apps already started by Windows, services, or scheduled tasks. Handle missing devices and permissions clearly.

### Milestone 5 — Glass and animation

Apply the agreed dark glass language, hover states, layout transitions, and responsive flyouts. Profile each significant effect; keep lower-cost alternatives.

### Milestone 6 — Performance and daily-use verification

Run the A/B suite and compatibility matrix, fix regressions, and test the full recovery path again. Test updates with an incompatible or failed new shell binary. Keep a working recovery tool independent of shell-version upgrades.

### Milestone 7 — Opt-in permanent activation

Only after recovery, compatibility, and performance gates pass, present the actual measured result and known limitations. Changing the user's default sign-in shell is a separate explicit activation step. Providing this brief does not itself authorize changing that setting now.

Deliver source, reproducible build instructions, packaged application, recovery bundle, benchmark evidence, compatibility notes, and concise operating instructions.

## 7. Product acceptance checklist

- [ ] Recovery exists first and restores the captured previous desktop configuration.
- [ ] Original Explorer taskbar is absent rather than hidden.
- [ ] No Explorer taskbar returns after File Explorer/Recycle Bin/This PC launches.
- [ ] Button is bottom-center on each monitor; running/minimized apps remain beside it.
- [ ] Hover expands pinned apps, media controls, and system access without stealing focus.
- [ ] Dark glass appearance is smooth and passes the performance gate.
- [ ] Desktop icons preserve original files, launch normally, and reveal labels accessibly.
- [ ] Ordinary maximized windows reach the bottom; fullscreen content remains unobstructed.
- [ ] Spotify basic playback and quick settings work with truthful error states.
- [ ] Required tray apps retain usable activation/context menus.
- [ ] Wallpaper Engine compatibility and startup/pause behavior are proven or the conflict is explicitly resolved with the user.
- [ ] Multi-monitor, DPI, sleep/resume, hotplug, and keyboard navigation are tested.
- [ ] All product processes and compositor effects are included in the resource comparison.
- [ ] No reproducible resource regression is present in the agreed benchmark suite.
- [ ] Permanent activation is reversible and separately chosen by the user.

## 8. Remaining facts to discover, not reasons to repeat the design discussion

The user's OS edition is confirmed as Windows 11 Pro. Discover the actual build and architecture, monitor count/scaling, hardware, Spotify variant, Wallpaper Engine installation and wallpaper types, startup/tray apps, and any existing shell customization during preflight. Exact visual tokens and provisional interaction defaults can be refined with the prototype.

The highest-risk unresolved points are Wallpaper Engine integration without Explorer's desktop, replacement tray hosting, preserving folder browsing without restoring the taskbar, fullscreen classification when maximized windows fill the monitor, and total-system performance with glass over an animated wallpaper.

## 9. Primary references

Verify current documentation and applicability to the installed Windows build while implementing; these links establish direction, not guaranteed compatibility.

- [Custom User Interface / CustomShell policy — Windows 11 Pro support](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-admx-winlogon#customshell)
- [Shell Launcher overview and edition requirements](https://learn.microsoft.com/en-us/windows/configuration/shell-launcher/) — this separate feature is not included in Pro.
- [Application desktop toolbars and reserved screen space](https://learn.microsoft.com/en-us/windows/win32/shell/application-desktop-toolbars)
- [Fullscreen appbar behavior](https://learn.microsoft.com/en-us/windows/win32/shell/abn-fullscreenapp) — behavioral reference; do not assume these notifications will provide our detection without Explorer.
- [Launching applications and shell objects](https://learn.microsoft.com/en-us/windows/win32/shell/launch)
- [Notification area and application-side icon APIs](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)
- [Windows global media-session manager](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager)
- [DWM system backdrop types](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)
- [Wallpaper Engine desktop/Explorer and GPU relationship](https://help.wallpaperengine.io/en/performance/dwm.html)
- [Wallpaper Engine command-line controls and standalone window playback](https://help.wallpaperengine.io/en/functionality/cli.html)

## 10. Suggested opening instruction for the new Codex project

> Read WINDOWS_SHELL_PROJECT_BRIEF.md completely and treat it as the product specification. Begin with environment inventory and the recovery milestone. Implement a standalone, tested way to restore my current Windows desktop, taskbar, and shell configuration before attempting any replacement. Preserve my existing desktop files and settings. Build a native, event-driven Windows 11 Pro shell and prove all compatibility assumptions with guarded trials. Do not hide the original taskbar and call it a replacement. Do not make the custom shell the default until recovery, compatibility, and total-system performance gates pass and I explicitly choose activation. The replacement must not use more resources than my current desktop under comparable workloads. Report unknowns and measured results honestly.
