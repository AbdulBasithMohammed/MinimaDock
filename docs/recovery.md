# Recovery bundle and current gate

The approved **temporary taskbar test** has a separate taskbar visibility/work-area journal and supervisor. Use `out\Revert-Taskbar.cmd` or Ctrl+Alt+Backspace for that mode. This original bundle restores shell configuration and does not itself restore the trial's taskbar visibility or work areas. See [taskbar test instructions](taskbar-test.md). No shell policy activation has been added.

The first deliverable is a standalone recovery engine using Windows-shipped Windows PowerShell 5.1 and .NET facilities. No running MinimaDock UI, portable compiler, network connection or permanent execution-policy change is required. Constrained Language Mode and organizational PowerShell policy may prevent execution; those environments are not yet validated.

## Your captured bundle

The initial bundle is stored in your local checkout at:

`recovery-bundle\`

The launcher alone is not a backup. Retain `baseline.json`, `journal.json`, `Recovery.psm1`, `Restore.ps1`, and `Restore-Windows.cmd` together. This repository path is local, outside build folders, and not under OneDrive. Do not delete the bundle during development or move it after activation. Preparing it captured the current user, session, OS build, shell values, monitor bounds/work areas, and presence of Explorer's desktop. No shell, startup, work-area or Wallpaper Engine setting was changed.

## Recover without the custom interface

1. Press Ctrl+Alt+Delete, choose Task Manager, and choose **Run new task**.
2. Leave **Create this task with administrative privileges** unchecked.
3. Enter the command below, replacing `C:\Path\To\MinimaDock` with your actual checkout path:

```text
"C:\Path\To\MinimaDock\recovery-bundle\Restore-Windows.cmd"
```

The script reports settings conflicts and errors, never forces sign-out/reboot, and never kills all Explorer processes. If same-session desktop verification fails, it reports that sign-out may be required so you can save work first. Actual Task Manager invocation while Explorer is absent and next-sign-in restoration still need isolated-account testing.

To validate without restoration:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\recovery-bundle\Restore.ps1 -VerifyOnly
```

If the backup is missing/corrupt or belongs to another user, exact restoration is refused. The emergency Task Manager command `explorer.exe` can request the normal desktop, but it does not reconstruct previous configuration. There is no automatic emergency registry rewrite.

## Transaction rules

- Captures use a versioned JSON envelope and SHA-256 corruption check. Files are flushed, read back and moved/replaced atomically on the same volume. The initial baseline is committed as a directory and cannot be overwritten by preparation.
- A journal records intended writes before mutation. Restore accepts either the recorded applied value or the original value; unexpected later edits cause a conflict. It only restores captured owned values, preserving the registry type and removing values originally absent.
- Production writes are allowlisted to `HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\System\Shell`. Machine values, wallpaper changes, startup entries and work-area mutations are unsupported and rejected. No production activation API currently writes that setting.
- A file lock serializes restorations. Each value is checked again before writing and verified afterward. Concurrent changes can still cause partial restoration; the journal remains rerunnable and the failure is reported.
- Current-user SID is required; elevated/wrong-account invocation is refused. Same user's later sign-in can restore registry state; active process records must match the captured session.
- Process stopping requires PID, session, executable path, creation time, SHA-256 and a confirmed owner SID. It accepts only identified MinimaDock/guard binaries. There are no process registrations in the prepared baseline.
- SHA-256 envelopes detect corruption, not a malicious same-user editor. ACL hardening, executable integrity attestation, and adversarial journal tampering tests are still required before activation.
- Recorded display geometry is diagnostic. Recovery never reapplies stale rectangles.

## Independent guard

`MinimaDockGuard.exe` runs separately from the UI. It validates the colocated shell executable, SID/session, PID creation time and liveness HWND. A future trial coordinator supplies private inherited ready/keep/revert event handles and waits for readiness before changing anything. The guard waits on the shell process and events, probes the UI thread with bounded `SendMessageTimeout(WM_NULL)`, and invokes the standalone restore engine on crash, persistent hang, trial timeout or revert. Keep cancels only the trial deadline; crash/hang supervision remains active and no permanent startup is enabled. There is no crash-relaunch loop.

The guard and registry engine are implemented and tested separately. The activation coordinator, next-sign-in interrupted-transaction detection and actual policy restoration are not implemented. **The replacement gate is not passed.** Do not manually change the live shell value or stop Explorer to try this build.

## Validation achieved

Isolated registry tests cover String, ExpandString, signed/raw DWORD, QWORD, Binary, None and MultiString values; absent-value removal; preservation of unrelated values; repeated restore; missing/corrupt backups; original-baseline protection; wrong-user rejection; later edits; write-ahead interruption; and production/test path isolation.

Native guard scenarios cover termination, UI hang, timeout, explicit revert, keep/revert and PID creation-time rejection. They launch an invisible fixture and a harmless test script. They do not prove Explorer-free recovery. An isolated VM/disposable account must still validate startup failure, normal/hung/absent shell recovery, real shell policy precedence, Task Manager access, Explorer folder behavior, work areas, Wallpaper Engine, keyboard behavior and the next sign-in.
