/* ============================================================================
   rc_service.cpp — 被控端内置守护服务 (v17)

   目的: 把「开机自启 / 崩溃自愈」做成 RemoteControl.exe 自带能力,
         用户双击一次 exe 即完成全部部署, 不需要任何脚本。

   服务做什么:
     LocalSystem / 自动启动 / 开机即起(不等登录)。
     每 5 秒确认当前交互式会话里有没有被控端, 没有就用
     CreateProcessAsUser 把它投递进去(带 --backup, 只兜底不抢端口)。

   为什么被控端本体不能跑在服务里:
     服务在 session 0, 那里没有桌面、抓不到屏也发不了键鼠。
     所以服务只当"拉起者", 真正干活的实例仍在用户会话中。
     (AnyDesk 也是这个结构)
   ============================================================================ */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "rc_service.h"

#pragma comment(lib,"wtsapi32.lib")
#pragma comment(lib,"userenv.lib")
#pragma comment(lib,"advapi32.lib")

#define RC_SVC_NAME "RemoteControlService"
#define RC_AGENT_EXE "RemoteControl.exe"
#define RC_POLL_MS  5000

static SERVICE_STATUS        g_ss;
static SERVICE_STATUS_HANDLE g_ssh = NULL;
static volatile LONG         g_svc_stop = 0;
static char g_svc_dir[MAX_PATH]   = { 0 };
static char g_svc_agent[MAX_PATH] = { 0 };
static char g_svc_log[MAX_PATH]   = { 0 };
static int  g_svc_port = 9999;

/* ------------------------------ 日志 ------------------------------ */
void RCService_SetLogDir(const char* dir)
{
	if (dir == NULL) return;
	_snprintf_s(g_svc_log, sizeof(g_svc_log), _TRUNCATE, "%s\\service.log", dir);
}

static void SvcLog(const char* fmt, ...)
{
	if (g_svc_log[0] == 0) return;
	char line[1024] = { 0 };
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d] ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	if (n < 0) n = 0;
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	size_t len = strlen(line);
	if (len + 2 < sizeof(line)) { line[len] = '\r'; line[len + 1] = '\n'; line[len + 2] = 0; }
	FILE* fp = NULL;
	if (fopen_s(&fp, g_svc_log, "ab") == 0 && fp != NULL) { fwrite(line, 1, strlen(line), fp); fclose(fp); }
}

/* ------------------- 某会话里是否已有被控端在跑 ------------------- */
static bool AgentRunningInSession(DWORD session)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32 pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
	bool found = false;
	if (Process32First(snap, &pe))
	{
		do {
			if (_wcsicmp(pe.szExeFile, L"RemoteControl.exe") == 0)
			{
				DWORD s = 0;
				if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session) { found = true; break; }
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return found;
}

/* ---------------- 把被控端投递到交互式会话里启动 ---------------- */
static void EnsureAgent()
{
	DWORD session = WTSGetActiveConsoleSessionId();
	if (session == 0xFFFFFFFF) return;              /* 无人登录, 无会话可投递 */
	if (AgentRunningInSession(session)) return;     /* 已在线 */

	HANDLE tok = NULL;
	if (!WTSQueryUserToken(session, &tok) || tok == NULL)
	{
		static DWORD lastErr = 0;
		DWORD e = GetLastError();
		if (e != lastErr) { lastErr = e; SvcLog("WTSQueryUserToken(session=%u) failed: %lu", session, e); }
		return;
	}

	char cmd[MAX_PATH * 2];
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" --port %d --backup", g_svc_agent, g_svc_port);

	STARTUPINFOA si; memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.lpDesktop = (char*)"winsta0\\default";       /* 关键: 放进交互式桌面 */
	PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

	void* env = NULL;
	CreateEnvironmentBlock(&env, tok, FALSE);
	BOOL ok = CreateProcessAsUserA(tok, NULL, cmd, NULL, NULL, FALSE,
		CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, env, g_svc_dir, &si, &pi);
	DWORD err = GetLastError();
	if (env) DestroyEnvironmentBlock(env);
	CloseHandle(tok);

	if (ok)
	{
		SvcLog("agent launched in session %u (pid %u)", session, pi.dwProcessId);
		CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	}
	else SvcLog("CreateProcessAsUser failed: %lu (session=%u)", err, session);
}

/* ------------------------------ 守护循环 ------------------------------ */
static DWORD WINAPI SvcWorker(LPVOID)
{
	SvcLog("watchdog started (poll=%dms, agent=%s, port=%d)", RC_POLL_MS, g_svc_agent, g_svc_port);
	while (InterlockedCompareExchange(&g_svc_stop, 0, 0) == 0)
	{
		EnsureAgent();
		for (int i = 0; i < RC_POLL_MS / 100 && InterlockedCompareExchange(&g_svc_stop, 0, 0) == 0; i++)
			Sleep(100);
	}
	SvcLog("watchdog stopped");
	return 0;
}

