# 安卓被控端 APK 集成笔记 (2026-09-27)

## 目标
控制端(client.exe)界面上能“一键生成”安卓被控端 APK；被控端 exe 侧已完成开机自启。

## Android 工程位置
- 源码/打包脚本: `F:\cpp interview\GenericAgent-main\temp\android\`
  - `AndroidManifest.xml`, `res/`, `src/com/rcagent/*.java`, `build.py`, `rc.keystore`(密码 rc123456, alias rcagent)
  - 产物: `temp\android\RCAndroidAgent.apk` (20952 B, 已签名 CN=RC Agent)
- 内嵌数据头(客户端用): `RemoteControl-master\client\android_apk_data.h` (由 `temp\android\embed_apk.py` 生成)

## 工具链(本机无 Gradle, 手工打包)
JDK = `D:\andorid\jbr\bin` (javac/keytool 21.0.5)
aapt2/d8/zipalign/apksigner = `F:\cpp interview\GenericAgent-main\temp\tools\bt` (+ `lib\d8.jar`, `lib\apksigner.jar`)
android.jar = `E:\Android\android-sdk\platforms\android-34\android.jar`
流程: aapt2 compile → aapt2 link(--java 生成 R.java) → javac → d8 → zipalign → apksigner sign

## 两个致命坑(已解决, 别踩回去)
1. **d8 8.2.2 + JDK21: class 只要含 synthetic `this$0` 字段就 NPE**(Compilation failed with an internal error,
   `String.length() because <parameter1> is null`), 与 javac 版本/`-g` 无关。
   → Android 源码里**禁止匿名内部类和非静态内部类**, 线程体一律写成静态嵌套类 + 显式持有外部引用。
   → `build.py` 已加自动校验(扫 class 里的 `this$0`, 命中即退出)。
2. `d8.bat` / `apksigner.bat` 用 `%params%` 传参, 遇**含空格路径**会被拆断。
   → 必须 `java -cp lib\d8.jar com.android.tools.r8.D8 ...` / `java -jar lib\apksigner.jar ...` 直调。
3. 另外: javac `@argfile` 里含空格路径必须加引号且反斜杠双写。

## 客户端集成(待办/进行中)
- 按钮: 工具页新增 `IDC_BTN_GENAPK`(“生成安卓被控端”)→ 保存对话框 → 写出内嵌 APK
- 命令行: `apk` / `androidapk` 子命令(供控制台与 headless 自测)
- 生成时把当前 `g_ip`/`g_port` 以注释形式写进同名 .txt 便于用户对照

## 验证方式
- `build.py` 输出 “=== APK 打包完成 ===” + apksigner verify 证书信息
- 客户端编译后 headless: `client.exe --dev "apk <路径>"` 应生成与源 APK 字节一致的副本
