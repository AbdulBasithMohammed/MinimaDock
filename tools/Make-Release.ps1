# Builds the download zip from the accepted binaries staged in out\:
#   MinimaDock-<version>\  Install MinimaDock.cmd, Uninstall MinimaDock.cmd, README.txt,
#                          LICENSE, THIRD_PARTY_NOTICES.md, third_party\licenses\,
#                          app\ (MinimaDock.exe, MinimaDockTrial.exe, installer scripts)
# Output: build\release\MinimaDock-<version>.zip and its SHA-256.
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $repo 'out'
foreach($name in 'MinimaDock.exe','MinimaDockTrial.exe'){if(-not (Test-Path -LiteralPath (Join-Path $out $name))){throw ('Stage an accepted build in out\ first: '+$name+' missing.')}}
$version=(Get-Item -LiteralPath (Join-Path $out 'MinimaDock.exe')).VersionInfo.ProductVersion -replace '\.0$',''
$name='MinimaDock-'+$version
$stage=Join-Path $repo ('build\release\'+$name)
$zip=Join-Path $repo ('build\release\'+$name+'.zip')
if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force}
New-Item -ItemType Directory -Path (Join-Path $stage 'app') -Force | Out-Null
foreach($file in 'MinimaDock.exe','MinimaDockTrial.exe'){Copy-Item -LiteralPath (Join-Path $out $file) -Destination (Join-Path $stage 'app')}
foreach($file in 'Install-MinimaDock.ps1','Dock-LaunchCheck.ps1'){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $stage 'app')}
foreach($file in 'LICENSE','THIRD_PARTY_NOTICES.md'){Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $stage}
New-Item -ItemType Directory -Path (Join-Path $stage 'third_party\licenses') -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repo 'third_party\licenses') -File | Copy-Item -Destination (Join-Path $stage 'third_party\licenses')
$readme=(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release\README.txt') -Raw).Replace('{VERSION}',$version)
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'),($readme -replace "`r?`n","`r`n"),(New-Object Text.UTF8Encoding($false)))
foreach($action in 'Install','Uninstall') {
    $cmd="@echo off`r`n""%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"" -NoProfile -ExecutionPolicy Bypass -File ""%~dp0app\Install-MinimaDock.ps1"" -Action $action`r`npause`r`n"
    [IO.File]::WriteAllText((Join-Path $stage ($action+' MinimaDock.cmd')),$cmd,(New-Object Text.ASCIIEncoding))
}
if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip -Force}
Compress-Archive -Path $stage -DestinationPath $zip
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Set-Content -LiteralPath ($zip+'.sha256') -Value ($hash+'  '+$name+'.zip') -Encoding ascii
[pscustomobject]@{Zip=$zip;Sha256=$hash;SizeKB=[int]((Get-Item -LiteralPath $zip).Length/1KB)}
