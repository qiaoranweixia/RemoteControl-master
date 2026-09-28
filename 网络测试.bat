@echo off
chcp 936 >nul
echo ==================== 一、本机 IP ====================
ipconfig | findstr /c:"IPv4"
echo.
echo ================== 二、测试到被控端 ================
set /p TARGET=请输入被控端 IP (例如 192.168.0.105)，直接回车跳过: 
if not "%TARGET%"=="" (
  echo.
  echo [1] ping %TARGET%
  ping -n 2 -w 1000 %TARGET%
  echo.
  echo [2] 测试 TCP 9999 端口...
  powershell -NoProfile -Command "$r = Test-NetConnection -ComputerName %TARGET% -Port 9999 -WarningAction SilentlyContinue; if ($r.TcpTestSucceeded) { Write-Host '  [OK] 9999 端口可以连通 -> 被控端在运行, 控制端直接填这个 IP 即可' -ForegroundColor Green } else { Write-Host '  [X] 9999 端口连不上 -> 那台电脑没开机/没联网, 或没启动被控端' -ForegroundColor Red }"
)
echo.
pause
