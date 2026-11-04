@echo off
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel% equ 0 (
  py -3 server.py --open
  pause
  exit /b
)
where python >nul 2>nul
if %errorlevel% equ 0 (
  python server.py --open
) else (
  echo Python 3 is required for the OBS control relay.
  echo You can also open index.html directly for a standalone preview.
)
pause
