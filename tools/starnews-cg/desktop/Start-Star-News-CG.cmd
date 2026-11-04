@echo off
setlocal
cd /d "%~dp0"
if not exist node_modules\electron\dist\electron.exe (
  echo Installing desktop dependencies. This requires Node.js and internet access.
  call npm install --no-audit --no-fund
  if errorlevel 1 (pause & exit /b 1)
)
call npm start
if errorlevel 1 pause
