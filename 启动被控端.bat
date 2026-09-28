@echo off
chcp 936 >nul
cd /d "%~dp0"
echo ============================================
echo   启动被控端 (RemoteControl Agent)
echo ============================================
echo.
echo [本机 IPv4 地址]  控制端就填下面其中之一 + 端口 9999:
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do echo     %%a
echo.
echo 接下来会弹出 UAC 授权窗口, 请点"是" !!
echo.
powershell -NoProfile -Command "Start-Process -FilePath '%~dp0RemoteControl.exe' -ArgumentList '--port 9999' -Verb RunAs; Start-Sleep -Seconds 4; $p = Get-Process RemoteControl -ErrorAction SilentlyContinue; if ($p) { Write-Host ('[成功] 被控端已在运行, PID=' + $p.Id) -ForegroundColor Green } else { Write-Host '[失败] 没检测到被控端进程 - UAC 被取消? 或被杀软拦截?' -ForegroundColor Red }; Get-NetTCPConnection -LocalPort 9999 -State Listen -ErrorAction SilentlyContinue | Out-Null; if (Get-NetTCPConnection -LocalPort 9999 -State Listen -ErrorAction SilentlyContinue) { Write-Host '[监听] 9999 端口已在监听, 可以从别的电脑连了' -ForegroundColor Green }"
echo.
echo 提示: 被控端是无界面静默程序, 看不到窗口是正常的。
echo       任务管理器里能看到 RemoteControl.exe 就说明它在工作。
echo.
pause
