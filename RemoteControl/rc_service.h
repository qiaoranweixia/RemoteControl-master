/* rc_service.h — 被控端内置守护服务 (v17)
   让 RemoteControl.exe 一个文件同时具备: 被控端 + 开机自启守护服务。
   双击 exe 即自动完成部署, 不需要任何 .bat/.ps1 辅助。 */
#pragma once

/* 服务是否已注册 */
bool RCService_IsInstalled();
/* 安装(或更新)并启动服务; 需管理员权限。返回是否成功 */
bool RCService_Install(const char* agentExe, int port);
/* v18: 服务是否已存在 且 ImagePath 指向给定 exe(读不到配置时保守返回 true) */
bool RCService_PointsTo(const char* agentExe);
/* 停止并删除服务; 需管理员权限 */
bool RCService_Remove();
/* 以服务身份运行(阻塞)。由 wWinMain 在收到 --svc 时调用 */
bool RCService_Entry(const char* agentExe, int port);
/* 写服务日志(与 agent 日志分开, 便于排查) */
void RCService_SetLogDir(const char* dir);
