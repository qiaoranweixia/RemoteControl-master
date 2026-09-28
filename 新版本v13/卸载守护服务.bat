@echo off
chcp 936 >nul
cd /d "%~dp0"
title 卸载 RemoteControl 守护服务
net session >nul 2>&1
if errorlevel 1 (
  echo 需要管理员权限, 正在重新以管理员身份启动...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
echo [1/3] 停止并删除守护服务...
"%~dp0RCSvc.exe" --uninstall
echo [2/3] 删除注册表 Run 键...
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v RemoteControlAgent /f
echo [3/3] 结束被控端进程...
taskkill /f /im RemoteControl.exe >nul 2>&1
echo 完成。
pause
