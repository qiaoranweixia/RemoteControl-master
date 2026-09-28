# 一键修复开机自启(被控端) —— 以管理员身份运行
$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "RemoteControl.exe"
Write-Host "被控端程序: $exe"
if (-not (Test-Path $exe)) { Write-Host "[错误] 当前目录下找不到 RemoteControl.exe" -ForegroundColor Red; Read-Host "回车退出"; exit 1 }

Write-Host "[1] 停掉正在运行的旧实例 ..."
Get-Process RemoteControl -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

Write-Host "[2] 重建计划任务(登录时自动启动, 最高权限, 无时间限制) ..."
$act = New-ScheduledTaskAction -Execute $exe -Argument "--port 9999"
$trg = New-ScheduledTaskTrigger -AtLogOn
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
try { Unregister-ScheduledTask -TaskName "RemoteControlAgent" -Confirm:$false -ErrorAction SilentlyContinue } catch {}
Register-ScheduledTask -TaskName "RemoteControlAgent" -Action $act -Trigger $trg -Settings $set -RunLevel Highest -Force | Out-Null
Write-Host "    [OK] 任务已重建" -ForegroundColor Green

Write-Host "[2.5] 清理注册表 Run 里的旧格式自启项(新格式由被控端自己写, 带 --backup 标记, 不会抢占端口) ..."
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if (Test-Path $runKey) { Remove-ItemProperty -Path $runKey -Name "RemoteControlAgent" -ErrorAction SilentlyContinue }
Write-Host "    [OK] 已清理(若原本就没有则忽略)" -ForegroundColor Green

Write-Host "[3] 立即启动被控端 ..."
Start-ScheduledTask -TaskName "RemoteControlAgent"
Start-Sleep -Seconds 6

Write-Host "[4] 检查结果 ..."
Write-Host "---- 本机 IPv4 (控制端填这个 + 端口 9999) ----"
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike "169.*" } | ForEach-Object { Write-Host ("    " + $_.IPAddress) }
$listen = Get-NetTCPConnection -LocalPort 9999 -State Listen -ErrorAction SilentlyContinue
$proc = Get-Process RemoteControl -ErrorAction SilentlyContinue
if ($proc) { Write-Host ("[OK] 被控端进程在运行 PID=" + ($proc.Id -join ",")) -ForegroundColor Green } else { Write-Host "[失败] 没有被控端进程!" -ForegroundColor Red }
if ($listen) { Write-Host "[OK] 9999 端口正在监听 - 控制端可以连了" -ForegroundColor Green } else { Write-Host "[失败] 9999 没有监听" -ForegroundColor Red }
$i = Get-ScheduledTaskInfo -TaskName "RemoteControlAgent"
Write-Host ("任务状态: " + (Get-ScheduledTask -TaskName "RemoteControlAgent").State + " / 上次结果=" + $i.LastTaskResult)
Write-Host ""
Write-Host "[重要] 该任务是""登录时""触发: 电脑重启后必须登录一次 Windows 桌面, 被控端才会起来。" -ForegroundColor Yellow
Write-Host "       想重启后不登录也能连, 请在【这台电脑】上执行 netplwiz 取消""要使用本计算机,用户必须输入用户名和密码""" -ForegroundColor Yellow
Read-Host "回车退出"
