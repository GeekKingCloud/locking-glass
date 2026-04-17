@echo off
setlocal
pushd "%~dp0"
start "" "%~dp0LockingGlass.exe" --background
popd
endlocal
