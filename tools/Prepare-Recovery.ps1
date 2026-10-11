param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'recovery\Recovery.psm1') -Force
New-MinimaCapture -Destination $Destination | ConvertTo-Json -Depth 16
Write-Output ('Recovery entry point: "' + [IO.Path]::Combine([IO.Path]::GetFullPath($Destination), 'Restore-Windows.cmd') + '"')
