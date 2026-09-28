# RemoteControl v2 使用说明（快速上手）

> 本文档面向“拿到 exe 就要用”的场景。设计与实现细节见 [FEATURES_PLAN.md](FEATURES_PLAN.md) 与 [../README.md](../README.md)。

---

## 0. 可执行文件在哪

| 角色 | 说明 | 路径 |
|---|---|---|
| **被控端**（Agent，跑在被控制的那台机器上） | 后台静默运行，无窗口无控制台，负责抓屏 + 执行鼠标键盘 + 收发文件 | `F:\cpp interview\RemoteControl-master\RemoteControl\x64\Debug\RemoteControl.exe` |
| **控制端**（Client，跑在你自己这台机器上） | 有 GUI：画面 + 文件管理 + 日志 | `F:\cpp interview\RemoteControl-master\client\x64\Debug\client.exe` |

> **Release 版（推荐用于给别人测试/拷到别的机器）** 也已构建好：
> `...\RemoteControl\x64\Release\RemoteControl.exe`、`...\client\x64\Release\client.exe`（同目录下若缺 MSVC 运行库，需装 VC++ 2015-2022 x64 运行库；本机已装 VS 直接用）。

源码根目录：`F:\cpp interview\RemoteControl-master\`（`RemoteControl\` 为被控端工程，`client\` 为控制端工程，`common\protocol.h` 为公共协议）。

---

## 1. 一分钟上手（本机双开自测）

1. **启动被控端**（推荐先限制文件沙箱范围，便于测试）：

   ```bat
   cd /d "F:\cpp interview\RemoteControl-master\RemoteControl\x64\Debug"
   RemoteControl.exe --port 9999 --root "D:\Share" --log agent.log
   ```

   > 双击运行也一样，默认端口 `9999`。启动后**不会出现任何窗口**，属于正常现象（后台静默）；确认是否在跑：任务管理器里能看到 `RemoteControl.exe`，或看 `agent.log` 里的 `agent listening on 0.0.0.0:9999`。

2. **启动控制端**：双击 `client.exe`（也可带参：`client.exe 127.0.0.1 9999`）。

3. 在控制端窗口里：
   - **IP** 填被控端地址（本机测就用 `127.0.0.1`），**端口**填 `9999`；
   - 点 **连接**，状态栏显示 `已连接 127.0.0.1:9999 [自动重连开启]`；
   - 左侧出现远程画面，右侧文件列表自动列出被控端 `D:\Share` 的内容（地址栏会自动被同步成被控端的起始目录）。

4. **跨机器使用**：被控端 IP 换成对方机器的局域网 IP；Windows 防火墙需放行该端口（首次运行选“允许访问”或手动入站规则放行 TCP 9999）。

---

## 2. 被控端命令行参数

| 参数 | 含义 | 默认 |
|---|---|---|
| `--port <n>` | 监听端口 | `9999` |
| `--root <dir>` | 文件传输沙箱根目录（**只允许在此目录内浏览/上传/下载/改名/删除**） | 空 = 不做限制（全盘可访问） |
| `--log <file>` | 日志文件路径 | 空 = 不落盘 |
| `--console` | 调试用：显示控制台窗口并输出日志 | 关闭（静默） |
| `--install` | 注册开机自启（写入 `HKCU\...\Run`，值为 `"<exe>" --port 9999`） | — |
| `--uninstall` | 取消开机自启 | — |
| `--stop` | 让正在运行的实例退出（配合 `--port` 指定端口，默认 9999） | — |

示例：

```bat
RemoteControl.exe --port 8888 --root "E:\共享" --log "D:\rc.log"
RemoteControl.exe --stop --port 8888      & rem 停止 8888 端口上的实例
RemoteControl.exe --install               & rem 开机自启
```

**单实例规则**：同一端口只允许一个实例（第二个实例会静默退出并在日志写 `another instance is running on port N`）；**不同端口可以并存**，便于多机/多实例调试。

---

## 3. 控制端界面说明

| 区域 | 控件 | 说明 |
|---|---|---|
| 顶部 | IP / 端口 / **连接** / **断开** | 连接目标；断开后若未手点“断开”会自动重连 |
| 顶部 | 帧率 / 编码下拉框 | 调节画面帧率与编码（PNG/JPEG），降低带宽占用 |
| 左侧大区 | **远程画面** | 鼠标移动/左右中键/双击、键盘按键都会实时发到被控端执行；坐标自适应缩放，支持高 DPI |
| 右侧 | 路径框 + **刷新** / **上一级** | 当前被控端目录；双击列表中的目录进入，`[目录]` 前缀即为目录 |
| 右侧 | 文件列表 | 显示名称/大小/时间 |
| 右侧 | **下载** / **上传** | 下载：选中文件 → 点下载 → 选择本机保存位置；上传：点上传 → 选择本机文件 → 传到当前远程目录 |
| 右侧 | 名称框 + **新建目录** / **重命名** / **删除** | 重命名/删除需先在列表选中目标 |
| 底部 | 日志框 | 连接、文件传输进度（含大小与耗时）、错误信息 |

**焦点说明**：远程键盘/鼠标只有在被控端“画面窗口为前台窗口”时才生效（等同于你人在那台机器上操作）。想打字时先用鼠标点一下远端目标窗口（例如记事本、搜索框）即可；被控端若停在锁屏/登录界面则无法接收输入。

---

## 4. 文件传输说明

- 传输走同一条 TCP 连接的独立命令（`CMD_FILE_*`），64 KB 分块 + 偏移量，支持大文件；上传/下载结束后控制端日志会打印字节数与耗时。
- 若被控端用 `--root` 限定沙箱：
  - 地址栏里输入的路径必须位于沙箱内，否则日志显示 `目录读取失败: 路径越权(...)`；
  - 含 `..` 的路径一律拒绝；
  - 想访问沙箱外的内容，需要重启被控端并调整/去掉 `--root`。
- 上传的目标文件名取自本机所选文件的文件名，落在当前远程目录。

---

## 5. 自测脚本（可选）

仓库外的测试脚本位于 `F:\cpp interview\_build\`：

| 脚本 | 覆盖内容 | 运行 |
|---|---|---|
| `smoke_test.py` | 19 项协议级端到端：屏幕抓取、文件列表(含 `--root` 沙箱根解析)、新建/重命名/删除、200 KB 分块上传与下载(md5 校验)、越权路径拦截、心跳、同端口单实例互斥、跨端口并存 | `python -X utf8 smoke_test.py 9988` |
| `gui_test.py` | 5 项 GUI 级：真实点“连接”后校验地址栏=沙箱根、文件列表已填充、状态栏=已连接、日志=连接成功 | `python -X utf8 gui_test.py` |

两者都会自己拉起/关闭被控端与控制端进程，不会影响你正在运行的程序（端口不同）。

最近一次结果（2026-09-17）：**smoke_test 19/19 通过，gui_test 5/5 通过**。

---

## 6. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 双击被控端“没反应” | 正常：它是无窗口后台程序。看任务管理器 / 日志确认 |
| 控制端一直连不上 | ① 被控端没启动或被防火墙拦截（放行 TCP 端口）；② IP/端口填错；③ 被控端日志有 `another instance is running on port N`（换端口或先 `--stop`） |
| 画面卡顿/流量大 | 降低帧率、切 JPEG 编码；或缩小远程桌面分辨率 |
| 点了远端但键盘没反应 | 先用鼠标点击远端目标窗口使其获得焦点；确认被控端未锁屏 |
| 文件列表报“路径越权” | 被控端配了 `--root` 沙箱，地址栏路径不在沙箱内 |
| 上传/新建目录没生效 | 日志会给出原因（无权限、路径不存在、越权等）；只读目录写入会失败 |
| 想只用一次不要开机自启 | 不要执行 `--install`；如已执行，用 `--uninstall` 撤销 |

---

## 7. 安全提示 / 已知限制

- **默认无认证、无加密**：任何能连上该端口的人都能控制画面与文件，请仅在可信局域网/自测环境使用；对外暴露需自行加认证与 TLS（属于后续规划项，见 `FEATURES_PLAN.md`）。
- 键鼠注入依赖被控端会话为已登录的活动桌面会话（服务/会话 0 下不可用）。
- 仅支持单控制端连接：新的连接会替换旧会话。
- `--root` 为唯一访问控制手段，去掉即等于开放全盘。

---

## 8. 重新编译

一键脚本（VS2022，x64，Debug）：

```bat
F:\cpp interview\_build\b_server.bat   & rem 编译被控端(Debug) -> RemoteControl\x64\Debug\RemoteControl.exe
F:\cpp interview\_build\b_client.bat   & rem 编译控制端(Debug) -> client\x64\Debug\client.exe
F:\cpp interview\_build\b_all_release.bat & rem 一次性编译两端 Release 版
```

或手动（已在 IDE/命令行把工具集统一为 v143，VS2022 可直接编译 Release）：

```bat
call "D:\vs2022\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "F:\cpp interview\RemoteControl-master"
msbuild RemoteControl\RemoteControl.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild client\client.vcxproj            /p:Configuration=Debug /p:Platform=x64
```
