@echo off
rem Builds PS3EyeVCam.dll (MF virtual camera media source) and
rem PS3EyeVCamTray.exe (tray app: capture, sleep/wake, settings, autostart)
rem with VS2019 Build Tools. Static CRT (/MT) matches the prebuilt libusb.
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo error: could not initialize the VS2019 x64 toolchain
    exit /b 1
)

set "ROOT=%~dp0"
set "DS=%ROOT%..\PS3EyeDirectShow"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

rem /utf-8 is required: sources are UTF-8 without BOM and contain non-ASCII
rem string literals (em-dashes in tray tooltips); without it MSVC decodes them
rem as Windows-1252 and the tooltips show mojibake.
set CFLAGS=/nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

echo === compiling vendored PS3EYEDriver ===
cl %CFLAGS% /I "%DS%\libusb\libusb" "%ROOT%third_party\ps3eye\ps3eye.cpp" /Fo"%OUT%\ps3eye.obj"
if errorlevel 1 exit /b 1

echo === compiling PS3EyeVCam.dll ===
cl %CFLAGS% "%ROOT%source\VCamSource.cpp" /Fo"%OUT%\VCamSource.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS% "%ROOT%source\dllmain.cpp" /Fo"%OUT%\dllmain.obj"
if errorlevel 1 exit /b 1

link /nologo /DLL /DEF:"%ROOT%source\PS3EyeVCam.def" /OUT:"%OUT%\PS3EyeVCam.dll" ^
    "%OUT%\VCamSource.obj" "%OUT%\dllmain.obj" ^
    mfplat.lib mfuuid.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo === compiling resources ===
rc /nologo /fo "%OUT%\app.res" "%ROOT%res\app.rc"
if errorlevel 1 exit /b 1

echo === compiling PS3EyeVCamTray.exe ===
set HOSTFLAGS=%CFLAGS% /I "%ROOT%third_party\ps3eye" /I "%DS%\libusb\libusb"
cl %HOSTFLAGS% "%ROOT%host\Main.cpp"                /Fo"%OUT%\Main.obj"                || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CaptureController.cpp"   /Fo"%OUT%\CaptureController.obj"   || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\TrayUI.cpp"              /Fo"%OUT%\TrayUI.obj"              || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\SettingsDialog.cpp"      /Fo"%OUT%\SettingsDialog.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CameraPreview.cpp"       /Fo"%OUT%\CameraPreview.obj"       || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\Ps3EyePreviewSource.cpp" /Fo"%OUT%\Ps3EyePreviewSource.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\Autostart.cpp"           /Fo"%OUT%\Autostart.obj"           || exit /b 1
cl %HOSTFLAGS% "%ROOT%common\Settings.cpp"          /Fo"%OUT%\Settings.obj"            || exit /b 1

link /nologo /SUBSYSTEM:WINDOWS /OUT:"%OUT%\PS3EyeVCamTray.exe" ^
    "%OUT%\Main.obj" "%OUT%\CaptureController.obj" "%OUT%\TrayUI.obj" ^
    "%OUT%\SettingsDialog.obj" "%OUT%\CameraPreview.obj" "%OUT%\Ps3EyePreviewSource.obj" ^
    "%OUT%\Autostart.obj" "%OUT%\Settings.obj" ^
    "%OUT%\ps3eye.obj" "%OUT%\app.res" ^
    "%DS%\libusb\x64\Release\lib\libusb-1.0.lib" ^
    mfplat.lib mfuuid.lib ole32.lib oleaut32.lib advapi32.lib setupapi.lib ^
    user32.lib gdi32.lib shell32.lib comctl32.lib secur32.lib taskschd.lib uuid.lib ^
    /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%res\app.manifest" /MANIFESTUAC:NO
if errorlevel 1 exit /b 1

echo.
echo build OK:
echo   %OUT%\PS3EyeVCam.dll
echo   %OUT%\PS3EyeVCamTray.exe
echo next: run install.bat as Administrator
endlocal
