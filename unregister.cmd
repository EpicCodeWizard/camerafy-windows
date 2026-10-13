@echo off
rem camerafy: unregister and remove the camera DLLs (run elevated).
setlocal EnableExtensions

net session >nul 2>&1
if errorlevel 1 (
    echo error: run this from an elevated prompt ^(Run as administrator^).
    exit /b 1
)

set "DST64=%ProgramFiles%\camerafy"
set "DST86=%ProgramFiles(x86)%\camerafy"

if exist "%DST64%\camerafy_mf.dll" "%SystemRoot%\System32\regsvr32.exe" /s /u "%DST64%\camerafy_mf.dll"
if exist "%DST64%\camerafy_ds.dll" "%SystemRoot%\System32\regsvr32.exe" /s /u "%DST64%\camerafy_ds.dll"
if exist "%DST86%\camerafy_ds.dll" "%SystemRoot%\SysWOW64\regsvr32.exe" /s /u "%DST86%\camerafy_ds.dll"

rem Per-user device entries left behind by a crashed CLI (normally removed on exit).
reg delete "HKCU\Software\Classes\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\{E63A4255-4E3B-48D8-8B63-461FD7470F7A}" /f >nul 2>&1
reg delete "HKCU\Software\Classes\WOW6432Node\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\{E63A4255-4E3B-48D8-8B63-461FD7470F7A}" /f >nul 2>&1

net stop FrameServerMonitor >nul 2>&1
net stop FrameServer >nul 2>&1
del /q "%DST64%\camerafy_mf.dll" "%DST64%\camerafy_ds.dll" >nul 2>&1
del /q "%DST86%\camerafy_ds.dll" >nul 2>&1
rmdir "%DST64%" >nul 2>&1
rmdir "%DST86%" >nul 2>&1
echo unregistered.
exit /b 0
