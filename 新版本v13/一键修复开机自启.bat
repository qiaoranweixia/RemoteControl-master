@echo off
chcp 936 >nul
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
  echo  需要管理员权限, 正在请求提权...
  powershell -NoProfile -Command "Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0fix_autostart.ps1' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fix_autostart.ps1"
