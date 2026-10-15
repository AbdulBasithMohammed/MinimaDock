# Launch the candidate's existing render-only mode. It does not create a live
# dock, hide Explorer or change the work area. Windows applies its normal policy.
function Invoke-DockRenderProbe([string]$Executable,[string]$Snapshot) {
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$Executable
    $info.Arguments='--snapshot-dock "'+$Snapshot+'"'
    $info.WorkingDirectory=[IO.Path]::GetDirectoryName($Executable)
    $info.UseShellExecute=$false
    $info.CreateNoWindow=$true
    $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $process=$null
    try {
        $process=[Diagnostics.Process]::Start($info)
        if(-not $process.WaitForExit(12000)){throw 'The dock render check timed out.'}
        return $process.ExitCode
    } finally {
        if($null -ne $process){
            try {if(-not $process.HasExited){$process.Kill();[void]$process.WaitForExit(1000)}}finally{$process.Dispose()}
        }
    }
}
function Assert-DockLaunchable([string]$Executable) {
    $path=[IO.Path]::GetFullPath($Executable)
    $snapshot=Join-Path ([IO.Path]::GetDirectoryName($path)) ('launch-check-'+[guid]::NewGuid().ToString('N')+'.png')
    $blocked='Windows Application Control blocked MinimaDock.exe. This build needs to be accepted by your Windows signing policy. Deployment stopped before starting the supervisor or changing the current taskbar.'
    try {
        try {$code=Invoke-DockRenderProbe $path $snapshot} catch {
            $exception=$_.Exception
            while($null -ne $exception -and $exception -isnot [ComponentModel.Win32Exception]){$exception=$exception.InnerException}
            if($null -ne $exception -and $exception.NativeErrorCode -in @(577,1260,4551)){throw $blocked}
            throw ('MinimaDock could not start its launch check: '+$_.Exception.Message+' The current taskbar was not changed.')
        }
        if($code -eq -1073740760){throw $blocked} # STATUS_INVALID_IMAGE_HASH
        if($code -ne 0){throw ('MinimaDock launch check exited with code '+$code+'. The current taskbar was not changed.')}
        if(-not (Test-Path -LiteralPath $snapshot) -or (Get-Item -LiteralPath $snapshot).Length -eq 0){throw 'MinimaDock launch check produced no rendered frame. The current taskbar was not changed.'}
    } finally {
        if(Test-Path -LiteralPath $snapshot){Remove-Item -LiteralPath $snapshot -Force}
    }
}
