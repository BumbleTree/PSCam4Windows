@echo off
rem Installs the PS3 Eye virtual camera. Must run as Administrator.
rem - self-elevates if run as standard user
rem - binaries to Program Files (Frame Server can't read user folders)
rem - registers the media source DLL (HKLM)
rem - seeds default settings (640x480@60, autogain)
rem - enables silent elevated start-at-logon (in-app toggle can disable it)
rem - starts the tray app
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrative privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set "SRC=%~dp0build"
set "DEST=%ProgramFiles%\PS3EyeVCam"

if not exist "%SRC%\PS3EyeVCam.dll" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)
if not exist "%SRC%\PS3EyeVCamTray.exe" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)

echo Stopping Camera Services...
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

echo Stopping existing host app instance...
taskkill /im PS3EyeVCamTray.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im PS3EyeVCamTray.exe >nul 2>&1

echo Creating destination directories...
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\driver" mkdir "%DEST%\driver"
if not exist "%DEST%\driver\amd64" mkdir "%DEST%\driver\amd64"

echo Copying files...
copy /y "%SRC%\PS3EyeVCam.dll" "%DEST%\" >nul
if errorlevel 1 (
    echo error: could not copy PS3EyeVCam.dll - still in use?
    pause
    exit /b 1
)
copy /y "%SRC%\PS3EyeVCamTray.exe" "%DEST%\" >nul
if errorlevel 1 (
    echo error: could not copy PS3EyeVCamTray.exe - still running?
    pause
    exit /b 1
)
copy /y "%~dp0uninstall.bat" "%DEST%\" >nul
if errorlevel 1 (
    echo error: could not copy uninstall.bat
    pause
    exit /b 1
)
copy /y "%~dp0driver\usb_device.inf" "%DEST%\driver\" >nul
if errorlevel 1 (
    echo error: could not copy usb_device.inf
    pause
    exit /b 1
)
copy /y "%~dp0driver\usb_device.cat" "%DEST%\driver\" >nul
if errorlevel 1 (
    echo error: could not copy usb_device.cat
    pause
    exit /b 1
)
copy /y "%~dp0driver\usb_device.cer" "%DEST%\driver\" >nul
if errorlevel 1 (
    echo error: could not copy usb_device.cer
    pause
    exit /b 1
)
copy /y "%~dp0driver\usb_audio.inf" "%DEST%\driver\" >nul
if errorlevel 1 (
    echo error: could not copy usb_audio.inf
    pause
    exit /b 1
)
copy /y "%~dp0driver\usb_audio.cat" "%DEST%\driver\" >nul
if errorlevel 1 (
    echo error: could not copy usb_audio.cat
    pause
    exit /b 1
)
copy /y "%~dp0driver\amd64\WdfCoInstaller01011.dll" "%DEST%\driver\amd64\" >nul
if errorlevel 1 (
    echo error: could not copy WdfCoInstaller01011.dll
    pause
    exit /b 1
)
copy /y "%~dp0driver\amd64\winusbcoinstaller2.dll" "%DEST%\driver\amd64\" >nul
if errorlevel 1 (
    echo error: could not copy winusbcoinstaller2.dll
    pause
    exit /b 1
)

echo Registering Virtual Camera DLL...
regsvr32 /s "%DEST%\PS3EyeVCam.dll"
if errorlevel 1 (
    echo error: regsvr32 failed
    pause
    exit /b 1
)

echo Registering WinUSB Driver Certificate...
certutil -addstore "Root" "%DEST%\driver\usb_device.cer" >nul 2>&1
certutil -addstore "TrustedPublisher" "%DEST%\driver\usb_device.cer" >nul 2>&1

echo Installing WinUSB Video Driver...
pnputil /add-driver "%DEST%\driver\usb_device.inf" /install >nul 2>&1

echo Installing custom PS3 Eye Audio Driver...
pnputil /add-driver "%DEST%\driver\usb_audio.inf" /install >nul 2>&1




echo Writing default registry settings (Highest Resolution: 640x480@60)...
"%DEST%\PS3EyeVCamTray.exe" --seed-defaults

echo Registering Startup Task in Task Scheduler...
"%DEST%\PS3EyeVCamTray.exe" --enable-autostart
if errorlevel 1 (
    echo warning: could not create the logon task - toggle it in the app
)

echo Restarting Camera Services...
net start FrameServer >nul 2>&1
net start FrameServerMonitor >nul 2>&1

echo Starting camera host app...
start "" "%DEST%\PS3EyeVCamTray.exe"

echo.
echo Installed successfully!
echo - Camera files copied to: %DEST%
echo - System tray app registered to start elevated on Windows logon.
echo - Camera host started. Look for the camera icon in your System Tray.
echo.
pause
endlocal
