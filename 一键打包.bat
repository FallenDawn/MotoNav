@echo off
cd /d "%~dp0"
echo 1. Build release APK
echo 2. Replace release JKS
echo 0. Exit
choice /C 120 /N /M "Select: "
if errorlevel 3 exit /b 0
if errorlevel 2 goto change_jks
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Personal.ps1"
if errorlevel 1 echo Build failed. See the error above.
goto done
:change_jks
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Change-ReleaseJks.ps1"
:done
pause
