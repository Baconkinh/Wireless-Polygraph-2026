@echo off
setlocal
cd /d "%~dp0"
rem --- find a working Python (python first, then the "py" launcher) ---
rem     the Microsoft Store "python" alias fails the import test, so we skip it
set "PY="
python -c "import sys" >nul 2>nul && set "PY=python"
if not defined PY (py -3 -c "import sys" >nul 2>nul && set "PY=py -3")
if not defined PY (
  echo [ERROR] Python not found.
  echo Install from https://www.python.org/downloads/ and tick "Add python.exe to PATH"
  pause
  exit /b 1
)
%PY% -c "import fastapi, uvicorn, websockets" >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Libraries are missing. Double-click install.bat first ^(needs Internet^).
  pause
  exit /b 1
)
echo Connect this computer to WiFi "Polygraph-Watch" first, then wait for the browser.
%PY% -m backend %*
pause
