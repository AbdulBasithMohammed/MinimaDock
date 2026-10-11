$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\..\tools\recovery\Recovery.psm1') -Force
$script:Checks=0
function Assert($Condition, [string]$Message) { $script:Checks++; if (-not $Condition) { throw $Message } }
function Throws([scriptblock]$Action, [string]$Message) { $threw=$false; try { & $Action | Out-Null } catch { $threw=$true }; Assert $threw $Message }
$testId=[guid]::NewGuid().ToString('N')
$root='Software\MinimaDock.RecoveryTests\' + $testId
$temp=Join-Path ([IO.Path]::GetTempPath()) ('MinimaDock tests 雪 ' + $testId)
[void][IO.Directory]::CreateDirectory($temp)
try {
    $cases=@(
        @{Kind='String'; Data='Original 雪 shell with spaces'},
        @{Kind='ExpandString'; Data='%WINDIR%\explorer.exe'},
        @{Kind='DWord'; Data='-1'},
        @{Kind='QWord'; Data='9223372036854775807'},
        @{Kind='Binary'; Data='AAECA/8='},
        @{Kind='None'; Data='AAECA/8='},
        @{Kind='MultiString'; Data=@('one','two 雪')},
        @{Kind='MultiString'; Data=@('single')}
    )
    for ($i=0; $i -lt $cases.Count; $i++) {
        $path=$root+'\type'+$i; $bundle=Join-Path $temp ('type '+$i)
        $original=[pscustomobject]@{Hive='CurrentUser'; Path=$path; Name='Shell'; KeyExisted=$false; Exists=$true; Kind=$cases[$i].Kind; Data=$cases[$i].Data}
        Set-MinimaRegistryValue $original -TestOnly
        $captured=New-MinimaCapture $bundle @($original) -TestOnly
        $applied=[pscustomobject]@{Hive='CurrentUser'; Path=$path; Name='Shell'; KeyExisted=$true; Exists=$true; Kind='String'; Data='MinimaDock trial'}
        Invoke-MinimaTestMutation $bundle $applied
        $result=Restore-MinimaTransaction $bundle -TestOnly
        Assert ($result.State -ceq 'Restored') 'Restoration state'
        Assert (Test-MinimaValueEqual (Get-MinimaRegistryValue 'CurrentUser' $path 'Shell') $captured.Values[0]) ('Exact type/data restoration: '+$cases[$i].Kind)
        $again=Restore-MinimaTransaction $bundle -TestOnly
        Assert (Test-MinimaValueEqual (Get-MinimaRegistryValue 'CurrentUser' $path 'Shell') $captured.Values[0]) 'Idempotent second restore'
        Throws { New-MinimaCapture $bundle @($original) -TestOnly } 'Baseline overwrite rejected'
    }
    $path=$root+'\absent'; $bundle=Join-Path $temp 'absent'
    $applied=[pscustomobject]@{Hive='CurrentUser'; Path=$path; Name='Shell'; KeyExisted=$false; Exists=$true; Kind='String'; Data='trial'}
    New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    Invoke-MinimaTestMutation $bundle $applied
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path,$true)
    try { $key.SetValue('Unrelated','preserve me') } finally { $key.Dispose() }
    Restore-MinimaTransaction $bundle -TestOnly | Out-Null
    Assert (-not (Get-MinimaRegistryValue 'CurrentUser' $path 'Shell').Exists) 'Originally absent value removed'
    Assert ((Get-MinimaRegistryValue 'CurrentUser' $path 'Unrelated').Data -ceq 'preserve me') 'Unrelated value preserved'

    $path=$root+'\conflict'; $bundle=Join-Path $temp 'conflict'
    $applied=[pscustomobject]@{Hive='CurrentUser'; Path=$path; Name='Shell'; KeyExisted=$false; Exists=$true; Kind='String'; Data='trial'}
    New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    Invoke-MinimaTestMutation $bundle $applied
    $later=$applied.PSObject.Copy(); $later.Data='new user edit'
    Set-MinimaRegistryValue $later -TestOnly
    Throws { Restore-MinimaTransaction $bundle -TestOnly } 'Later edit conflict rejected'
    Assert ((Get-MinimaRegistryValue 'CurrentUser' $path 'Shell').Data -ceq 'new user edit') 'Later edit survives recovery'

    $bundle=Join-Path $temp 'corrupt'; New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    [IO.File]::WriteAllText((Join-Path $bundle 'baseline.json'), '{"Format":"MinimaDock/1","Sha256":"bad","Payload":"{}"}')
    Throws { Restore-MinimaTransaction $bundle -TestOnly } 'Corrupt backup rejected'
    Throws { Restore-MinimaTransaction (Join-Path $temp 'missing') -TestOnly } 'Missing backup rejected'

    $bundle=Join-Path $temp 'wrong-user'; New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    $manifest=Read-MinimaEnvelope (Join-Path $bundle 'baseline.json'); $manifest.Identity.Sid='S-1-5-21-wrong'
    Write-MinimaAtomic (Join-Path $bundle 'baseline.json') $manifest
    Throws { Restore-MinimaTransaction $bundle -TestOnly } 'Wrong user rejected'

    $bundle=Join-Path $temp 'interrupted'; $path=$root+'\interrupted'
    $applied=[pscustomobject]@{Hive='CurrentUser'; Path=$path; Name='Shell'; KeyExisted=$false; Exists=$true; Kind='String'; Data='trial'}
    New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    Invoke-MinimaTestMutation $bundle $applied
    $journal=Read-MinimaEnvelope (Join-Path $bundle 'journal.json'); $journal.State='Applying'; Write-MinimaAtomic (Join-Path $bundle 'journal.json') $journal
    Restore-MinimaTransaction $bundle -TestOnly | Out-Null
    Assert (-not (Get-MinimaRegistryValue 'CurrentUser' $path 'Shell').Exists) 'Interrupted activation restores'

    $bundle=Join-Path $temp 'planned-before-write'; New-MinimaCapture $bundle @($applied) -TestOnly | Out-Null
    $journal=Read-MinimaEnvelope (Join-Path $bundle 'journal.json'); $journal.Changes=@([pscustomobject]@{Applied=$applied}); $journal.State='Applying'
    Write-MinimaAtomic (Join-Path $bundle 'journal.json') $journal
    Restore-MinimaTransaction $bundle -TestOnly | Out-Null
    Assert (-not (Get-MinimaRegistryValue 'CurrentUser' $path 'Shell').Exists) 'Write-ahead journal without mutation restores safely'

    Throws { Set-MinimaRegistryValue ([pscustomobject]@{Hive='LocalMachine'; Path=$path; Name='Shell'}) -TestOnly } 'Machine writes rejected'
    Throws { Set-MinimaRegistryValue ([pscustomobject]@{Hive='CurrentUser'; Path='Software\Microsoft\Windows\CurrentVersion\Policies\System'; Name='Shell'}) -TestOnly } 'Test adapter cannot mutate live shell'
    Throws { Restore-MinimaTransaction $bundle } 'Test bundle cannot use production restore'
    Write-Output "$script:Checks recovery checks passed. Only isolated HKCU test keys were modified."
} finally {
    # Both deletion targets are explicit isolated test roots, checked before recursive cleanup.
    if ($root -cne ('Software\MinimaDock.RecoveryTests\'+$testId)) { throw 'Unsafe registry cleanup path.' }
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($root,$false)
    $resolved=[IO.Path]::GetFullPath($temp)
    if ([IO.Path]::GetDirectoryName($resolved) -cne [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') -or -not [IO.Path]::GetFileName($resolved).EndsWith($testId)) { throw 'Unsafe temporary cleanup path.' }
    if ([IO.Directory]::Exists($resolved)) { [IO.Directory]::Delete($resolved,$true) }
}
