@echo off
cd /d "%~dp0"
python build_flasher.py %*
if errorlevel 9009 echo Python not found - install it from python.org (tick "Add to PATH").
pause
