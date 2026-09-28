/* ============================================================================
   RCSvc.cpp  —  RemoteControl 被控端「守护服务」 (v1, 2026-09-27)

   解决什么:
     计划任务只在「用户登录的那一瞬间」触发一次。机器重启后若无人登录、
     或者 agent 被任务管理器/杀软结束掉，就再也没人把它拉起来 —— 于是
     "重启后连不上"。

   这个服务做什么:
     1) 注册成 Windows 服务, 启动类型 = 自动, 账户 = LocalSystem
        → 开机(不等任何用户登录)就运行。这是计划任务天然做不到的。
     2) 主循环每 5 秒:
          session = WTSGetActiveConsoleSessionId()      // 当前有人用的会话
          若该会话里已经有 RemoteControl.exe 在跑 → 什么都不做
          否则 WTSQueryUserToken(session) 拿到用户令牌,
               CreateProcessAsUser 把 agent 投递到【用户的会话】里启动。
        (服务本身在 session 0, 它自己启动的进程抓不到桌面、发不了键鼠,
         所以必须用 CreateProcessAsUser 把 agent 放进交互式会话。)
     3) 拉起的 agent 一律带 --backup:
          → 只做兜底。发现 9999 已被占(说明"最高权限"实例已由计划任务拉起)
            就安静退出, 绝不抢占/接管。两层实例因此不会互相打架。

   结果 = 四层保险:
     1) 本服务(LocalSystem, 开机即起, 崩了 5 秒内拉回)       <- 无条件保底
     2) 计划任务(登录触发 + 最高权限)                          <- 主, 能操作提权窗口
     3) HKCU\Run(用户层兜底)
     4) 手动双击

   用法:
     RCSvc.exe --install     安装并启动服务(需管理员)
     RCSvc.exe --uninstall   停止并删除服务(需管理员)
     RCSvc.exe --run         前台跑守护循环(调试用)
     RCSvc.exe               由 SCM 调用(不要手动双击)
   ============================================================================ */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <string>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

#define SVC_NAME    "RemoteControlService"
#define SVC_DISPLAY "RemoteControl Agent Watchdog"
#define AGENT_EXE   "RemoteControl.exe"
#define AGENT_ARGS  "--port 9999 --backup"   /* --backup: 只兜底, 不抢端口 */
#define POLL_MS     5000

static SERVICE_STATUS        g_ss;
static SERVICE_STATUS_HANDLE g_ssh   = NULL;
static volatile LONG         g_stop  = 0;
static char                  g_dir[MAX_PATH]   = { 0 };   /* agent 所在目录 */
static char                  g_agent[MAX_PATH] = { 0 };   /* agent 完整路径 */
static char                  g_log[MAX_PATH]   = { 0 };

/* ------------------------------ 日志 ------------------------------ */
static void SvcLog(const char* fmt, ...)
{
	char line[1024] = { 0 };
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
		"[%04d-%02d-%02d %02d:%02d:%02d] ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	if (n < 0) n = 0;
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	size_t len = strlen(line);
	if (len + 2 < sizeof(line)) { line[len] = '\r'; line[len + 1] = '\n'; line[len + 2] = 0; }
	FILE* fp = NULL;
	if (fopen_s(&fp, g_log, "ab") == 0 && fp != NULL) { fwrite(line, 1, strlen(line), fp); fclose(fp); }
}

