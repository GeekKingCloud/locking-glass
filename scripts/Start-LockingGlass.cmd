@echo off
setlocal
pushd "%~dp0"
start "" "%~dp0Locking Glass.exe" --background
popd
endlocal
