@echo off
setlocal
set "DIAG_PS1=%~dp0scripts\AigisKR\diagnostics.ps1"
for %%I in ("%~dp0.") do set "GAME_ROOT=%%~fI"

if not exist "%DIAG_PS1%" (
    echo [AigisKR Diagnostics] Error: "%DIAG_PS1%" not found.
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%DIAG_PS1%" -GameRoot "%GAME_ROOT%"
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if "%EXIT_CODE%"=="0" (
    echo [AigisKR Diagnostics] Done. Attach the generated zip if reporting an issue.
) else (
    echo [AigisKR Diagnostics] Failed with exit code %EXIT_CODE%.
)
if "%AIGISKR_NO_PAUSE%"=="" pause
exit /b %EXIT_CODE%
