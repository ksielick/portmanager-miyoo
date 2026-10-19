@echo off
rem Screenshot from the Miyoo over the network: saved to screenshots\ in the repo and opened.
rem Double-click it, or: tools\screenshot_miyoo.bat [out.png]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0screenshot_miyoo.ps1" %* -Open
if errorlevel 1 pause
