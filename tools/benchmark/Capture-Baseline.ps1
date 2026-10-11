param(
    [ValidateRange(10,600)][int]$Seconds=30,
    [ValidateSet('stock','custom')][string]$Mode='stock',
    [string]$Output=(Join-Path $PSScriptRoot '..\..\captures'),
    [string]$Scenario='development-session-pilot'
)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '..\recovery\Recovery.psm1') -Force
Initialize-MinimaNative
if ($Mode -eq 'stock' -and -not (Test-MinimaExplorerDesktop)) { throw 'Stock Explorer desktop could not be verified. Refusing to label the capture as stock.' }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MinimaBenchmark {
    [StructLayout(LayoutKind.Sequential)] public struct Performance {
        public uint Size; public UIntPtr CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache, KernelTotal, KernelPaged, KernelNonpaged, PageSize;
        public uint Handles, Processes, Threads;
    }
    [DllImport("psapi.dll")] public static extern bool GetPerformanceInfo(ref Performance p, uint size);
    public static Performance Read() { var p=new Performance(); p.Size=(uint)Marshal.SizeOf(p); if(!GetPerformanceInfo(ref p,p.Size)) throw new System.ComponentModel.Win32Exception(); return p; }
}
'@
$identity=Get-MinimaIdentity
$names=@('explorer','StartMenuExperienceHost','ShellExperienceHost','SearchHost','dwm','wallpaper32','wallpaper64','MinimaDock','MinimaDockGuard','Spotify')
$started=[DateTime]::UtcNow
$samples=New-Object 'System.Collections.Generic.List[object]'
$clock=[Diagnostics.Stopwatch]::StartNew()
for($i=0;$i -le $Seconds;$i++) {
    $processes=@(Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object SessionId -eq $identity.Session | ForEach-Object {
        $cpu=$null; $path=$null; $ticks=$null
        try { $cpu=$_.TotalProcessorTime.TotalSeconds; $path=$_.Path; $ticks=$_.StartTime.ToUniversalTime().Ticks.ToString() } catch {}
        [ordered]@{Name=$_.Name; Id=$_.Id; StartTicks=$ticks; Path=$path; CpuSeconds=$cpu; PrivateCommitBytes=$_.PrivateMemorySize64; WorkingSetBytes=$_.WorkingSet64}
    })
    $memory=[MinimaBenchmark]::Read(); $page=$memory.PageSize.ToUInt64()
    $samples.Add([ordered]@{ElapsedSeconds=$clock.Elapsed.TotalSeconds; Utc=[DateTime]::UtcNow.ToString('o'); SystemCommitBytes=($memory.CommitTotal.ToUInt64()*$page); PhysicalAvailableBytes=($memory.PhysicalAvailable.ToUInt64()*$page); Processes=$processes})
    if($i -lt $Seconds) { $remaining=($i+1)*1000-$clock.ElapsedMilliseconds; if($remaining -gt 0) { Start-Sleep -Milliseconds $remaining } }
}
$cpuReport=@()
$first=$samples[0]; $last=$samples[$samples.Count-1]
foreach($p in $last.Processes) {
    $prior=@($first.Processes | Where-Object { $_.Id -eq $p.Id -and $_.StartTicks -ceq $p.StartTicks })
    if($prior.Count -eq 1 -and $null -ne $p.CpuSeconds -and $null -ne $prior[0].CpuSeconds) {
        $cpuReport += [pscustomobject][ordered]@{Name=$p.Name; Id=$p.Id; CpuSeconds=($p.CpuSeconds-$prior[0].CpuSeconds); IntervalSeconds=($last.ElapsedSeconds-$first.ElapsedSeconds); OneCorePercent=(100*($p.CpuSeconds-$prior[0].CpuSeconds)/($last.ElapsedSeconds-$first.ElapsedSeconds))}
    }
}
$hardware=@{}; $limitations=@('Pilot only: no matched A/B trials, GPU/ETW counters, wakeup traces, latency, or leak coverage. Working sets are reported per process and never added as unique memory. Unreadable CPU counters are null, not zero. Wallpaper configuration and power mode need confirmation for repeatability.')
try {
    $os=Get-CimInstance Win32_OperatingSystem; $cpu=Get-CimInstance Win32_Processor; $gpu=Get-CimInstance Win32_VideoController
    $hardware=@{OS=$os.Caption; Version=$os.Version; CPU=@($cpu.Name); GPU=@($gpu.Name); RamBytes=($os.TotalVisibleMemorySize*1024)}
} catch { $limitations += 'CIM hardware inventory unavailable in this execution context.' }
$report=[ordered]@{Schema=1; Mode=$Mode; Scenario=$Scenario; StartedUtc=$started.ToString('o'); Identity=$identity; Hardware=$hardware; Displays=@([MinimaRecovery.Native]::Monitors()); Samples=$samples.ToArray(); CpuIntervals=$cpuReport; Limitations=$limitations}
[void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($Output))
$file=Join-Path $Output ($Mode+'-'+$started.ToString('yyyyMMddTHHmmssZ')+'.json')
Write-MinimaAtomic $file $report -NewOnly
Write-Output $file
$cpuReport | Format-Table Name,Id,CpuSeconds,IntervalSeconds,OneCorePercent
