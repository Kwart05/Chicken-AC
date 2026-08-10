@echo off
title Chicken AC — Climate Controller Server
echo.
echo  ============================================
echo   Chicken AC — Climate Controller
echo  ============================================
echo.
echo  Port options:
echo    WIFI       = ESP8266 NodeMCU over local WiFi
echo    DEMO       = simulated data (no hardware)
echo    COM9       = Arduino via USB
echo    /dev/ttyUSB0 = Linux / Raspberry Pi
echo.
set /p PORT="Enter port [default: DEMO]: "
if "%PORT%"=="" set PORT=DEMO
echo.
echo  Starting on http://localhost:8000  (port=%PORT%)
echo  Open that URL in your browser.
echo  Close this window to stop the server.
echo  ============================================
echo.
cd /d "%~dp0server"
python app.py --port %PORT% --web-port 8000
pause
