@echo off
setlocal

if not defined HT2MP_LISTEN set "HT2MP_LISTEN=0.0.0.0:28020"
if not "%~1"=="" set "HT2MP_LISTEN=%~1"

set "HT2MP_SERVER=%~dp0bin\ht2mp-coordinator.exe"
if not exist "%HT2MP_SERVER%" set "HT2MP_SERVER=%~dp0ht2mp-coordinator.exe"

echo HT2MP server
echo Endpoint: %HT2MP_LISTEN%
echo Profile:  steam-8138acee
echo.
echo A new access token will be printed below. Send it only to players you trust.
echo Keep this window open. Press Ctrl+C to stop the server.
echo.

"%HT2MP_SERVER%" --listen "%HT2MP_LISTEN%" --profile steam-8138acee
set "HT2MP_EXIT_CODE=%ERRORLEVEL%"

echo.
if not "%HT2MP_EXIT_CODE%"=="0" echo Server stopped with error %HT2MP_EXIT_CODE%.
if "%HT2MP_EXIT_CODE%"=="0" echo Server stopped normally.
pause
exit /b %HT2MP_EXIT_CODE%
