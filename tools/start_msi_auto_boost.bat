@echo off
title DS5-Bridge Pico 2 W - MSI Auto Cooler Boost and RAMMap Cleaner
fltmc >nul 2>&1
if %errorLevel% neq 0 (
    if /i not "%~1"=="elevated" (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs"
        exit /b
    )
)
chcp 65001 >nul
cd /d "%~dp0"
python "%~dp0ds5_msi_auto_boost.py" --auto-restore-fan
pause
