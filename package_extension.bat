@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package_extension.ps1"
exit /b %ERRORLEVEL%
