@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\Install-MinimaDock.ps1" -Action Uninstall
if errorlevel 1 pause
