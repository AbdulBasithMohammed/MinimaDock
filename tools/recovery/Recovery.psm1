Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:ShellPath = 'Software\Microsoft\Windows\CurrentVersion\Policies\System'
$script:TestPrefix = 'Software\MinimaDock.RecoveryTests\'

function Get-MinimaIdentity {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    [pscustomobject]@{
        Sid = $identity.User.Value
        Session = [Diagnostics.Process]::GetCurrentProcess().SessionId
        Elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
}

function Get-MinimaDigest([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Write-MinimaAtomic([string]$Path, $Value, [switch]$NewOnly) {
    $payload = ConvertTo-Json -InputObject $Value -Depth 32 -Compress
    $envelope = ConvertTo-Json -InputObject ([ordered]@{ Format = 'MinimaDock/1'; Sha256 = Get-MinimaDigest $payload; Payload = $payload }) -Compress
    $full = [IO.Path]::GetFullPath($Path)
    $temp = $full + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($envelope)
        $stream = New-Object IO.FileStream($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        if ([IO.File]::ReadAllText($temp) -cne $envelope) { throw 'Atomic write readback failed.' }
        if ($NewOnly -or -not [IO.File]::Exists($full)) { [IO.File]::Move($temp, $full) }
        else {
            # Windows PowerShell 5.1 can coerce a null backup path into an invalid empty string.
            $previous = $temp + '.previous'
            try { [IO.File]::Replace($temp, $full, $previous) }
            finally { if ([IO.File]::Exists($previous)) { [IO.File]::Delete($previous) } }
        }
    } finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
}

function Read-MinimaEnvelope([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing recovery file: $Path. Exact restoration is unavailable." }
    $info = New-Object IO.FileInfo($Path)
    if ($info.Length -gt 4MB) { throw 'Recovery file exceeds size limit.' }
    $envelope = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if ($envelope.Format -cne 'MinimaDock/1' -or $envelope.Sha256 -cne (Get-MinimaDigest $envelope.Payload)) { throw 'Recovery file format/integrity check failed.' }
    $envelope.Payload | ConvertFrom-Json
}

function Assert-MinimaTarget($Target, [switch]$TestOnly) {
    if ($Target.Hive -cne 'CurrentUser') { throw 'Recovery writes are restricted to the captured HKCU scope.' }
    if ($TestOnly) {
        if (-not $Target.Path.StartsWith($script:TestPrefix, [StringComparison]::Ordinal) -or $Target.Path -match '\.\.' -or $Target.Path.Length -gt 240) { throw 'Test adapter requires a dedicated MinimaDock.RecoveryTests key.' }
    } elseif ($Target.Path -cne $script:ShellPath -or $Target.Name -cne 'Shell') { throw 'Unowned registry target rejected.' }
}

function Get-MinimaRegistryValue([string]$Hive, [string]$Path, [string]$Name) {
    $root = if ($Hive -ceq 'CurrentUser') { [Microsoft.Win32.Registry]::CurrentUser } elseif ($Hive -ceq 'LocalMachine') { [Microsoft.Win32.Registry]::LocalMachine } else { throw 'Unknown registry hive.' }
    $key = $root.OpenSubKey($Path, $false)
    try {
        $exists = $null -ne $key -and $key.GetValueNames() -contains $Name
        $kind = $null; $data = $null
        if ($exists) {
            $kind = $key.GetValueKind($Name).ToString()
            $raw = $key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            switch ($kind) {
                'Binary' { $data = [Convert]::ToBase64String($raw) }
                'None' { $data = [Convert]::ToBase64String($raw) }
                'DWord' { $data = ([int]$raw).ToString([Globalization.CultureInfo]::InvariantCulture) }
                'QWord' { $data = ([long]$raw).ToString([Globalization.CultureInfo]::InvariantCulture) }
                'MultiString' { $data = @($raw) }
                default { $data = [string]$raw }
            }
        }
        [pscustomobject][ordered]@{ Hive = $Hive; Path = $Path; Name = $Name; KeyExisted = ($null -ne $key); Exists = $exists; Kind = $kind; Data = $data }
    } finally { if ($null -ne $key) { $key.Dispose() } }
}

function Test-MinimaValueEqual($Left, $Right) {
    if ([bool]$Left.Exists -ne [bool]$Right.Exists) { return $false }
    if (-not $Left.Exists) { return $true }
    if ($Left.Kind -cne $Right.Kind) { return $false }
    (ConvertTo-Json -InputObject $Left.Data -Compress) -ceq (ConvertTo-Json -InputObject $Right.Data -Compress)
}

function Set-MinimaRegistryValue($Value, [switch]$TestOnly) {
    Assert-MinimaTarget $Value -TestOnly:$TestOnly
    if (-not $Value.Exists) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Value.Path, $true)
        if ($null -ne $key) {
            try { $key.DeleteValue($Value.Name, $false); $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 } finally { $key.Dispose() }
            if (-not $Value.KeyExisted -and $empty) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($Value.Path, $false) }
        }
        return
    }
    $kind = [Microsoft.Win32.RegistryValueKind]::$($Value.Kind)
    $data = switch ($Value.Kind) {
        'Binary' { ,([Convert]::FromBase64String($Value.Data)) }
        'None' { ,([Convert]::FromBase64String($Value.Data)) }
        'DWord' { [int]::Parse($Value.Data, [Globalization.CultureInfo]::InvariantCulture) }
        'QWord' { [long]::Parse($Value.Data, [Globalization.CultureInfo]::InvariantCulture) }
        'MultiString' { ,([string[]]$Value.Data) }
        'String' { [string]$Value.Data }
        'ExpandString' { [string]$Value.Data }
        default { throw 'Unsupported registry value type.' }
    }
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Value.Path)
    try { $key.SetValue($Value.Name, $data, $kind) } finally { $key.Dispose() }
}

