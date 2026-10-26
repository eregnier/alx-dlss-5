@echo off
setlocal
echo ========================================================
echo   HLX-DLSS5: Suite Complete de Tests Automatises
echo ========================================================

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cd /d "%~dp0build"
ctest --output-on-failure
if %ERRORLEVEL% equ 0 (
    echo.
    echo [SUCCES] 100%% des tests unitaires et d'interop GPU sont valides !
) else (
    echo.
    echo [ECHEC] Au moins un test a echoue. Verifiez les logs ci-dessus.
)