/* --------------------- 某个会话里 agent 是否在跑 --------------------- */
static bool AgentRunningInSession(DWORD session)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32 pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
	bool found = false;
	if (Process32First(snap, &pe))
	{
		do {
			if (_stricmp(pe.szExeFile, AGENT_EXE) == 0)
			{
				DWORD s = 0;
				if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session) { found = true; break; }
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return found;
}

/* ----------------- 把 agent 投递到交互式会话里启动 ----------------- */
static void EnsureAgent()
{
	DWORD session = WTSGetActiveConsoleSessionId();
	if (session == 0xFFFFFFFF) return;             /* 没有活动的控制台会话(无人登录) */

	if (AgentRunningInSession(session)) return;    /* 已在线, 不重复启动 */

	HANDLE tok = NULL;
	if (!WTSQueryUserToken(session, &tok) || tok == NULL)
	{
		static DWORD lastErr = 0;
		DWORD e = GetLastError();
		if (e != lastErr) { lastErr = e; SvcLog("WTSQueryUserToken(session=%u) failed: %lu (通常=无人真正登录)", session, e); }
		return;
	}

	char cmd[MAX_PATH * 2];
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" %s", g_agent, AGENT_ARGS);

	STARTUPINFOA si; memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.lpDesktop = (char*)"winsta0\\default";      /* 关键: 放进交互式桌面 */
	PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

	void* env = NULL;
	CreateEnvironmentBlock(&env, tok, FALSE);

	BOOL ok = CreateProcessAsUserA(tok, NULL, cmd, NULL, NULL, FALSE,
		CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
		env, g_dir, &si, &pi);

	DWORD err = GetLastError();
	if (env) DestroyEnvironmentBlock(env);
	CloseHandle(tok);

	if (ok)
	{
		SvcLog("agent launched in session %u (pid %u): %s", session, pi.dwProcessId, cmd);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	else
	{
		SvcLog("CreateProcessAsUser failed: %lu  (session=%u, cmd=%s)", err, session, cmd);
	}
}

/* ------------------------------ 守护循环 ------------------------------ */
static DWORD WINAPI Worker(LPVOID)
{
	SvcLog("watchdog started (poll=%dms, agent=%s, args=%s)", POLL_MS, g_agent, AGENT_ARGS);
	while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
	{
		EnsureAgent();
		for (int i = 0; i < POLL_MS / 100 && InterlockedCompareExchange(&g_stop, 0, 0) == 0; i++)
			Sleep(100);
	}
	SvcLog("watchdog stopped");
	return 0;
}

/* ------------------------------ SCM 回调 ------------------------------ */
static void ReportStatus(DWORD state, DWORD exitCode, DWORD waitHint)
{
	g_ss.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
	g_ss.dwCurrentState     = state;
	g_ss.dwWin32ExitCode    = exitCode;
	g_ss.dwWaitHint         = waitHint;
	g_ss.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0
	                        : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
	g_ss.dwCheckPoint       = 0;
	if (g_ssh) SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI CtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
	switch (ctrl)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
		InterlockedExchange(&g_stop, 1);
		return NO_ERROR;
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

static void WINAPI ServiceMain(DWORD, LPSTR*)
{
	g_ssh = RegisterServiceCtrlHandlerExA(SVC_NAME, CtrlHandler, NULL);
	if (g_ssh == NULL) return;
	ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
	ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
	Worker(NULL);
	ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

/* --------------------------- 安装 / 卸载 --------------------------- */
static bool InstallService()
{
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (scm == NULL) { printf("OpenSCManager failed: %lu  (请以管理员身份运行)\n", GetLastError()); return false; }

	char bin[MAX_PATH];
	GetModuleFileNameA(NULL, bin, MAX_PATH);
	char quoted[MAX_PATH * 2];
	_snprintf_s(quoted, sizeof(quoted), _TRUNCATE, "\"%s\"", bin);

	SC_HANDLE svc = CreateServiceA(scm, SVC_NAME, SVC_DISPLAY,
		SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL, quoted, NULL, NULL, NULL, NULL, NULL);

	if (svc == NULL)
	{
		DWORD e = GetLastError();
		if (e == ERROR_SERVICE_EXISTS)
		{
			svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
			if (svc != NULL)
			{
				ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
					quoted, NULL, NULL, NULL, NULL, NULL, SVC_DISPLAY);
				printf("[OK] 服务已存在, 已更新为自动启动并刷新路径\n");
			}
		}
		if (svc == NULL) { printf("CreateService failed: %lu\n", e); CloseServiceHandle(scm); return false; }
	}
	else printf("[OK] 服务已创建 (自动启动 / LocalSystem)\n");

	if (StartServiceA(svc, 0, NULL)) printf("[OK] 服务已启动\n");
	else
	{
		DWORD e = GetLastError();
		if (e == ERROR_SERVICE_ALREADY_RUNNING) printf("[OK] 服务已在运行\n");
		else printf("[注意] StartService 返回 %lu\n", e);
	}

	SERVICE_DESCRIPTIONA desc;
	desc.lpDescription = (char*)
		"RemoteControl 被控端守护: 开机即启动, 每 5 秒确认 agent 是否在线, 不在就把它投递到当前登录用户的会话里拉起(--backup 模式, 只兜底不抢端口)。";
	ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return true;
}

static bool UninstallService()
{
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (scm == NULL) { printf("OpenSCManager failed: %lu (请以管理员身份运行)\n", GetLastError()); return false; }
	SC_HANDLE svc = OpenServiceA(scm, SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
	if (svc == NULL) { printf("OpenService failed: %lu (服务可能未安装)\n", GetLastError()); CloseServiceHandle(scm); return false; }
	SERVICE_STATUS st;
	if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) printf("[OK] 已请求停止\n");
	Sleep(800);
	if (DeleteService(svc)) printf("[OK] 服务已删除\n");
	else printf("DeleteService failed: %lu\n", GetLastError());
	CloseServiceHandle(svc); CloseServiceHandle(scm);
	return true;
}

/* ------------------------------ 入口 ------------------------------ */
static void InitPaths()
{
	GetModuleFileNameA(NULL, g_dir, MAX_PATH);
	char* p = strrchr(g_dir, '\\');
	if (p != NULL) *p = 0;
	_snprintf_s(g_agent, sizeof(g_agent), _TRUNCATE, "%s\\%s", g_dir, AGENT_EXE);
	_snprintf_s(g_log, sizeof(g_log), _TRUNCATE, "%s\\service.log", g_dir);
}

int main(int argc, char** argv)
{
	InitPaths();

	bool wantInstall = false, wantUninstall = false, wantRun = false;
	for (int i = 1; i < argc; i++)
	{
		if (_stricmp(argv[i], "--install")   == 0) wantInstall = true;
		else if (_stricmp(argv[i], "--uninstall") == 0) wantUninstall = true;
		else if (_stricmp(argv[i], "--run")     == 0) wantRun = true;
	}

	if (wantUninstall) return UninstallService() ? 0 : 1;
	if (wantInstall)   return InstallService()   ? 0 : 1;

	if (wantRun)
	{
		printf("前台运行守护循环 (agent=%s, args=%s)\n", g_agent, AGENT_ARGS);
		printf("按 Ctrl+C 退出\n");
		Worker(NULL);
		return 0;
	}

	SERVICE_TABLE_ENTRYA table[] = { { (char*)SVC_NAME, ServiceMain }, { NULL, NULL } };
	if (!StartServiceCtrlDispatcherA(table))
	{
		printf("StartServiceCtrlDispatcher failed: %lu\n", GetLastError());
		printf("这是 Windows 服务程序: 请用 --install 安装后由系统启动 (或 --run 前台调试)。\n");
		return 1;
	}
	return 0;
}
