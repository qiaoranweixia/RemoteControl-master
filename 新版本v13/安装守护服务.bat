@echo off
chcp 936 >nul
cd /d "%~dp0"
title 安装 RemoteControl 守护服务
net session >nul 2>&1
if errorlevel 1 (
  echo 需要管理员权限, 正在重新以管理员身份启动...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
echo ============================================================
echo   安装 RemoteControl 守护服务 (开机自启 / 崩溃自动拉起)
echo ============================================================
echo.
if not exist "%~dp0RCSvc.exe"         ( echo [X] 找不到 RCSvc.exe, 请确认文件完整 & pause & exit /b 1 )
if not exist "%~dp0RemoteControl.exe" ( echo [X] 找不到 RemoteControl.exe & pause & exit /b 1 )

echo [1/6] 停止旧的守护服务(如有)...
sc stop RemoteControlService >nul 2>&1
ping -n 2 127.0.0.1 >nul

echo [2/6] 安装并启动守护服务...
"%~dp0RCSvc.exe" --install

echo.
echo [3/6] 服务状态:
sc query RemoteControlService
echo.
sc qc RemoteControlService

echo.
echo [4/6] 写入注册表 Run 键(用户级兜底)...
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v RemoteControlAgent /t REG_SZ /d "\"%~dp0RemoteControl.exe\" --port 9999 --backup" /f

echo.
echo [5/6] 立即启动被控端(当前会话)...
tasklist /fi "imagename eq RemoteControl.exe" | find /i "RemoteControl.exe" >nul
if errorlevel 1 (
  start "" "%~dp0RemoteControl.exe" --port 9999
  echo     已启动
) else (
  echo     已有被控端在运行, 跳过
)

echo.
echo [6/6] 检查 9999 端口监听:
ping -n 3 127.0.0.1 >nul
netstat -ano | findstr ":9999" | findstr LISTENING

echo.
echo ============================================================
echo  完成! 之后即使不登录 Windows, 服务也会每 5 秒把被控端拉起。
echo.
echo  提示: 本机 IP (控制端填这个):
ipconfig | findstr /i "IPv4"
echo.
echo  卸载: 运行 卸载守护服务.bat
echo ============================================================
pause
