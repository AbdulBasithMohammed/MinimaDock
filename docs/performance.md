# Performance evidence and release gate

No resource-compliance claim is made. The brief requires every resource dimension to avoid reproducible regressions in matched workloads, including DWM, wallpaper and the independent guard.

After the user resumed testing, version 0.2.3 rendered its real inventory on a content-sized 776 by 88 physical-pixel canvas. `captures/deploy-023.json` contains 96 cached frames: median 0.4006 ms, p95 0.5248 ms, maximum 0.7766 ms. The PNG sibling was visually inspected. These remain narrow hidden-window frame timings, not pointer-to-display latency or release resource acceptance. The prior deferral below records the earlier state.

Version 0.2.3 sizes the 88 DIP-high normal canvas from content, capped by monitor width, instead of using a fixed 1,000 DIP canvas. This invalidates treating the older fixed-size rendering timings below as measurements of this candidate. No new GUI benchmark was run because the user deferred desktop testing and the prior unsigned-build launch block is unresolved.

## Hover responsiveness development check, October 10

The old state machine deliberately waited 125 ms before hover expansion and 375 ms before collapse, then animated for 190 ms. Version 0.2.1 removes both waits and uses a reversible 90 ms transition. Unchanged window inventories no longer rebuild icon textures or trigger redraws. Normal desktop frames use a 1000 by 88 DIP surface instead of 1000 by 440; panels still require the taller surface. Native icon assets are cached at their actual physical display size.

`captures/hover-final.json` records 96 warmed cached native frames in a hidden dock window: median 0.3946 ms, p95 0.488 ms, maximum 0.7991 ms on this development machine. The renderer exported `captures/hover-final.png` for visual inspection. `captures/hover-before.json` retains the earlier larger-surface microbenchmark (median 0.4692 ms, p95 0.5624 ms). These runs cover cached rendering only, with differing surface sizes and icon handling; they do not measure end-to-end input latency, sustained resource usage, compositor presentation, or the release acceptance workloads. Run `MinimaDock.exe --benchmark-render <absolute-json-path>` to collect the same narrow diagnostic.

## Resource acceptance remains pending

`tools/benchmark/Capture-Baseline.ps1` records per-process private commit/working set/CPU time, total system commit and available physical memory at one-second intervals. PID creation time prevents mixing reused PIDs. CPU intervals use seconds and percentage of one core; unavailable counters stay null. Individual working sets are not added into a purported unique-memory total. Collection runs only during a requested capture; it is not a shipped daemon.

The initial `captures/stock-20261010T025722Z.json` is a 30-second **development-session pilot** with 31 samples. Its early scenario label says `desktop-idle-pilot`, but compilation overlapped the capture; it must not be used as a clean idle acceptance baseline. It verifies the collection pipeline and keeps the raw evidence. Shell-related CPU counters showed no measured increment in that interval; this is not proof of zero idle work or a meaningful comparison.

For a fresh stock capture, first warm the desktop, ensure no build or instrumentation jobs are running, and document the actual app/wallpaper/power conditions:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\benchmark\Capture-Baseline.ps1 -Mode stock -Seconds 60 -Scenario 'desktop-idle-wallpaper'
```

The collector verifies Explorer's desktop before labeling a run stock. Keep raw captures with the executable build hashes and matched configurations. The current collector is incomplete: GPU engine time/memory, wakeups/ETW, total CPU effects, disk/network activity, refresh/scaling, app/media/wallpaper versions, latency and leak behavior still require dedicated tooling.

Before activation, perform warmed repeated A/B trials for all nine scenarios in the brief; define noise/repeatability first; report medians/spread and inconclusive results honestly. Include folder Explorer processes equivalently on both sides. Glass over animated wallpapers can move cost into DWM; a small preview executable or fewer private bytes is not an acceptance result. Lower-cost translucent/solid rendering remains the default until actual blur/material costs are measured.
