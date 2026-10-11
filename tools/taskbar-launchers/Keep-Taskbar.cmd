@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\Taskbar-Test.ps1" -Action Keep
if errorlevel 1 pause
