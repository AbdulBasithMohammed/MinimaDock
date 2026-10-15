param([string]$TestedTrial,[ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$TestedTrialSha256)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $repo 'out'
$trialSource=Join-Path $repo 'build\portable\MinimaDockTrial.exe'
if($TestedTrial) {
    if(-not $TestedTrialSha256 -or (Get-FileHash -LiteralPath $TestedTrial -Algorithm SHA256).Hash -ne $TestedTrialSha256){throw 'Previously tested supervisor hash does not match.'}
    function Get-NormalizedRecoveryHash([string]$Path) {
        $data=[IO.File]::ReadAllBytes($Path)
        if($data.Length -lt 512){throw 'Invalid recovery executable.'}
        $peAt=[BitConverter]::ToInt32($data,60)
        if($peAt -lt 64 -or $peAt+120 -gt $data.Length -or [BitConverter]::ToUInt32($data,$peAt) -ne 17744){throw 'Invalid recovery PE header.'}
        [Array]::Clear($data,$peAt+8,4) # Link timestamp.
        [Array]::Clear($data,$peAt+88,4) # PE checksum.
        $count=[BitConverter]::ToUInt16($data,$peAt+6)
        $table=$peAt+24+[BitConverter]::ToUInt16($data,$peAt+20)
        if($count -gt 96 -or $table+40*$count -gt $data.Length){throw 'Invalid recovery sections.'}
        $headers=[BitConverter]::ToUInt32($data,$peAt+84)
        for($i=0;$i -lt $count;$i++) {
            $at=$table+40*$i
            if([Text.Encoding]::ASCII.GetString($data,$at,8).Trim([char]0) -ne '.buildid'){continue}
            $size=[BitConverter]::ToUInt32($data,$at+16);$offset=[BitConverter]::ToUInt32($data,$at+20)
            if($size -gt 4096 -or $offset -lt $headers -or [uint64]$offset+$size -gt $data.Length -or ([BitConverter]::ToUInt32($data,$at+36) -band 0x20000000)){throw 'Invalid non-executable build metadata.'}
            [Array]::Clear($data,$offset,$size)
        }
        $sha=[Security.Cryptography.SHA256]::Create()
        try {[BitConverter]::ToString($sha.ComputeHash($data))}finally{$sha.Dispose()}
    }
    if((Get-NormalizedRecoveryHash $TestedTrial) -ne (Get-NormalizedRecoveryHash $trialSource)){throw 'Recovery implementation differs; previously tested supervisor cannot be retained.'}
    $trialSource=[IO.Path]::GetFullPath($TestedTrial)
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
foreach($name in @('MinimaDock.exe','MinimaDockGuard.exe','MinimaDockTrial.exe')) {
    $source=Join-Path $repo ('build\portable\'+$name)
    if($name -eq 'MinimaDockTrial.exe'){$source=$trialSource}
    if(-not (Test-Path -LiteralPath $source)){throw ('Build missing: '+$name)}
    Copy-Item -LiteralPath $source -Destination (Join-Path $out $name) -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination (Join-Path $out 'README.md') -Force
foreach($notice in @('LICENSE','THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $notice) -Destination (Join-Path $out $notice) -Force
}
$noticeOut=Join-Path $out 'third_party\licenses'
New-Item -ItemType Directory -Path $noticeOut -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repo 'third_party\licenses') -File | Copy-Item -Destination $noticeOut -Force
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'taskbar-launchers') -Filter '*.cmd' | Copy-Item -Destination $out -Force
$projectText=Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
$versionMatch=[regex]::Match($projectText,'project\(MinimaDock VERSION ([0-9.]+)')
if(-not $versionMatch.Success){throw 'Project version missing'}
$binaries=@(foreach($name in @('MinimaDock.exe','MinimaDockGuard.exe','MinimaDockTrial.exe')) {
    [ordered]@{Name=$name;Sha256=(Get-FileHash -LiteralPath (Join-Path $out $name) -Algorithm SHA256).Hash}
})
[ordered]@{
    Product='MinimaDock'
    Version=$versionMatch.Groups[1].Value
    Stage='Per-user install; Explorer-backed taskbar replacement started at sign-in'
    Architecture='x64'
    PackagedUtc=[DateTime]::UtcNow.ToString('o')
    Binaries=$binaries
    RetainedTestedTrialSha256=$TestedTrialSha256
    ValidationEvidence='docs/progress.md; packaging does not run validation'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'build-info.json') -Encoding UTF8
# Active deployments and their recovery journals are never overwritten or removed.
'Packaged native binaries and taskbar test launchers in '+$out
