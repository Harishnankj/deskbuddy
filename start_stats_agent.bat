@echo off
title DeskBuddy PC Stats Agent
echo ===================================================
echo             DeskBuddy PC Stats Agent
echo ===================================================
echo.

set CONFIG_FILE=device_id.txt

if not exist "%CONFIG_FILE%" (
    echo [Setup] First time setup. Please enter your DeskBuddy Device ID.
    echo (You can find this on your DeskBuddy screen on startup, e.g. db-A1B2)
    echo.
    set /p DEVICE_ID="Device ID: "
    
    :: Save the device ID to the file
    echo | set /p="%DEVICE_ID%" > "%CONFIG_FILE%"
    echo.
    echo Saved Device ID to %CONFIG_FILE%
    echo.
) else (
    set /p DEVICE_ID=<"%CONFIG_FILE%"
)

:: Trim trailing/leading spaces if any
for /f "tokens=*" %%a in ("%DEVICE_ID%") do set DEVICE_ID=%%a

echo [Status] Connecting to DeskBuddy Device ID: %DEVICE_ID%
echo [Status] Starting stats agent...
echo.
echo (You can minimize this window. Press Ctrl+C to close)
echo.

:: Run node agent with the saved Device ID
node server/pc_agent.js %DEVICE_ID% --host=deskbuddy-relay.onrender.com

if %ERRORLEVEL% neq 0 (
    echo.
    echo [Error] Failed to start PC Stats Agent.
    echo Please make sure Node.js is installed on your PC.
    echo.
    pause
)
