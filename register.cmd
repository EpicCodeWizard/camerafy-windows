@echo off
rem camerafy: install and register the camera DLLs (run elevated, once per build).
rem
rem   register.cmd              uses build\x64\Release and build\x86\Release next to this script
rem   register.cmd <x64dir> <x86dir>
rem
rem The DLLs are copied under Program Files: the Media Foundation source is loaded by the
rem Frame Server services (LocalService / LocalSystem) and by packaged apps, which cannot read
rem a user profile. Creating the cameras afterwards needs no admin.
setlocal EnableExtensions

net session >nul 2>&1
if errorlevel 1 (
    echo error: run this from an elevated prompt ^(Run as administrator^).
    exit /b 1
)

set "SRC64=%~1"
set "SRC86=%~2"
if "%SRC64%"=="" set "SRC64=%~dp0build\x64\Release"
if "%SRC86%"=="" set "SRC86=%~dp0build\x86\Release"
set "DST64=%ProgramFiles%\camerafy"
set "DST86=%ProgramFiles(x86)%\camerafy"

if not exist "%SRC64%\camerafy_mf.dll" (
    echo error: %SRC64%\camerafy_mf.dll not found. Build first: cmake --build --preset x64
    exit /b 1
)

if not exist "%DST64%" mkdir "%DST64%"
if not exist "%DST86%" mkdir "%DST86%"

call :install "%SRC64%\camerafy_mf.dll" "%DST64%" || exit /b 1
call :install "%SRC64%\camerafy_ds.dll" "%DST64%" || exit /b 1
if exist "%SRC86%\camerafy_ds.dll" (
    call :install "%SRC86%\camerafy_ds.dll" "%DST86%" || exit /b 1
) else (
    echo warning: no x86 build at %SRC86%; 32-bit DirectShow apps will not see the camera.
)

"%SystemRoot%\System32\regsvr32.exe" /s "%DST64%\camerafy_mf.dll" || (echo error: registering camerafy_mf.dll failed & exit /b 1)
"%SystemRoot%\System32\regsvr32.exe" /s "%DST64%\camerafy_ds.dll" || (echo error: registering x64 camerafy_ds.dll failed & exit /b 1)
if exist "%DST86%\camerafy_ds.dll" (
    "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%DST86%\camerafy_ds.dll" || (echo error: registering x86 camerafy_ds.dll failed & exit /b 1)
)

echo registered:
echo   %DST64%\camerafy_mf.dll   (Media Foundation virtual camera source)
echo   %DST64%\camerafy_ds.dll   (DirectShow filter, 64-bit)
if exist "%DST86%\camerafy_ds.dll" echo   %DST86%\camerafy_ds.dll   (DirectShow filter, 32-bit)
echo Now run camerafy.exe (no admin needed).
exit /b 0

:install
rem A running camera app or the Frame Server may hold the old DLL; stop the Frame Server
rem services once and retry rather than failing the upgrade.
copy /y "%~1" "%~2\" >nul 2>&1 && exit /b 0
echo %~nx1 is in use; stopping the Windows Camera Frame Server and retrying...
net stop FrameServerMonitor >nul 2>&1
net stop FrameServer >nul 2>&1
copy /y "%~1" "%~2\" >nul 2>&1 && exit /b 0
echo error: cannot replace %~2\%~nx1 ^(close every app using a camera and retry^).
exit /b 1
