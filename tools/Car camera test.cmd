@echo off
rem Runs carcam_test.py as administrator (NR2003 runs as administrator, so the test must too).
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
cd /d "%~dp0"
python carcam_test.py
pause
