@echo off
cd /d "%~dp0"
set PORT=%~1
if "%PORT%"=="" set PORT=COM3
echo Uploading to %PORT% ...
pio run -e cardputer-adv -t upload --upload-port %PORT%
exit /b %ERRORLEVEL%
