@echo off
setlocal
set "ROOT=%~dp0.."
set "TEST_VS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TEST_VS=%%i"
if not defined TEST_VS exit /b 1
call "%TEST_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%ROOT%\build\SetupOps.obj" (
    echo Run build.bat before building the regression tests.
    exit /b 1
)
if /i "%~1"=="public" (
    set "PSCAM_PUBLIC_RELEASE=1"
    call "%ROOT%\build.bat" installer
    if errorlevel 1 exit /b 1
)
set "OUT=%ROOT%\build\tests"
if not exist "%OUT%" mkdir "%OUT%"
set FLAGS=/nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS
if "%PSCAM_PUBLIC_RELEASE%"=="1" set FLAGS=%FLAGS% /DPSCAM_PUBLIC_RELEASE
cl %FLAGS% "%ROOT%\tests\installer_tests.cpp" "%ROOT%\build\SetupOps.obj" "%ROOT%\build\SetupLog.obj" "%ROOT%\build\Payload.obj" /Fo"%OUT%\installer_tests.obj" /Fe"%OUT%\installer_tests.exe" /link advapi32.lib setupapi.lib ole32.lib oleaut32.lib crypt32.lib shlwapi.lib taskschd.lib user32.lib shell32.lib winhttp.lib bcrypt.lib || exit /b 1
cl %FLAGS% "%ROOT%\tests\framebus_tests.cpp" /Fo"%OUT%\framebus_tests.obj" /Fe"%OUT%\framebus_tests.exe" /link advapi32.lib ole32.lib || exit /b 1
cl %FLAGS% "%ROOT%\tests\source_tests.cpp" "%ROOT%\third_party\libjpeg-turbo\lib\x64\turbojpeg.lib" "%ROOT%\third_party\libjpeg-turbo\lib\x64\jpeg.lib" /Fo"%OUT%\source_tests.obj" /Fe"%OUT%\source_tests.exe" /link mfplat.lib mfuuid.lib ole32.lib advapi32.lib || exit /b 1
cl %FLAGS% /I "%ROOT%\third_party\libusb\include" "%ROOT%\tests\jpeg_tests.cpp" "%ROOT%\build\UsbCore.obj" "%ROOT%\build\EyeToyUsb.obj" "%ROOT%\build\DeviceProfiles.obj" "%ROOT%\third_party\libusb\lib\x64\libusb-1.0.lib" "%ROOT%\third_party\libjpeg-turbo\lib\x64\turbojpeg.lib" "%ROOT%\third_party\libjpeg-turbo\lib\x64\jpeg.lib" /Fo"%OUT%\jpeg_tests.obj" /Fe"%OUT%\jpeg_tests.exe" /link /LTCG ole32.lib advapi32.lib || exit /b 1
cl %FLAGS% /I "%ROOT%\third_party\libusb\include" "%ROOT%\tests\host_tests.cpp" "%ROOT%\build\Ps4ViewDevice.obj" "%ROOT%\build\Ps4Usb.obj" "%ROOT%\build\Ps4Firmware.obj" "%ROOT%\build\Ps4Demosaic.obj" "%ROOT%\build\UsbCore.obj" "%ROOT%\build\DeviceProfiles.obj" "%ROOT%\third_party\libusb\lib\x64\libusb-1.0.lib" /Fo"%OUT%\host_tests.obj" /Fe"%OUT%\host_tests.exe" /link /LTCG ole32.lib advapi32.lib shell32.lib winhttp.lib bcrypt.lib || exit /b 1
cl %FLAGS% "%ROOT%\tests\ui_tests.cpp" "%ROOT%\build\Theme.obj" "%ROOT%\build\Widgets.obj" "%ROOT%\build\Settings.obj" "%ROOT%\build\FrameBusPreviewSource.obj" /Fo"%OUT%\ui_tests.obj" /Fe"%OUT%\ui_tests.exe" /link user32.lib gdi32.lib dwmapi.lib comctl32.lib uxtheme.lib ole32.lib advapi32.lib || exit /b 1
"%OUT%\installer_tests.exe" || exit /b 1
"%OUT%\framebus_tests.exe" || exit /b 1
"%OUT%\source_tests.exe" || exit /b 1
"%OUT%\jpeg_tests.exe" || exit /b 1
"%OUT%\host_tests.exe" || exit /b 1
"%OUT%\ui_tests.exe" || exit /b 1
endlocal
