@echo off
setlocal

set "ALYX_BIN=C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\game\bin\win64"

echo ========================================================
echo   HLX-DLSS5: Desinstallation / Restauration Vanilla
echo ========================================================

if exist "%ALYX_BIN%\openvr_api_real.dll" (
    echo [1/2] Restauration du vrai openvr_api.dll...
    copy /y "%ALYX_BIN%\openvr_api_real.dll" "%ALYX_BIN%\openvr_api.dll" >nul
    del "%ALYX_BIN%\openvr_api_real.dll"
) else (
    echo [1/2] openvr_api_real.dll non trouve, rien a restaurer.
)

if exist "%ALYX_BIN%\hlx_dlss5.ini" (
    echo [2/2] Nettoyage de hlx_dlss5.ini et hlx_dlss5.log...
    del "%ALYX_BIN%\hlx_dlss5.ini" 2>nul
    del "%ALYX_BIN%\hlx_dlss5.log" 2>nul
)

echo.
echo [SUCCES] Half-Life: Alyx est restaure en configuration vanilla d'origine.
echo.
pause
