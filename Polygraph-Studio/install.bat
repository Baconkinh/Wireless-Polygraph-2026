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
echo ============================================================
echo  Polygraph Studio - install libraries (once, needs Internet)
echo  Using: %PY%
echo ============================================================
%PY% -m pip install --upgrade pip
%PY% -m pip install -r requirements.txt
if errorlevel 1 (
  echo.
  echo [ERROR] Install failed. Check the Internet connection ^(not the watch WiFi^) and try again.
) else (
  echo.
  echo Done. Next: double-click run_studio.bat  ^(or run_demo_simulator.bat without hardware^)
)
pause
