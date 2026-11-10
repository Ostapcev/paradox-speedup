@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
echo EU4 CPU profiler: 60 seconds, starting now.
echo The game must already be running and unpaused at maximum speed.
echo Do not close this window until the report is written.
echo.
"%~dp0eu4_sampler.exe" --seconds 60 --interval-ms 5 --output "%~dp0latest"
set result=%ERRORLEVEL%
echo.
if not "%result%"=="0" (
    echo Profiling did not complete successfully. Exit code: %result%
    pause
    exit /b %result%
)
echo Profiling finished. Report files are in "%~dp0latest".
echo.
python "%~dp0tools\analyze_stacks.py" "%~dp0latest" > "%~dp0latest\summary.txt"
type "%~dp0latest\summary.txt"
pause
exit /b 0
