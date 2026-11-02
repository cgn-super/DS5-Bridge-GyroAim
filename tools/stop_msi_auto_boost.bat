@echo off
chcp 65001 >nul
title Stop MSI Cooler Boost and Restore MSI AI Engine
cd /d "%~dp0"
python "%~dp0ds5_msi_auto_boost.py" --stop
echo.
pause
