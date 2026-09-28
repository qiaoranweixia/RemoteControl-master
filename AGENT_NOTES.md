# 构建 / 改动备忘 (2026-09-19)

## 构建
- 命令行编译（VS2022 在 D:\vs2022）:
  - 服务端: `call "D:\vs2022\VC\Auxiliary\Build\vcvarsall.bat" x64` → `MSBuild "RemoteControl\RemoteControl.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:n /m`
  - 客户端: 同上，改 `client\client.vcxproj`
- 产物:
  - 被控端 `RemoteControl\x64\Release\RemoteControl.exe`
  - 控制端 `client\x64\Release\client.exe`
  - 发布副本目录: `deliver\`（两个 exe + 中文名副本）
- 源码必须保持 **UTF-8 BOM**；校验 exe 里的中文串要按 **GBK** 搜索（MSVC 会把窄字面量编成 GBK）。
- 链接失败多半是旧 exe 还在运行占用文件 → 先结束同名进程再编译。

## 2026-09-19 的改动
1. **修复致命崩溃（被控端启动 3~5 秒必崩，0xC0000005 @ntdll）**
   原因: `wWinMain` 里 `Log("=== agent start ===")` 写在了 `InitializeCriticalSection(&g_log_lock)` 之前，
   未初始化的 CRITICAL_SECTION 上 EnterCriticalSection → ntdll 崩溃。
   现在: 先初始化 g_send_lock / g_log_lock，再打日志。
2. **新实例自动接管旧实例**（同端口的旧被控端在跑时，新被控端不再直接退出）
   - 流程: 检测 mutex 冲突 → 发 stop 事件让旧实例优雅退出 → 轮询 4s → 仍冲突则枚举同名进程强杀（排除自身 PID）→ 继续监听。
   - 新参数 `--keep-old`: 保持旧行为（发现同端口实例就直接退出）。
   - 启动时会把注册表自启动项路径刷新为当前 exe 路径。
   - 日志证据: `another instance on port N detected, taking over` → `stop signal sent` → `takeover done: old agent replaced`。
3. 已知缺口: 摄像头（CMD_CAMERA）在被控端有采集（camera_capture.cpp），但**控制端 client.cpp 没有收包/显示通路**，控制端看不到摄像头画面。

## 常用命令行参数（被控端）
- `--port N` 端口；`--log <file>` 日志文件；`--root <dir>` 限制文件访问根
- `--install` / `--uninstall` 开机自启；`--stop` 结束同端口实例；`--console` 控制台模式
- `--keep-old` 不接管旧实例