function Initialize-MinimaNative {
    if ('MinimaRecovery.Native' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MinimaRecovery {
  public static class Native {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("kernel32.dll")] public static extern bool GlobalMemoryStatusEx(ref Memory m);
    [StructLayout(LayoutKind.Sequential)] public struct Memory { public uint Length, Load; public ulong Total, Available, TotalPage, AvailablePage, TotalVirtual, AvailableVirtual, Extended; }
    public static Memory MemoryStatus() { var m = new Memory(); m.Length = (uint)Marshal.SizeOf(m); if (!GlobalMemoryStatusEx(ref m)) throw new System.ComponentModel.Win32Exception(); return m; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct MonitorInfo { public uint Size; public Rect Monitor, Work; public uint Flags; [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)] public string Device; }
    public delegate bool MonitorCallback(IntPtr h, IntPtr dc, ref Rect r, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorCallback cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr h, ref MonitorInfo info);
    public static MonitorInfo[] Monitors() { var list = new System.Collections.Generic.List<MonitorInfo>(); MonitorCallback cb = delegate(IntPtr h, IntPtr dc, ref Rect r, IntPtr p) { var i = new MonitorInfo(); i.Size=(uint)Marshal.SizeOf(i); if(GetMonitorInfo(h,ref i)) list.Add(i); return true; }; if(!EnumDisplayMonitors(IntPtr.Zero,IntPtr.Zero,cb,IntPtr.Zero)) throw new System.ComponentModel.Win32Exception(); return list.ToArray(); }
  }
}
'@
}

function Test-MinimaExplorerDesktop {
    Initialize-MinimaNative
    $identity = Get-MinimaIdentity
    $expected = [IO.Path]::Combine($env:WINDIR, 'explorer.exe')
    foreach ($hwnd in @([MinimaRecovery.Native]::FindWindow('Shell_TrayWnd', $null), [MinimaRecovery.Native]::GetShellWindow())) {
        if ($hwnd -eq [IntPtr]::Zero) { return $false }
        [uint32]$processId = 0
        [void][MinimaRecovery.Native]::GetWindowThreadProcessId($hwnd, [ref]$processId)
        try { $p = Get-Process -Id $processId; if ($p.SessionId -ne $identity.Session -or -not [string]::Equals($p.Path, $expected, [StringComparison]::OrdinalIgnoreCase)) { return $false } }
        catch { return $false }
    }
    return $true
}

