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
rem collect training data from the watch into data\result_<date_time>.csv  (no extra libraries needed)
rem next step after collecting: double-click train_ai.bat
rem usage: collect_data.bat --subject Somchai     (add --sim for the virtual watch)
%PY% ml\collect.py %*
pause
