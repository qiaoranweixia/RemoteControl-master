@echo off
chcp 936 >nul
cd /d "%~dp0"
echo ============================================
echo   RemoteControl v13 一键替换（优化版）
echo ============================================
echo.
echo [1/3] 关闭旧的控制端 / 被控端进程 ...
taskkill /f /im client.exe >nul 2>&1
taskkill /f /im RemoteControl.exe >nul 2>&1
timeout /t 2 >nul

echo [2/3] 覆盖到正式目录 ...
copy /y "client.exe"          "F:\cpp interview\RemoteControl-master\client\x64\Release\client.exe" >nul
if errorlevel 1 (echo    [失败] 控制端复制失败 - 控制端可能还在运行) else (echo    [OK] 控制端已更新)
copy /y "RemoteControl.exe"   "F:\cpp interview\RemoteControl-master\RemoteControl\x64\Release\RemoteControl.exe" >nul
if errorlevel 1 (echo    [失败] 被控端复制失败 - 请右键本脚本"以管理员身份运行") else (echo    [OK] 被控端已更新)

echo [3/3] 完成。
echo.
echo 提示：本脚本只更新了"这台电脑"上的两份 exe。
echo       被控端的黑屏/卡顿修复在【被控的那台电脑】上，
echo       请把 RemoteControl.exe 拷到那台电脑替换后再启动。
echo.
pause
