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
rem  train_ai.bat = TRAIN the AI (use this one after collecting data)
rem   1) converts old-format files in data\ to result_*.csv (originals moved to data\old_format\)
rem   2) trains from data\result_*.csv ONLY (files listed in data\train_exclude.txt are skipped)
rem   3) saves data\model.json (+ a copy in data\models\)
rem   4) if this PC is on the watch WiFi now -> sends model.json to the watch automatically
rem      if not -> run_studio.bat will send it automatically when the watch connects
rem  options: --no-upload   --download (also pull data the watch recorded by itself)   --host 127.0.0.1:8081 (simulator)
rem ==========================================================================
%PY% ml\train.py %*
pause
