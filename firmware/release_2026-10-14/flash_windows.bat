@echo off
setlocal
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"
if exist ".venv\Scripts\python.exe" (
  set "PYTHON=.venv\Scripts\python.exe"
) else (
  set "PYTHON=py"
)
"%PYTHON%" -m esptool --chip esp32c3 --port %PORT% --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
if errorlevel 1 exit /b %errorlevel%
