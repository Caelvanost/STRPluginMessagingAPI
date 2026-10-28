@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File ".\build-vortex.ps1"
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)
echo.
echo BUILD COMPLETE.
endlocal
