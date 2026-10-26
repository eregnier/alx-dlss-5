@echo off
setlocal

set "ALYX_BIN=C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\game\bin\win64"
set "BUILD_DLL=%~dp0build\openvr_api.dll"
set "CONFIG_INI=%~dp0hlx_dlss5.ini"
set "MODEL_DLL=C:\code\dlss5-audit\samples\_extracted\rhi_dlssnr_nvngx_dlssnr_310.8.Lecram\nvngx_dlssnr.dll"

echo ========================================================
echo   HLX-DLSS5: Installation pour Half-Life: Alyx
echo ========================================================

if not exist "%ALYX_BIN%\hlvr.exe" (
    echo [ERREUR] Impossible de trouver hlvr.exe dans "%ALYX_BIN%".
    pause
    exit /b 1
)

if not exist "%BUILD_DLL%" (
    echo [ERREUR] Le binaire compil build\openvr_api.dll n'existe pas. Lancez build.bat d'abord.
    pause
    exit /b 1
)

:: Sauvegarde du vrai openvr_api.dll
if not exist "%ALYX_BIN%\openvr_api_real.dll" (
    echo [1/3] Sauvegarde de l'original openvr_api.dll vers openvr_api_real.dll...
    copy "%ALYX_BIN%\openvr_api.dll" "%ALYX_BIN%\openvr_api_real.dll" >nul
) else (
    echo [1/3] openvr_api_real.dll deja present.
)

:: Copie du proxy et du forwarder
echo [2/3] Deploiement du proxy openvr_api.dll, forwarder et configuration...
copy /y "%BUILD_DLL%" "%ALYX_BIN%\openvr_api.dll" >nul
copy /y "%CONFIG_INI%" "%ALYX_BIN%\hlx_dlss5.ini" >nul
if exist "%~dp0bin\nvngx.dll_dlssnr.dll" (
    copy /y "%~dp0bin\nvngx.dll_dlssnr.dll" "%ALYX_BIN%\nvngx.dll_dlssnr.dll" >nul
)

:: Copie du runtime DLSS 5 si present
if exist "%MODEL_DLL%" (
    if not exist "%ALYX_BIN%\nvngx_dlssnr.dll" (
        echo [3/3] Copie du modele DLSS 5 (nvngx_dlssnr.dll)...
        copy "%MODEL_DLL%" "%ALYX_BIN%\nvngx_dlssnr.dll" >nul
    ) else (
        echo [3/3] nvngx_dlssnr.dll deja present dans le dossier de jeu.
    )
) else (
    echo [3/3] nvngx_dlssnr.dll introuvable au chemin d'audit, recherche automatique activee.
)

echo.
echo [SUCCES] HLX-DLSS5 est installe ! Lancez le jeu via Steam.
echo Pour desinstaller : lancez uninstall_alyx.bat
echo.
pause
