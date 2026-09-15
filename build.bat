@echo off
rem Builds PSCam4Win.dll (MF virtual camera media source), PSCam4WinTray.exe
rem (tray app: capture, sleep/wake, settings, autostart), and
rem PSCam4Win-Setup.exe (GUI installer with the payload embedded as resources)
rem with VS2019 Build Tools. Static CRT (/MT) matches the prebuilt libusb.
rem
rem   build.bat            full build (all three artifacts)
rem   build.bat installer  re-embed + relink ONLY the setup exe (used by CI to
rem                        rebuild the installer around signed payload binaries)
rem
rem PSCAM_SIGN_CMD (optional): a command invoked as  %PSCAM_SIGN_CMD% "<file>"
rem after each binary links, for local Authenticode signing setups.
setlocal
set "BUILD_MODE=%~1"

rem ---- Locate VS compiler using vswhere -------------------------------------
set "VS_PATH="
set "VCVARS_PATH="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%temp%\vs_path.txt" 2>nul
    if exist "%temp%\vs_path.txt" (
        set /p VS_PATH=<"%temp%\vs_path.txt"
        del "%temp%\vs_path.txt"
    )
)

if defined VS_PATH (
    if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
    )
)

rem Fallback to default hardcoded paths if vswhere couldn't find it
if not defined VCVARS_PATH (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS_PATH (
    echo error: could not locate Visual Studio C++ build tools: vcvars64.bat
    echo Please install Visual Studio 2019 or 2022 with the "Desktop development with C++" workload.
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

call "%VCVARS_PATH%" >nul
if errorlevel 1 (
    echo error: could not initialize the Visual Studio x64 toolchain
    if not defined GITHUB_ACTIONS pause
    exit /b 1
)

set "ROOT=%~dp0"
rem libusb is vendored in-repo (third_party/libusb): prebuilt 1.0.27 static /MT x64,
rem header in include/. See third_party/libusb/README.md. No external repo needed.
set "LIBUSB_INC=%ROOT%third_party\libusb\include"
set "LIBUSB_LIB=%ROOT%third_party\libusb\lib\x64\libusb-1.0.lib"
rem libjpeg-turbo is vendored too (third_party/libjpeg-turbo): prebuilt 3.1.4
rem static /MT x64 (TurboJPEG API). Used only by the EyeToy JFIF->YUY2 decode.
rem Each path is quoted (the repo path contains spaces) so the two libs survive
rem as separate tokens on the link line.
set TJPEG_LIB="%ROOT%third_party\libjpeg-turbo\lib\x64\turbojpeg.lib" "%ROOT%third_party\libjpeg-turbo\lib\x64\jpeg.lib"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

rem /utf-8 is required: sources are UTF-8 without BOM and contain non-ASCII
rem string literals (em-dashes in tray tooltips); without it MSVC decodes them
rem as Windows-1252 and the tooltips show mojibake.
rem /W4 on everything this project writes -- it is what catches the shadowed
rem locals and unused parameters that /W3 lets through, and the tree builds clean
rem at it. VENDORFLAGS drops back to /W3 for third_party\ps3eye only: warnings in
rem code we do not author are noise we cannot act on without diverging upstream.
set CFLAGS=/nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS
set VENDORFLAGS=%CFLAGS:/W4=/W3%
if "%PSCAM_PUBLIC_RELEASE%"=="1" set CFLAGS=%CFLAGS% /DPSCAM_PUBLIC_RELEASE

if /i "%BUILD_MODE%"=="installer" goto installer_phase

echo === compiling vendored PS3EYEDriver ===
cl %VENDORFLAGS% /I "%LIBUSB_INC%" "%ROOT%third_party\ps3eye\ps3eye.cpp" /Fo"%OUT%\ps3eye.obj"
if errorlevel 1 exit /b 1

echo === compiling PSCam4Win.dll ===
cl %CFLAGS% "%ROOT%source\VCamSource.cpp" /Fo"%OUT%\VCamSource.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS% "%ROOT%source\dllmain.cpp" /Fo"%OUT%\dllmain.obj"
if errorlevel 1 exit /b 1

link /nologo /DLL /DEF:"%ROOT%source\PSCam4Win.def" /OUT:"%OUT%\PSCam4Win.dll" ^
    "%OUT%\VCamSource.obj" "%OUT%\dllmain.obj" ^
    mfplat.lib mfuuid.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
if defined PSCAM_SIGN_CMD call %PSCAM_SIGN_CMD% "%OUT%\PSCam4Win.dll"

echo === compiling resources ===
rc /nologo /fo "%OUT%\app.res" "%ROOT%res\app.rc"
if errorlevel 1 exit /b 1

echo === compiling PSCam4WinTray.exe ===
set HOSTFLAGS=%CFLAGS% /I "%ROOT%third_party\ps3eye" /I "%LIBUSB_INC%"
cl %HOSTFLAGS% "%ROOT%host\Main.cpp"                /Fo"%OUT%\Main.obj"                || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CaptureController.cpp"   /Fo"%OUT%\CaptureController.obj"   || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\AudioRender.cpp"         /Fo"%OUT%\AudioRender.obj"         || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\TrayUI.cpp"              /Fo"%OUT%\TrayUI.obj"              || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\MicWatch.cpp"            /Fo"%OUT%\MicWatch.obj"            || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\HostLog.cpp"             /Fo"%OUT%\HostLog.obj"             || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\ui\Theme.cpp"            /Fo"%OUT%\Theme.obj"              || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\ui\Widgets.cpp"          /Fo"%OUT%\Widgets.obj"            || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\ui\SettingsWindow.cpp"   /Fo"%OUT%\SettingsWindow.obj"     || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\ui\TrayMenu.cpp"         /Fo"%OUT%\TrayMenu.obj"           || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CameraPreview.cpp"       /Fo"%OUT%\CameraPreview.obj"       || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\FrameBusPreviewSource.cpp" /Fo"%OUT%\FrameBusPreviewSource.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\Autostart.cpp"           /Fo"%OUT%\Autostart.obj"           || exit /b 1
cl %HOSTFLAGS% "%ROOT%common\Settings.cpp"          /Fo"%OUT%\Settings.obj"            || exit /b 1
cl %HOSTFLAGS% "%ROOT%common\UsbCore.cpp"           /Fo"%OUT%\UsbCore.obj"             || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\DeviceProfiles.cpp"      /Fo"%OUT%\DeviceProfiles.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\DeviceRegistry.cpp"      /Fo"%OUT%\DeviceRegistry.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_bulk\Ps3EyeDevice.cpp" /Fo"%OUT%\Ps3EyeDevice.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_iso\EyeToyUsb.cpp"    /Fo"%OUT%\EyeToyUsb.obj"    || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_iso\EyeToyDevice.cpp" /Fo"%OUT%\EyeToyDevice.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_ps4\Ps4Usb.cpp"           /Fo"%OUT%\Ps4Usb.obj"           || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_ps4\Ps4Firmware.cpp"      /Fo"%OUT%\Ps4Firmware.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_ps4\Ps4Demosaic.cpp"      /Fo"%OUT%\Ps4Demosaic.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_ps4\Ps4CaptureSource.cpp" /Fo"%OUT%\Ps4CaptureSource.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_ps4\Ps4ViewDevice.cpp"    /Fo"%OUT%\Ps4ViewDevice.obj"    || exit /b 1

rem /LTCG: the vendored libusb is built with whole-program optimization (/GL);
rem stating /LTCG here avoids the linker's automatic "restarting link" pass.
link /nologo /LTCG /SUBSYSTEM:WINDOWS /OUT:"%OUT%\PSCam4WinTray.exe" ^
    "%OUT%\Main.obj" "%OUT%\CaptureController.obj" "%OUT%\AudioRender.obj" "%OUT%\TrayUI.obj" ^
    "%OUT%\MicWatch.obj" "%OUT%\HostLog.obj" ^
    "%OUT%\Theme.obj" "%OUT%\Widgets.obj" "%OUT%\SettingsWindow.obj" "%OUT%\TrayMenu.obj" ^
    "%OUT%\CameraPreview.obj" "%OUT%\FrameBusPreviewSource.obj" ^
    "%OUT%\Autostart.obj" "%OUT%\Settings.obj" "%OUT%\UsbCore.obj" ^
    "%OUT%\DeviceProfiles.obj" "%OUT%\DeviceRegistry.obj" "%OUT%\Ps3EyeDevice.obj" ^
    "%OUT%\EyeToyUsb.obj" "%OUT%\EyeToyDevice.obj" ^
    "%OUT%\Ps4Usb.obj" "%OUT%\Ps4Firmware.obj" "%OUT%\Ps4Demosaic.obj" ^
    "%OUT%\Ps4CaptureSource.obj" "%OUT%\Ps4ViewDevice.obj" ^
    "%OUT%\ps3eye.obj" "%OUT%\app.res" ^
    "%LIBUSB_LIB%" %TJPEG_LIB% ^
    mfplat.lib mfuuid.lib ole32.lib oleaut32.lib advapi32.lib setupapi.lib ^
    user32.lib gdi32.lib shell32.lib comctl32.lib uxtheme.lib dwmapi.lib secur32.lib taskschd.lib uuid.lib bcrypt.lib ^
    /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%res\app.manifest" /MANIFESTUAC:NO
if errorlevel 1 exit /b 1
if defined PSCAM_SIGN_CMD call %PSCAM_SIGN_CMD% "%OUT%\PSCam4WinTray.exe"

:installer_phase
echo === compiling PSCam4Win-Setup.exe ===
rem The installer embeds build\PSCam4Win.dll + build\PSCam4WinTray.exe as
rem resources, so the main build must have run first ("installer" mode relinks
rem the setup exe around whatever binaries are in build\ -- CI uses this to
rem re-embed SIGNED payload binaries).
if not exist "%OUT%\PSCam4Win.dll" (
    echo error: build\PSCam4Win.dll missing -- run a full build.bat first
    exit /b 1
)
if not exist "%OUT%\PSCam4WinTray.exe" (
    echo error: build\PSCam4WinTray.exe missing -- run a full build.bat first
    exit /b 1
)
rem /i "%ROOT%." lets installer.rc reference payload files repo-root-relative.
rc /nologo /i "%ROOT%." /i "%ROOT%res" /i "%ROOT%installer" /fo "%OUT%\installer.res" "%ROOT%installer\installer.rc"
if errorlevel 1 exit /b 1
cl %CFLAGS% "%ROOT%installer\SetupMain.cpp"    /Fo"%OUT%\SetupMain.obj"    || exit /b 1
cl %CFLAGS% "%ROOT%installer\SetupWizard.cpp"  /Fo"%OUT%\SetupWizard.obj"  || exit /b 1
cl %CFLAGS% "%ROOT%installer\InstallSteps.cpp" /Fo"%OUT%\InstallSteps.obj" || exit /b 1
cl %CFLAGS% "%ROOT%installer\SetupOps.cpp"     /Fo"%OUT%\SetupOps.obj"     || exit /b 1
cl %CFLAGS% "%ROOT%installer\Payload.cpp"      /Fo"%OUT%\Payload.obj"      || exit /b 1
cl %CFLAGS% "%ROOT%installer\SetupLog.cpp"     /Fo"%OUT%\SetupLog.obj"     || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OUT:"%OUT%\PSCam4Win-Setup.exe" ^
    "%OUT%\SetupMain.obj" "%OUT%\SetupWizard.obj" "%OUT%\InstallSteps.obj" ^
    "%OUT%\SetupOps.obj" "%OUT%\Payload.obj" "%OUT%\SetupLog.obj" ^
    "%OUT%\installer.res" ^
    user32.lib gdi32.lib comctl32.lib shell32.lib shlwapi.lib advapi32.lib ^
    ole32.lib oleaut32.lib uuid.lib crypt32.lib bcrypt.lib setupapi.lib ^
    taskschd.lib winhttp.lib ^
    /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%installer\setup.manifest" /MANIFESTUAC:NO
if errorlevel 1 exit /b 1
if defined PSCAM_SIGN_CMD call %PSCAM_SIGN_CMD% "%OUT%\PSCam4Win-Setup.exe"

echo.
echo build OK:
if /i not "%BUILD_MODE%"=="installer" echo   %OUT%\PSCam4Win.dll
if /i not "%BUILD_MODE%"=="installer" echo   %OUT%\PSCam4WinTray.exe
echo   %OUT%\PSCam4Win-Setup.exe
echo next: run build\PSCam4Win-Setup.exe (it self-elevates)
endlocal