/* ------------------------------ SCM ------------------------------ */
static void ReportStatus(DWORD state, DWORD exitCode, DWORD waitHint)
{
	g_ss.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
	g_ss.dwCurrentState     = state;
	g_ss.dwWin32ExitCode    = exitCode;
	g_ss.dwWaitHint         = waitHint;
	g_ss.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
	g_ss.dwCheckPoint       = 0;
	if (g_ssh) SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI SvcCtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
	switch (ctrl)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
		InterlockedExchange(&g_svc_stop, 1);
		return NO_ERROR;
	default: return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

static void WINAPI SvcMain(DWORD, LPSTR*)
{
	g_ssh = RegisterServiceCtrlHandlerExA(RC_SVC_NAME, SvcCtrlHandler, NULL);
	if (g_ssh == NULL) return;
	ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
	ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
	SvcWorker(NULL);
	ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

bool RCService_Entry(const char* agentExe, int port)
{
	if (agentExe) _snprintf_s(g_svc_agent, sizeof(g_svc_agent), _TRUNCATE, "%s", agentExe);
	_snprintf_s(g_svc_dir, sizeof(g_svc_dir), _TRUNCATE, "%s", g_svc_agent);
	char* p = strrchr(g_svc_dir, '\\');
	if (p) *p = 0;
	g_svc_port = port;
	SERVICE_TABLE_ENTRYA table[] = { { (char*)RC_SVC_NAME, SvcMain }, { NULL, NULL } };
	if (!StartServiceCtrlDispatcherA(table)) return false;
	return true;
}

/* --------------------------- 安装 / 卸载 / 查询 --------------- */
bool RCService_IsInstalled()
{
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm == NULL) return false;
	SC_HANDLE svc = OpenServiceA(scm, RC_SVC_NAME, SERVICE_QUERY_STATUS);
	bool ok = (svc != NULL);
	if (svc) CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return ok;
}

bool RCService_Install(const char* agentExe, int port)
{
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (scm == NULL) return false;

	char bin[MAX_PATH * 2];
	_snprintf_s(bin, sizeof(bin), _TRUNCATE, "\"%s\" --svc --port %d", agentExe, port);

	SC_HANDLE svc = CreateServiceA(scm, RC_SVC_NAME, "RemoteControl Agent Watchdog",
		SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL, bin, NULL, NULL, NULL, NULL, NULL);

	if (svc == NULL && GetLastError() == ERROR_SERVICE_EXISTS)
	{
		svc = OpenServiceA(scm, RC_SVC_NAME, SERVICE_ALL_ACCESS);
		if (svc) ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
			bin, NULL, NULL, NULL, NULL, NULL, "RemoteControl Agent Watchdog");
	}
	if (svc == NULL) { CloseServiceHandle(scm); return false; }

	SERVICE_DESCRIPTIONA desc;
	desc.lpDescription = (char*)"RemoteControl 被控端守护: 开机即启动, 每 5 秒确认被控端是否在线, 不在就把它投递到当前登录会话里拉起(--backup 兜底模式)。";
	ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

	if (!StartServiceA(svc, 0, NULL))
	{
		DWORD e = GetLastError();
		if (e != ERROR_SERVICE_ALREADY_RUNNING) { CloseServiceHandle(svc); CloseServiceHandle(scm); return false; }
	}
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return true;
}

bool RCService_PointsTo(const char* agentExe)
{
	if (agentExe == NULL || agentExe[0] == 0) return false;
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm == NULL) return true;                      /* 读不到 → 保守: 别反复弹 UAC */
	SC_HANDLE svc = OpenServiceA(scm, RC_SVC_NAME, SERVICE_QUERY_CONFIG);
	if (svc == NULL) { CloseServiceHandle(scm); return false; }   /* 服务不存在 → 需要装 */
	DWORD need = 0;
	QueryServiceConfigA(svc, NULL, 0, &need);
	bool ok = true;                                    /* 查不到配置 → 保守 */
	if (need > 0)
	{
		QUERY_SERVICE_CONFIGA* qc = (QUERY_SERVICE_CONFIGA*)malloc(need);
		if (qc != NULL)
		{
			if (QueryServiceConfigA(svc, qc, need, &need) && qc->lpBinaryPathName != NULL)
			{
				const char* hay = qc->lpBinaryPathName;
				size_t nl = strlen(agentExe);
				ok = false;
				for (const char* p = hay; *p; ++p)
					if (_strnicmp(p, agentExe, nl) == 0) { ok = true; break; }
			}
			free(qc);
		}
	}
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return ok;
}

bool RCService_Remove()
{
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (scm == NULL) return false;
	SC_HANDLE svc = OpenServiceA(scm, RC_SVC_NAME, SERVICE_STOP | DELETE);
	if (svc == NULL) { CloseServiceHandle(scm); return true; }  /* 本来就没有 */
	SERVICE_STATUS st;
	ControlService(svc, SERVICE_CONTROL_STOP, &st);
	Sleep(500);
	bool ok = DeleteService(svc) != FALSE;
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return ok;
}
