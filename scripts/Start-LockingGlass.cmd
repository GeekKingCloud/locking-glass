@echo off
setlocal
start "" /D "%~dp0" "%~dp0Locking Glass.exe" --background
endlocal
exit /b 0
