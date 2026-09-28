@echo off
chcp 936 >nul
title 被控端状态诊断
echo ==================================================
echo            被控端状态诊断（本机）
echo ==================================================
echo.
echo [1] 被控端进程:
tasklist /fi "imagename eq RemoteControl.exe" | findstr /i "RemoteControl.exe" || echo     没有! 被控端没在运行
echo.
echo [2] 9999 端口监听:
netstat -ano | findstr ":9999" | findstr LISTENING || echo     没有! 端口未监听
echo.
echo [3] 本机 IPv4 (控制端应连这个):
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do echo     %%a
echo.
echo [4] 开机自启任务:
schtasks /query /tn RemoteControlAgent /v /fo LIST 2>nul | findstr /c:"状态" /c:"上次运行时间" /c:"上次结果" /c:"计划类型" /c:"要运行的任务" || echo     任务不存在（请用"一键修复开机自启.bat"重建）
echo.
echo [5] 被控端日志最后 15 行:
powershell -NoProfile -Command "if (Test-Path 'C:\ProgramData\RemoteControl\agent.log') { Get-Content 'C:\ProgramData\RemoteControl\agent.log' -Tail 15 } else { '（还没有日志文件，说明被控端从未在这台电脑上成功启动过）' }"
echo.
pause
