<#
.SYNOPSIS
Measures CPU, memory and GPU use of the Windows taskbar processes and MinimaDock
over the same sampling window. Read-only: it starts, stops and changes nothing.

.DESCRIPTION
Explorer also hosts the desktop and File Explorer windows, so its numbers are an
upper bound for "the taskbar". Start, Search and the shell experience hosts are
the Windows 11 taskbar's flyout processes. During a MinimaDock trial Explorer
keeps running with its taskbar hidden, so the dock's numbers add to it.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools\Measure-Resources.ps1 -Seconds 60 -Label idle
#>
param(
    [int]$Seconds = 60,
    [string]$Label = 'idle',
    [string]$Output = ''
)
$ErrorActionPreference = 'Stop'
$groups = [ordered]@{
    'Windows taskbar' = @('explorer','StartMenuExperienceHost','SearchHost','ShellExperienceHost','ShellHost','Widgets','WidgetService')
    'MinimaDock'      = @('MinimaDock','MinimaDockTrial','MinimaDockGuard')
}
$names = $groups.Values | ForEach-Object { $_ }
function Snapshot {
    $result = @{}
    foreach ($p in Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName }) {
        try { $result[$p.Id] = [pscustomobject]@{ Id=$p.Id; Name=$p.ProcessName; Cpu=$p.TotalProcessorTime.TotalSeconds; WorkingSet=$p.WorkingSet64; Private=$p.PrivateMemorySize64; Threads=$p.Threads.Count; Handles=$p.HandleCount } } catch {}
    }
    $result
}
$cores = [Environment]::ProcessorCount
$before = Snapshot
$clock = [Diagnostics.Stopwatch]::StartNew()
# GPU engine utilisation per process (pid_N in the instance name), sampled each second.
$gpu = @{}
try {
    $samples = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $Seconds -ErrorAction Stop
    foreach ($sample in $samples) {
        $perPid = @{}
        foreach ($c in $sample.CounterSamples) {
            if ($c.InstanceName -match 'pid_(\d+)_') { $id = [int]$Matches[1]; $perPid[$id] = ($perPid[$id] + $c.CookedValue) }
        }
        foreach ($id in $perPid.Keys) { $gpu[$id] = @($gpu[$id]) + $perPid[$id] }
    }
} catch { Start-Sleep -Seconds $Seconds; $gpu = $null }
$elapsed = $clock.Elapsed.TotalSeconds
$after = Snapshot
$gpuMemory = @{}
try {
    foreach ($c in (Get-Counter -Counter '\GPU Process Memory(*)\Dedicated Usage' -ErrorAction Stop).CounterSamples) {
        if ($c.InstanceName -match 'pid_(\d+)_') { $id=[int]$Matches[1]; $gpuMemory[$id] = ($gpuMemory[$id] + $c.CookedValue) }
    }
} catch {}
$rows = foreach ($id in $after.Keys) {
    $a = $after[$id]; $b = $before[$id]
    if (-not $b) { continue }
    $cpu = ($a.Cpu - $b.Cpu) / $elapsed * 100
    $group = ($groups.GetEnumerator() | Where-Object { $_.Value -contains $a.Name } | Select-Object -First 1).Key
    $g = if ($gpu -and $gpu[$id]) { ($gpu[$id] | Measure-Object -Average -Maximum) } else { $null }
    [pscustomobject]@{
        Group = $group; Process = $a.Name; Id = $id
        'CPU % of one core' = [math]::Round($cpu, 2)
        'CPU % of PC' = [math]::Round($cpu / $cores, 3)
        'Working set MB' = [math]::Round($a.WorkingSet / 1MB, 1)
        'Private MB' = [math]::Round($a.Private / 1MB, 1)
        'GPU avg %' = if ($g) { [math]::Round($g.Average, 2) } else { 0 }
        'GPU peak %' = if ($g) { [math]::Round($g.Maximum, 2) } else { 0 }
        'GPU memory MB' = [math]::Round(($gpuMemory[$id]) / 1MB, 1)
        Threads = $a.Threads; Handles = $a.Handles
    }
}
$rows = $rows | Sort-Object Group, Process
$totals = foreach ($group in $groups.Keys) {
    $set = @($rows | Where-Object Group -eq $group)
    if (-not $set.Count) { continue }
    [pscustomobject]@{
        Group = $group; Processes = $set.Count
        'CPU % of one core' = [math]::Round(($set | Measure-Object 'CPU % of one core' -Sum).Sum, 2)
        'Working set MB' = [math]::Round(($set | Measure-Object 'Working set MB' -Sum).Sum, 1)
        'Private MB' = [math]::Round(($set | Measure-Object 'Private MB' -Sum).Sum, 1)
        'GPU avg %' = [math]::Round(($set | Measure-Object 'GPU avg %' -Sum).Sum, 2)
        'GPU memory MB' = [math]::Round(($set | Measure-Object 'GPU memory MB' -Sum).Sum, 1)
    }
}
"Label: $Label   Window: $([math]::Round($elapsed,1)) s   Logical CPUs: $cores"
$rows | Format-Table -AutoSize | Out-String -Width 250
'Totals'
$totals | Format-Table -AutoSize | Out-String -Width 250
if ($Output) {
    [pscustomobject]@{ Label=$Label; Seconds=$elapsed; Cores=$cores; Measured=(Get-Date).ToString('o'); Processes=$rows; Totals=$totals } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Output -Encoding UTF8
}
