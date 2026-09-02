@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0native\build_native.ps1"
exit /b %ERRORLEVEL%
