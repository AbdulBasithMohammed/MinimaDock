# Initial decisions

Version 0.2.3 candidate: use content-driven native canvas width up to the monitor boundary; do not impose a fixed 1,000 DIP limit. Keep the pill 44 DIP high throughout horizontal expansion. Reserve a slim always-available Show desktop control on the right, invoking the existing Shell ToggleDesktop action only on explicit activation. Testing/deployment are deferred at the user's request, and the last tested package is retained.

Version 0.2.2 candidate: balance running pins first, idle pins second, and unpinned running apps last. This balances pins, expanded apps and collapsed running apps while preserving each app's side on hover. Match packaged windows/pins by AppUserModelID and resolve registered taskbar icon metadata before generic executable icons. Read the observed version-3 pin stream conservatively and fall back on unsupported formats. Deployment takes only packaged binaries, so an unverified candidate does not replace the last tested package. Smart App Control currently prevents candidate GUI validation/deployment.

Version 0.2.1 responsiveness decision: preserve the approved scale; begin hover/exit transitions immediately with 90 ms reversible motion. Give each app one side in both layouts and preserve established ordering across foreground changes. Cache larger native icons at the physical display size and draw on pixel boundaries. Use the shorter surface outside flyouts and skip redraws for unchanged inventories. These supersede the original preview's delayed hover timings.

October 10 update: the user explicitly approved temporary Explorer-backed taskbar tests. That development mode adds native transparent rendering and a separate visibility supervisor/journal without changing shell or startup settings. The original decisions below describe the first delivery; the Explorer-free production goal and its gates remain unchanged. See `taskbar-test.md`.

Design update: a 24 DIP Windows symbol stays centered in a 44 DIP button. Collapsed mode shows open/minimized apps. Hover expands pins/media/system wings symmetrically to 48 DIP height. Real icon caching, traditional-pin merging, overflow menus and native quick-settings design panels are implemented; complete Store identity, tray hosting and media/device adapters remain pending.

1. Follow the brief's recovery-first gate. Production activation, taskbar hiding and Explorer termination are absent from this build. A framed preview allows design development without a replacement experiment.
2. Use Windows-shipped PowerShell only for independent one-shot recovery/benchmark tooling. The GUI and supervisor are native C++ and do not run a persistent interpreter or browser runtime.
3. Production rollback supports one allowlisted user-shell value. New startup/work-area/wallpaper mutation types are rejected until their capture/restore behavior is added and tested.
4. Build policy activation later, after RSOP/MDM precedence and isolated recovery evidence. Do not infer managed policy state from the absence of a registry value.
5. Begin with native translucent/solid rendering and a static design backdrop. Real blur and Wallpaper Engine embedding need profiling and compatibility trials.
6. Clicking a focused running app activates it; it does not minimize it. Multiple windows use a text menu. Preview app grouping currently falls back to full executable path and must be upgraded for packaged apps before shipping.
7. The preview displays honest Settings routes and pending media/tray states. No fake Wi-Fi/Bluetooth toggles, browser-session media control or hidden Explorer tray is provided.
8. No runtime Win-key hook until chord preservation and Explorer-free keyboard behavior can be verified.
