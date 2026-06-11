@echo off
rem Completely removes the PS3 Eye virtual camera: process, logon task,
rem DLL registration, settings, files. Must run as Administrator.
rem - self-elevates if run as standard user
setlocal

:: If not running the temp copy, copy to temp and run from there so we don't lock %DEST%
if /i "%~nx0" neq "uninstall_temp.bat" (
    copy /y "%~f0" "%temp%\uninstall_temp.bat" >nul
    start "" "%temp%\uninstall_temp.bat"
    exit /b
)

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrative privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set "DEST=%ProgramFiles%\PS3EyeVCam"

echo Stopping existing host app instance...
taskkill /im PS3EyeVCamTray.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im PS3EyeVCamTray.exe >nul 2>&1

echo Deleting Startup Task from Task Scheduler...
schtasks /delete /tn "PS3EyeVCam" /f >nul 2>&1

echo Stopping Camera Services...
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

echo Unregistering Virtual Camera DLL...
if exist "%DEST%\PS3EyeVCam.dll" (
    regsvr32 /u /s "%DEST%\PS3EyeVCam.dll"
)

echo Deleting registry settings...
reg delete "HKLM\SOFTWARE\PS3EyeVCam" /f >nul 2>&1

echo Removing Video and Audio Drivers...
powershell -Command "$oem = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*usb_device.inf' -or $_.OriginalFileName -like '*usb_audio.inf' } | Select-Object -ExpandProperty Driver; if ($oem) { foreach ($d in $oem) { pnputil /delete-driver $d /uninstall /force } }" >nul 2>&1


echo Removing WinUSB Driver Certificate...
certutil -delstore "Root" "d0a3c5233b11288afa8d6924949d985988b536f6" >nul 2>&1
certutil -delstore "TrustedPublisher" "d0a3c5233b11288afa8d6924949d985988b536f6" >nul 2>&1

echo Cleaning up files...
if exist "%DEST%" (
    rmdir /s /q "%DEST%"
)

echo Restarting Camera Services...
net start FrameServer >nul 2>&1
net start FrameServerMonitor >nul 2>&1

echo.
echo Uninstalled successfully!
echo - Camera files and scheduled tasks removed.
echo - Registry settings deleted.
echo.
pause
endlocal
