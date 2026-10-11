param([string]$Bundle = $PSScriptRoot, [switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
try {
    Import-Module (Join-Path $PSScriptRoot 'Recovery.psm1') -Force
    Restore-MinimaTransaction -Bundle $Bundle -VerifyOnly:$VerifyOnly | Format-List
    exit 0
} catch {
    [Console]::Error.WriteLine('Restoration failed: ' + $_.Exception.Message)
    [Console]::Error.WriteLine('Your apps have not been force-logged-off. Preserve this bundle and resolve the reported conflict. If Explorer is absent, Task Manager > Run new task > explorer.exe is an emergency desktop launch, not exact configuration recovery.')
    exit 1
}
