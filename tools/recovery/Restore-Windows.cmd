@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Restore.ps1" -Bundle "%~dp0."
set "restore_result=%errorlevel%"
echo.
if not "%restore_result%"=="0" echo Recovery needs attention. Read the error above before signing out.
pause
exit /b %restore_result%
