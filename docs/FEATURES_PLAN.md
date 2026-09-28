# RemoteControl 功能规划 (v2 路线图)

> 现状：仅支持「屏幕回传 + 鼠标/键盘控制」，单客户端、硬编码 IP、无拆包/心跳/认证、被控端无法静默。
> 下文给出功能清单、优先级、设计与验收标准。标 `[已实现]` 的为本轮已提交代码。

---

## A. 被控端后台静默（P0）`[已实现]`

| 子项 | 做法 | 验收 |
|---|---|---|
| 无窗口无控制台 | 链接期 `/SUBSYSTEM:WINDOWS` + 入口 `wWinMain`，不创建任何窗口 | 双击后任务栏/Alt-Tab 无窗口，仅进程存在 |
| 日志落盘 | `%ProgramData%\RemoteControl\agent.log`（无权限回退 `%TEMP%`），带时间戳+线程号 | 断连/异常均有记录 |
| 单实例互斥 | `CreateMutex("Global\\RemoteControlAgent_Mutex")` | 重复启动第二个进程静默退出 |
| 开机自启 | `HKCU\...\CurrentVersion\Run`，`--install / --uninstall` | 重启后自动上线 |
| 后台停止 | 命名事件 `Global\RemoteControlAgentStop`，`--stop` 触发 | 无需任务管理器即可优雅退出 |
| 断线自动等待 | `accept` 循环 + 会话结束回到监听 | 控制端断开后可立即重连 |
| 保活 | TCP KeepAlive + 应用层 `CMD_HEARTBEAT` + 30s 空闲超时 | 拔网线 30s 内释放会话 |
| 参数化 | `--port/--log/--console/--root` | 不再硬编码 9999 |
| 后续可选 | Windows 服务化(SCM)、托盘图标、看门狗自恢复、进程名伪装 | 见 Phase 3 |

## B. 文件传输（P0）`[已实现]`

- 协议：`CMD_FILE_LIST / FILE_LIST_RES / FILE_DOWNLOAD / FILE_UPLOAD_BEGIN / FILE_DATA / FILE_END / FILE_OP`
- 结构：`FileChunk{file_id, offset, data_len, flags}` + `FileRequest{file_id, offset, file_size, path[520]}`
- 64KB 分块流式收发（**不整文件进内存**），`file_id` 支持并发，`offset` 天然支持断点续传
- 能力：远程目录浏览、下载、上传、删除、新建文件夹、重命名
- 安全：`--root` 限定可操作根目录，拒绝 `..` 越权路径
- 体验：传输期间暂停屏幕请求（把带宽让给文件）、进度回调、完成后自动刷新列表
- 后续可选：传输队列、限速、压缩、多文件打包、断点续传 UI

## C. 协议与健壮性（P0）`[已实现]`

- 抽出 `common/protocol.h` 双端共用，协议单点维护
- `PacketStream` 流式拆包：解决「一次 recv ≠ 一个包」的粘包/半包，PNG 内出现魔数也不误判
- `SendAll` 循环发送：解决 `send` 部分发送导致的数据损坏
- 全局发送锁：屏幕线程与文件线程并发 `send` 不交叉
- 修复键盘只发 KEYDOWN 的「按键卡住」缺陷（补 `WM_KEYUP`，`dwFlags=KEYEVENTF_KEYUP`）
- 修复鼠标移动无节流（20ms/5px 阈值，且保留原始动作全部 13 种）

## D. 会话与体验（P1）
| 功能 | 说明 |
|---|---|
| 断线重连 | 控制端自动重连 + UI 状态提示（本轮已含基础重连） |
| 多客户端并发 | 被控端一对多，屏幕按需分发 |
| 连接认证 | 口令质询-响应（challenge/HMAC），失败锁定 |
| 传输加密 | AES-256 流加密或 Schannel(TLS) |
| IP 白名单 | 仅允许指定网段连接 |
| 地址簿/多机管理 | 控制端主机列表、分组、备注 |

## E. 画面质量与性能（P1）
- 帧率/画质动态调节（`ScreenCtrl{fps, quality, encode, monitor}`，本轮已预留字段与 JPEG 通路）
- 脏矩形/差分编码，只传变化区域
- DXGI Desktop Duplication 高速采集（替代 GDI BitBlt）
- 带宽自适应：按 RTT/丢包动态降质
- 多显示器枚举与切换

