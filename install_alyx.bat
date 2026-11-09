@echo off
setlocal
cd /d "%~dp0"

if exist "dist\HLX-DLSS5-Installer.exe" (
    start "" "dist\HLX-DLSS5-Installer.exe"
    exit /b 0
)
if exist "build\HLX-DLSS5-Installer.exe" (
    start "" "build\HLX-DLSS5-Installer.exe"
    exit /b 0
)

echo [INFO] HLX-DLSS5-Installer.exe not yet built. Running build.bat first...
call build.bat
if exist "dist\HLX-DLSS5-Installer.exe" (
    start "" "dist\HLX-DLSS5-Installer.exe"
)