function New-MinimaCapture([string]$Destination, [object[]]$Targets, [switch]$TestOnly) {
    $identity = Get-MinimaIdentity
    if ($identity.Elevated) { throw 'Run capture as the normal interactive user, without elevation.' }
    $full = [IO.Path]::GetFullPath($Destination)
    if ([IO.Directory]::Exists($full) -or [IO.File]::Exists($full)) { throw 'Original recovery bundle already exists. It will never be overwritten.' }
    if (-not $Targets) { $Targets = @([pscustomobject]@{ Hive='CurrentUser'; Path=$script:ShellPath; Name='Shell' }) }
    foreach ($target in $Targets) { Assert-MinimaTarget $target -TestOnly:$TestOnly }
    $observed = @()
    if (-not $TestOnly) {
        Initialize-MinimaNative
        foreach ($hive in @('CurrentUser', 'LocalMachine')) {
            foreach ($path in @($script:ShellPath, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon')) {
                $observed += Get-MinimaRegistryValue $hive $path 'Shell'
            }
        }
        # No policy writes are provided. Values here are evidence, not a reliable RSOP determination.
    }
    $values = @($Targets | ForEach-Object { Get-MinimaRegistryValue $_.Hive $_.Path $_.Name })
    $osKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion')
    try { $os = [ordered]@{ Build=$osKey.GetValue('CurrentBuild'); Revision=$osKey.GetValue('UBR'); Edition=$osKey.GetValue('EditionID'); DisplayVersion=$osKey.GetValue('DisplayVersion'); Architecture=$env:PROCESSOR_ARCHITECTURE } } finally { $osKey.Dispose() }
    $manifest = [ordered]@{
        Version = 1; TransactionId = [guid]::NewGuid().ToString('N'); CapturedUtc = [DateTime]::UtcNow.ToString('o')
        Identity = $identity; TestOnly = [bool]$TestOnly; OS = $os; Values = $values; ObservedShell = $observed
        Displays = $(if ($TestOnly) { @() } else { @([MinimaRecovery.Native]::Monitors()) })
        ExplorerDesktopPresent = $(if ($TestOnly) { $false } else { Test-MinimaExplorerDesktop })
        PolicyAssessment = 'Not evaluated with RSOP/MDM. Activation is disabled pending policy and recovery gates.'
        StartupChanges = @(); WallpaperChanges = @(); WorkAreaChanges = @()
    }
    $parent = [IO.Path]::GetDirectoryName($full)
    [void][IO.Directory]::CreateDirectory($parent)
    $staging = Join-Path $parent ('.capture-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($staging)
    try {
        Write-MinimaAtomic (Join-Path $staging 'baseline.json') $manifest -NewOnly
        $journal = [ordered]@{ Version=1; TransactionId=$manifest.TransactionId; State='Captured'; RestartAllowed=$false; Changes=@(); Processes=@() }
        Write-MinimaAtomic (Join-Path $staging 'journal.json') $journal -NewOnly
        foreach ($name in @('Recovery.psm1', 'Restore.ps1', 'Restore-Windows.cmd')) {
            if (-not $TestOnly) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $staging $name) }
        }
        $check = Read-MinimaEnvelope (Join-Path $staging 'baseline.json')
        if ($check.TransactionId -cne $manifest.TransactionId) { throw 'Capture readback mismatch.' }
        [IO.Directory]::Move($staging, $full)
    } finally { if ([IO.Directory]::Exists($staging)) { [IO.Directory]::Delete($staging, $true) } }
    $manifest
}