## F. 系统管理（P2）
- 远程 Shell（`CMD_SHELL_EXEC` → `CMD_SHELL_RESULT`）
- 系统信息（CPU/内存/磁盘/OS/网络）
- 进程管理（列表/结束）、服务管理、启动项管理
- 电源控制（关机/重启/注销/锁屏/休眠）、音量/静音
- 远程注册表、文件搜索

## G. 多媒体与进阶（P3）
- 剪贴板同步（文本/图片，防回环）
- 屏幕录制与回放（会话审计）
- 摄像头采集、麦克风/系统声音传输
- 中继/穿透（中转服务器 + NAT 打洞）
- 多控制端协同（屏幕共享会话）

---

## 实施阶段
1. **Phase 1（本轮完成）**：后台静默 + 文件传输 + 协议健壮性 + 心跳重连
2. **Phase 2**：认证与加密、远程 Shell/系统信息/进程管理、画质帧率调节、多显示器
3. **Phase 3**：服务化(SCM)、托盘与看门狗、多客户端并发、地址簿
4. **Phase 4**：剪贴板、录制审计、音频、中继穿透

## 使用示例
```
# 被控端（静默）
RemoteControl.exe --port 9999
RemoteControl.exe --install        # 注册开机自启
RemoteControl.exe --stop           # 停止后台实例
RemoteControl.exe --console        # 调试：带控制台运行

# 控制端
client.exe 192.168.3.53 9999       # 默认 127.0.0.1:9999
```

---

## 验收记录（2026-09-17）

### 自动化测试

| 测试 | 覆盖 | 结果 |
|---|---|---|
| `F:\cpp interview\_build\smoke_test.py` | 19 项协议级：屏幕抓取、文件列表(空路径→沙箱根 `@path` 回传)、绝对路径一致、新建/重命名/删除目录、200 KB 分块上传(md5)、下载(md5)、沙箱外路径拦截、删除、心跳、无 unknown cmd、同端口单实例互斥、跨端口并存、断开后回到监听 | **19/19 PASS**（Debug 与 Release 各一次） |
| `F:\cpp interview\_build\gui_test.py` | 5 项 GUI 级：控制端窗口存在、真实点“连接”后地址栏=被控端起始目录、文件列表已填充(4 项)、状态栏=已连接、日志=连接成功 | **5/5 PASS**（Debug 与 Release 各一次） |
| `key_test*.py` / `k6test.py` | 键鼠注入链路：`SendInput` 键鼠由被控端真实执行（CapsLock 系统状态翻转作证；鼠标注入控件级验证），被控端记录无异常 | 通过 |

### 本轮测试驱动修复的问题

1. **沙箱根无法浏览**：被控端配置 `--root` 后，控制端初始路径 `C:\`（以及空路径）被 `PathAllowed` 判为越权，文件区直接报 `err:路径越权`。
   修复：被控端空路径解析为 `--root` 沙箱根；列表响应首行新增 `@path\t<规范化绝对路径>`，控制端据此同步地址栏与后续相对路径基准；控制端地址栏不再兜底 `C:\`。
2. **单实例锁与多端口冲突**：互斥体/停止事件使用全局名字，导致不同端口也无法并存（第二个实例静默退出）。
   修复：互斥体与停止事件按端口命名（`RemoteControlAgent_Mutex_<port>` / `..._Stop_<port>`），`--stop --port N` 只作用于该端口实例；保留旧名字作为兼容回退。
3. **工程配置**：`client.vcxproj` 的 Release(及 Win32) 配置子系统为 `Console`，与 `WinMain` 入口不符导致 Release 链接失败；全部改为 `Windows + WinMainCRTStartup`。两工程中未安装的 `v145` 工具集统一为 `v143`，使 VS2022 可直接编译 Release。

### 产物

- 被控端：`RemoteControl\x64\{Debug,Release}\RemoteControl.exe`
- 控制端：`client\x64\{Debug,Release}\client.exe`

使用说明见 [USAGE.md](USAGE.md)。
