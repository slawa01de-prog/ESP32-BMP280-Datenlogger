@echo off
cd /d "%~dp0"
py -m pip install pyserial matplotlib pyinstaller
py -m PyInstaller --onefile --windowed --clean --name BMP280_Logger_V74 --collect-all matplotlib --hidden-import serial.tools.list_ports bmp280_logger_v74_gui.py
pause