function Open-MinimaTransaction([string]$Bundle, [switch]$TestOnly) {
    $manifest = Read-MinimaEnvelope (Join-Path $Bundle 'baseline.json')
    $journal = Read-MinimaEnvelope (Join-Path $Bundle 'journal.json')
    $identity = Get-MinimaIdentity
    if ($manifest.Version -ne 1 -or $journal.Version -ne 1 -or $manifest.TransactionId -cne $journal.TransactionId) { throw 'Recovery transaction mismatch.' }
    if ($identity.Elevated -or $manifest.Identity.Sid -cne $identity.Sid) { throw 'Wrong user or elevated invocation. Use the original normal user account.' }
    if ([bool]$manifest.TestOnly -ne [bool]$TestOnly) { throw 'Production/test recovery adapter mismatch.' }
    if (@($manifest.StartupChanges).Count -or @($manifest.WallpaperChanges).Count -or @($manifest.WorkAreaChanges).Count) { throw 'Unsupported mutation in this version; refusing incomplete restoration.' }
    foreach ($value in $manifest.Values) { Assert-MinimaTarget $value -TestOnly:$TestOnly }
    if (@($journal.Processes).Count -and $identity.Session -ne $manifest.Identity.Session) { throw 'Active transaction belongs to another session. Process restoration refused.' }
    foreach ($change in $journal.Changes) {
        Assert-MinimaTarget $change.Applied -TestOnly:$TestOnly
        $matches = @($manifest.Values | Where-Object { $_.Hive -ceq $change.Applied.Hive -and $_.Path -ceq $change.Applied.Path -and $_.Name -ceq $change.Applied.Name })
        if ($matches.Count -ne 1) { throw 'Journal contains an uncaptured target.' }
    }
    [pscustomobject]@{ Manifest=$manifest; Journal=$journal }
}

function Invoke-MinimaTestMutation([string]$Bundle, $Applied) {
    # Deliberately no equivalent production activation function exists in this milestone.
    $tx = Open-MinimaTransaction $Bundle -TestOnly
    Assert-MinimaTarget $Applied -TestOnly
    $original = @($tx.Manifest.Values | Where-Object { $_.Path -ceq $Applied.Path -and $_.Name -ceq $Applied.Name })
    if ($original.Count -ne 1) { throw 'Test mutation was not captured.' }
    if (-not (Test-MinimaValueEqual (Get-MinimaRegistryValue $Applied.Hive $Applied.Path $Applied.Name) $original[0])) { throw 'Registry changed since capture.' }
    $tx.Journal.Changes = @($tx.Journal.Changes) + @([pscustomobject]@{ Applied=$Applied })
    $tx.Journal.State = 'Applying'
    Write-MinimaAtomic (Join-Path $Bundle 'journal.json') $tx.Journal
    Set-MinimaRegistryValue $Applied -TestOnly
    $tx.Journal.State = 'Trial'
    Write-MinimaAtomic (Join-Path $Bundle 'journal.json') $tx.Journal
}

function Stop-MinimaOwnedProcess($Record, $Identity) {
    # PID alone or a process name is never sufficient. Verify all recorded identity fields.
    $p = Get-Process -Id $Record.Id -ErrorAction SilentlyContinue
    if ($null -eq $p) { return }
    if ($p.SessionId -ne $Identity.Session -or $Record.Session -ne $Identity.Session) { throw 'Owned process session mismatch.' }
    if (-not [string]::Equals($p.Path, $Record.Path, [StringComparison]::OrdinalIgnoreCase) -or $p.StartTime.ToUniversalTime().Ticks.ToString() -cne $Record.StartTicks) { throw 'Owned process identity changed (possible PID reuse).' }
    if ([IO.Path]::GetFileName($Record.Path) -cnotin @('MinimaDock.exe', 'MinimaDockGuard.exe')) { throw 'Unexpected process in recovery journal.' }
    if ((Get-FileHash -LiteralPath $Record.Path -Algorithm SHA256).Hash -cne $Record.Sha256) { throw 'Owned executable hash changed.' }
    $owner = Get-CimInstance Win32_Process -Filter "ProcessId=$($Record.Id)" | Invoke-CimMethod -MethodName GetOwnerSid
    if ($owner.ReturnValue -ne 0 -or $owner.Sid -cne $Identity.Sid) { throw 'Cannot authenticate process owner.' }
    if ($p.Id -eq [Diagnostics.Process]::GetCurrentProcess().Id) { throw 'Recovery cannot terminate itself.' }
    $p.Kill()
    if (-not $p.WaitForExit(5000)) { throw 'Owned process did not stop within timeout.' }
}

