@echo off
setlocal
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" tools\warehouse_cloud.py open
) else (
  where py >nul 2>&1
  if not errorlevel 1 (
    py -3 tools\warehouse_cloud.py open
  ) else (
    python tools\warehouse_cloud.py open
  )
)
if errorlevel 1 (
  echo.
  echo Dashboard could not start. Read the message above.
  pause
)
endlocal
