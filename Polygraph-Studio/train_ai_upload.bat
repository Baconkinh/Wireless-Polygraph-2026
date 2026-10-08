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
rem ==========================================================================
rem  train_ai_upload.bat = SEND the existing data\model.json to the watch (NO training)
rem   use when: you trained while the PC was not on the watch WiFi, and you do not want to open Studio
rem   (run_studio.bat already sends the newest model.json automatically - this file is just a manual option)
rem   connect this PC to WiFi "Polygraph-Watch" first
rem ==========================================================================
%PY% ml\train.py --upload-only %*
pause