function Restore-MinimaTransaction([string]$Bundle, [switch]$TestOnly, [switch]$VerifyOnly) {
    $full = [IO.Path]::GetFullPath($Bundle)
    $lock = New-Object IO.FileStream((Join-Path $full 'restore.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $tx = Open-MinimaTransaction $full -TestOnly:$TestOnly
        $plan = @()
        foreach ($change in $tx.Journal.Changes) {
            $a = $change.Applied
            $original = @($tx.Manifest.Values | Where-Object { $_.Hive -ceq $a.Hive -and $_.Path -ceq $a.Path -and $_.Name -ceq $a.Name })[0]
            $current = Get-MinimaRegistryValue $a.Hive $a.Path $a.Name
            if (Test-MinimaValueEqual $current $original) { continue }
            if (-not (Test-MinimaValueEqual $current $a)) { throw "Later registry edit detected: $($a.Path)\$($a.Name). No settings were overwritten." }
            $plan += $original
        }
        if ($VerifyOnly) { return [pscustomobject]@{ Valid=$true; State=$tx.Journal.State; PendingChanges=$plan.Count; ActivationEnabled=$false } }
        $tx.Journal.RestartAllowed = $false
        $tx.Journal.State = 'Recovering'
        Write-MinimaAtomic (Join-Path $full 'journal.json') $tx.Journal
        foreach ($value in $plan) {
            # Recheck immediately before writing; registry operations are not a global transaction.
            $applied = @($tx.Journal.Changes | Where-Object { $_.Applied.Path -ceq $value.Path -and $_.Applied.Name -ceq $value.Name })[0].Applied
            $current = Get-MinimaRegistryValue $value.Hive $value.Path $value.Name
            if (-not (Test-MinimaValueEqual $current $applied) -and -not (Test-MinimaValueEqual $current $value)) { throw 'Concurrent edit detected during restoration.' }
            Set-MinimaRegistryValue $value -TestOnly:$TestOnly
            if (-not (Test-MinimaValueEqual (Get-MinimaRegistryValue $value.Hive $value.Path $value.Name) $value)) { throw 'Registry restoration readback failed.' }
        }
        $identity = Get-MinimaIdentity
        foreach ($record in $tx.Journal.Processes) { Stop-MinimaOwnedProcess $record $identity }
        $desktop = $false
        if (-not $TestOnly) {
            $desktop = Test-MinimaExplorerDesktop
            if (-not $desktop -and $tx.Manifest.ExplorerDesktopPresent) {
                Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe')
                for ($i=0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 250; if (Test-MinimaExplorerDesktop) { $desktop=$true; break } }
            }
        }
        $tx.Journal.State = if ($TestOnly -or $desktop) { 'Restored' } else { 'RestoredSignOutRequired' }
        Write-MinimaAtomic (Join-Path $full 'journal.json') $tx.Journal
        [pscustomobject]@{ State=$tx.Journal.State; SettingsRestored=$true; ExplorerDesktopVerified=$desktop; NextSignIn='Captured changed values restored; policy precedence requires isolated validation.' }
    } finally { $lock.Dispose() }
}

Export-ModuleMember -Function Get-MinimaIdentity,Get-MinimaDigest,Get-MinimaRegistryValue,Test-MinimaValueEqual,Set-MinimaRegistryValue,Write-MinimaAtomic,Read-MinimaEnvelope,New-MinimaCapture,Open-MinimaTransaction,Invoke-MinimaTestMutation,Restore-MinimaTransaction,Initialize-MinimaNative,Test-MinimaExplorerDesktop
