// ============================================================
//  RemoteControl 被控端 (Agent)  v2.0
//  新特性: 后台静默运行 / 双向文件传输 / 心跳保活 / 断线等待重连
//         流式拆包(PacketStream) / 参数化配置 / 日志落盘
// ============================================================
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>


#include <atlimage.h>
#include <shellapi.h>   /* v9: CF_HDROP / DROPFILES / DragQueryFileW(剪贴板文件) */
#include <string>
#include <map>
#include <vector>
#include <cstdarg>
#include "..\\common\\protocol.h"
#include "..\\common\\rc_phase4.h"
#include "rc_service.h"   /* v17: 内置守护服务(无需外部脚本) */

/* Phase 4 模块实现于 audio_capture.cpp / camera_capture.cpp */
void HandleAudioPacket(Packet* p);
void HandleAudioStop();
void HandleCameraPacket(Packet* p);
void HandleCameraStop();
void HandleMicPacket(Packet* p);
void HandleMicStop();

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"ole32.lib")   /* v9: 剪贴板通道 PNG 转码用 CreateStreamOnHGlobal */
// 后台静默：子系统已在 vcxproj 中设为 Windows，入口 wWinMain，无控制台/无窗口

/* ---------------- 全局状态 ---------------- */
SOCKET g_listen_socket = INVALID_SOCKET;
SOCKET g_client_socket = INVALID_SOCKET;
CRITICAL_SECTION g_send_lock;   /* 所有 send 串行化：屏幕数据与文件数据不交叉 */
CRITICAL_SECTION g_log_lock;
volatile LONG g_running = 1;
volatile ULONGLONG g_last_recv_tick = 0;
ScreenCtrl g_screen_ctrl = { 0, 0, RC_ENC_PNG, 0 };
std::string g_file_root;        /* 非空 = 文件操作沙箱根目录 */
std::string g_log_path;
bool g_console = false;
HANDLE g_stop_event = NULL;
std::map<unsigned int, FILE*> g_uploads;  /* 上传中的文件 (仅接收线程访问) */

/* ---------- v9: 剪贴板同步虚拟通道状态 ---------- */
CRITICAL_SECTION g_clip_lock;
volatile LONG g_clip_auto  = 1;      /* 1 = 自动双向同步(控制端 HELLO 可切换) */
volatile LONG g_clip_hold  = 0;      /* 1 = 正在写本机剪贴板/接收文件(抑制上报, 防回环) */
volatile LONG g_clip_ready = 0;      /* 双方 HELLO 完成 */
volatile LONG g_clip_stopping = 0;
DWORD  g_clip_last_seq = 0;          /* 已处理过的 GetClipboardSequenceNumber */
std::vector<ClipFileDesc> g_clip_peer_files;   /* 对端 OFFER 的文件清单 */
ClipMsg g_clip_peer_offer = { 0 };             /* 对端最近一次 OFFER */
int    g_clip_peer_fmt = 0;                    /* 对端支持格式位(HELLO) */
/* 接收(对端 → 本机)文件会话状态 */
bool         g_clip_in_active = false;
unsigned int g_clip_in_idx = 0;
int          g_clip_in_count = 0;
FILE*        g_clip_in_fp = NULL;
unsigned long long g_clip_in_got = 0;
std::string  g_clip_in_dir;
std::vector<std::string>  g_clip_in_paths;
std::vector<ClipFileDesc> g_clip_in_desc;

/* ---------------- 日志 ---------------- */
void Log(const char* fmt, ...)
{
	char msg[2000];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(msg, sizeof(msg) - 1, _TRUNCATE, fmt, ap);
	va_end(ap);

	SYSTEMTIME st;
	GetLocalTime(&st);
	char line[2100];
	_snprintf_s(line, _TRUNCATE,
		"[%04d-%02d-%02d %02d:%02d:%02d.%03d][T%05u] %s\r\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		st.wMilliseconds, GetCurrentThreadId() % 100000, msg);

	EnterCriticalSection(&g_log_lock);
	if (!g_log_path.empty())
	{
		FILE* fp = NULL;
		if (fopen_s(&fp, g_log_path.c_str(), "ab") == 0 && fp != NULL)
		{
			fwrite(line, 1, strlen(line), fp);
			fclose(fp);
		}
	}
	if (g_console)
		printf("%s", line);
	OutputDebugStringA(line);
	LeaveCriticalSection(&g_log_lock);
}

/* ---------------- 发送封装 ---------------- */
static bool SendAll(SOCKET s, const char* data, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = send(s, data + sent, len - sent, 0);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

bool SendPacket(SOCKET s, Packet* pck)
{
	EnterCriticalSection(&g_send_lock);
	bool ok = SendAll(s, (char*)&pck->header.magic, GetPacketLen(pck));
	LeaveCriticalSection(&g_send_lock);
	return ok;
}

bool SendCmd(SOCKET s, int cmd, const char* body, int len)
{
	Packet* p = PackPacket(cmd, body, len);
	bool ok = SendPacket(s, p);
	free(p);
	return ok;
}

bool SendText(SOCKET s, int cmd, const std::string& text)
{
	return SendCmd(s, cmd, text.data(), (int)text.size());
}

/* ---------------- 命令行参数 ---------------- */
struct Options
{
	int  port = 9999;
	bool console = false;
	bool install = false;
	bool uninstall = false;
	bool stop = false;
	bool keepOld = false;
	bool backup = false;      /* v14: 由 HKCU\\Run 兜底拉起的实例(不抢端口) */
	bool svc = false;         /* v17: 以 Windows 服务身份运行守护 */
	bool deploy = false;      /* v17: 已提权的部署实例(装齐三层后继续常驻) */

	std::string log;
	std::string root;
};

static std::vector<std::string> SplitArgs(LPSTR cmdline)
{
	std::vector<std::string> out;
	std::string cur;
	bool inQuote = false;
	for (char* p = cmdline; *p; ++p)
	{
		if (*p == '"') { inQuote = !inQuote; continue; }
		if ((*p == ' ' || *p == '\t') && !inQuote)
		{
			if (!cur.empty()) { out.push_back(cur); cur.clear(); }
			continue;
		}
		cur.push_back(*p);
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

static void ParseArgs(LPSTR cmdline, Options& o)
{
	std::vector<std::string> a = SplitArgs(cmdline);
	for (size_t i = 0; i < a.size(); ++i)
	{
		const std::string& s = a[i];
		if (s == "--port" && i + 1 < a.size())      o.port = atoi(a[++i].c_str());
		else if (s == "--console")                  o.console = true;
		else if (s == "--install")                  o.install = true;
		else if (s == "--uninstall")                o.uninstall = true;
		else if (s == "--stop")                     o.stop = true;
		else if (s == "--keep-old")                 o.keepOld = true;
		else if (s == "--backup")                   o.backup = true;
		else if (s == "--svc")                      o.svc = true;
		else if (s == "--deploy")                   o.deploy = true;

		else if (s == "--log" && i + 1 < a.size())  o.log = a[++i];
		else if (s == "--root" && i + 1 < a.size()) o.root = a[++i];
	}
}

/* ---------------- 开机自启 (HKCU\...\Run) ---------------- */
#define RC_RUN_KEY  "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RC_RUN_NAME "RemoteControlAgent"
static int g_agent_port = 9999;   /* v8: 当前监听端口(自启项里要用) */

static bool InstallAutostart()
{
	char exe[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	char portArg[32] = { 0 };
	/* v14: Run 键写 --backup —— 兜底实例发现主实例已在跑就安静退出, 绝不抢端口/降级 */
	sprintf_s(portArg, " --port %d --backup", g_agent_port);
	std::string val = "\"";
	val += exe;
	val += "\"";
	val += portArg;

	HKEY hKey = NULL;
	if (RegCreateKeyExA(HKEY_CURRENT_USER, RC_RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
		return false;
	LONG r = RegSetValueExA(hKey, RC_RUN_NAME, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)val.size() + 1);
	RegCloseKey(hKey);
	Log("install autostart: %s (%s)", r == ERROR_SUCCESS ? "OK" : "FAIL", val.c_str());
	return r == ERROR_SUCCESS;
}

static bool UninstallAutostart()
{
	HKEY hKey = NULL;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, RC_RUN_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
		return false;
	LONG r = RegDeleteValueA(hKey, RC_RUN_NAME);
	RegCloseKey(hKey);
	Log("uninstall autostart: %s", r == ERROR_SUCCESS ? "OK" : "FAIL(not found)");
	return true;
}

/* ---- v8: 开机自启升级 ----
   优先用「计划任务: 用户登录时以最高权限启动」(schtasks /SC ONLOGON /RL HIGHEST)。
   好处: 开机自启出来的实例天生是管理员权限, 不会再出现
   「C盘写不了 / 任务管理器等高权限窗口点不动(UIPI)」;
   计划任务创建失败(如权限不足)时回退 HKCU\...\Run(普通权限)。 */
#define RC_TASK_NAME "RemoteControlAgent"

static bool RunHiddenWait(const char* cmdline, DWORD timeoutMs)
{
	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	std::string cl(cmdline);
	if (!CreateProcessA(NULL, &cl[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return false;
	WaitForSingleObject(pi.hProcess, timeoutMs);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code == 0;
}

static bool BuildAutostartCmd(char* taskTarget, size_t cap)
{
	char exe[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	sprintf_s(taskTarget, cap, "\"%s\" --port %d", exe, g_agent_port);
	return true;
}

static bool InstallAutostartTask()
{
	char tr[MAX_PATH + 64] = { 0 };
	BuildAutostartCmd(tr, sizeof(tr));
	char cmd[1024] = { 0 };
	sprintf_s(cmd, "schtasks.exe /Create /TN \"%s\" /TR \"%s\" /SC ONLOGON /RL HIGHEST /F", RC_TASK_NAME, tr);
	bool ok = RunHiddenWait(cmd, 20000);
	Log("install autostart task: %s (%s)", ok ? "OK" : "FAIL", tr);
	return ok;
}

static bool UninstallAutostartTask()
{
	char cmd[512] = { 0 };
	sprintf_s(cmd, "schtasks.exe /Delete /TN \"%s\" /F", RC_TASK_NAME);
	bool ok = RunHiddenWait(cmd, 20000);
	Log("uninstall autostart task: %s", ok ? "OK" : "FAIL(not found)");
	return ok;
}

static bool AutostartTaskExists()
{
	char cmd[512] = { 0 };
	sprintf_s(cmd, "schtasks.exe /Query /TN \"%s\"", RC_TASK_NAME);
	return RunHiddenWait(cmd, 20000);
}

static bool RunValueExists()
{
	HKEY hKey = NULL;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, RC_RUN_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
		return false;
	DWORD sz = 0, type = 0;
	bool found = (RegQueryValueExA(hKey, RC_RUN_NAME, NULL, &type, NULL, &sz) == ERROR_SUCCESS);
	RegCloseKey(hKey);
	return found;
}

/* 通知指定端口上已运行的实例退出 */
static void StopRunningInstance(int port)
{
	char name[128];
	sprintf_s(name, "Global\\RemoteControlAgentStop_%d", port);
	HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
	if (ev == NULL)
	{
		/* 兼容 v2.0 早期版本的无端口命名 */
		ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Global\\RemoteControlAgentStop");
	}
	if (ev != NULL)
	{
		SetEvent(ev);
		CloseHandle(ev);
		Log("stop signal sent to running agent (port=%d)", port);
	}
	else
	{
		Log("no running agent found (port=%d)", port);
	}
}
/* v6: 结束其他同名被控端进程(排除自身 PID), 便于新版本直接接管 */

static int KillOtherAgentProcesses()

{

	DWORD me = GetCurrentProcessId();

	int killed = 0;

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (snap == INVALID_HANDLE_VALUE)

	{

		Log("takeover: snapshot failed (%u)", GetLastError());

		return 0;

	}

	PROCESSENTRY32W pe;

	memset(&pe, 0, sizeof(pe));

	pe.dwSize = sizeof(pe);

	BOOL ok = Process32FirstW(snap, &pe);

	while (ok)

	{

		if (pe.th32ProcessID != 0 && pe.th32ProcessID != me && pe.th32ProcessID != 4)

		{

			wchar_t low[MAX_PATH] = { 0 };

			wcsncpy_s(low, pe.szExeFile, _TRUNCATE);

			CharLowerW(low);

			if (wcsstr(low, L"remotecontrol") != NULL || wcsstr(low, L"被控端") != NULL)

			{

				HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);

				if (hp != NULL)

				{

					if (TerminateProcess(hp, 0))

					{

						WaitForSingleObject(hp, 2000);

						Log("takeover: terminated old agent pid=%u", pe.th32ProcessID);

						++killed;

					}

					CloseHandle(hp);

				}

			}

		}

		ok = Process32NextW(snap, &pe);

	}

	CloseHandle(snap);

	return killed;



}



/* v6: 若已注册开机自启, 把其中的 exe 路径刷新为当前这份(避免开机仍启动旧版本) */

static void RefreshAutostartIfPresent(int port)
{
	g_agent_port = port;
	/* v8: 自启项若是计划任务形式, 同步刷新其中的 exe 路径与端口(否则开机还启动旧版本) */
	if (AutostartTaskExists())
	{
		char tr[MAX_PATH + 64] = { 0 };
		BuildAutostartCmd(tr, sizeof(tr));
		/* v14 修复: schtasks /Change 不支持 /TR(实测报「拒绝访问」, 任务里的路径根本没变),
		   所以开机仍然拉起旧目录里的 exe —— 表现就是「换了新版本, 重启后新功能全没了」。
		   正确做法: 用 /Create ... /F 重建同名任务, 它会覆盖并更新 /TR 指向当前 exe。 */
		bool refreshOk = InstallAutostartTask();
		Log("autostart task refresh: %s (port %d, target %s)",
			refreshOk ? "OK" : "FAIL(需要管理员权限, 任务可能仍指向旧 exe)", port, tr);
	}
	/* v14: 双保险 —— 计划任务被安全软件拦截/被误删时, HKCU\\Run 仍能拉起诉一个 --backup
	   兜底实例(普通权限: 能连上/能看屏幕/能传文件), 保证「至少连得上」。 */
	InstallAutostart();

	HKEY hKey = NULL;

	if (RegOpenKeyExA(HKEY_CURRENT_USER, RC_RUN_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)

		return;

	char cur[MAX_PATH * 2] = { 0 };

	DWORD sz = sizeof(cur);

	DWORD type = 0;

	if (RegQueryValueExA(hKey, RC_RUN_NAME, NULL, &type, (BYTE*)cur, &sz) != ERROR_SUCCESS)

	{

		RegCloseKey(hKey);

		return;

	}

	char exe[MAX_PATH] = { 0 };

	GetModuleFileNameA(NULL, exe, MAX_PATH);

	char want[MAX_PATH * 2] = { 0 };

	sprintf_s(want, "\"%s\" --port %d", exe, port);

	if (_stricmp(cur, want) != 0)

	{

		LONG r = RegSetValueExA(hKey, RC_RUN_NAME, 0, REG_SZ, (const BYTE*)want, (DWORD)strlen(want) + 1);

		Log("autostart refreshed: %s (rc=%d)", want, (int)r);

	}

	RegCloseKey(hKey);

}


/* ---------------- 屏幕采集与发送 ---------------- */
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	UINT num = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;
	Gdiplus::ImageCodecInfo* pInfo = (Gdiplus::ImageCodecInfo*)malloc(size);
	if (pInfo == NULL) return -1;
	Gdiplus::GetImageEncoders(num, size, pInfo);
	int found = -1;
	for (UINT i = 0; i < num; ++i)
	{
		if (wcscmp(pInfo[i].MimeType, format) == 0) { *pClsid = pInfo[i].Clsid; found = (int)i; break; }
	}
	free(pInfo);
	return found;
}

/* 屏幕采集缓冲：32bpp 顶朝下 DIB + 上一帧快照（用于脏矩形检测） */
static HDC       s_capDC = NULL;
static HBITMAP   s_capBmp = NULL;
static HGDIOBJ   s_capOld = NULL;
static void*     s_capBits = NULL;
static int       s_capW = 0, s_capH = 0;
static std::vector<unsigned char> s_prev;
static bool      s_prevValid = false;
static unsigned long long s_frameCounter = 0;
static void ReleaseCaptureBuffer()
{
	if (s_capDC != NULL)
	{
		if (s_capOld != NULL) { SelectObject(s_capDC, s_capOld); s_capOld = NULL; }
		DeleteDC(s_capDC);
		s_capDC = NULL;
	}
	if (s_capBmp != NULL) { DeleteObject(s_capBmp); s_capBmp = NULL; }
	s_capBits = NULL;
	s_capW = s_capH = 0;
	s_prev.clear();
	s_prevValid = false;
}
static bool EnsureCaptureBuffer(int w, int h)
{
	if (s_capDC != NULL && s_capW == w && s_capH == h)
		return true;
	ReleaseCaptureBuffer();
	s_capDC = CreateCompatibleDC(NULL);
	if (s_capDC == NULL)
		return false;
	BITMAPINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;          /* 负高度 = 顶朝下，行序与 memcmp 一致 */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = NULL;
	s_capBmp = CreateDIBSection(s_capDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (s_capBmp == NULL || bits == NULL)
	{
		if (s_capBmp != NULL) { DeleteObject(s_capBmp); s_capBmp = NULL; }
		DeleteDC(s_capDC);
		s_capDC = NULL;
		return false;
	}
	s_capOld = SelectObject(s_capDC, s_capBmp);
	s_capBits = bits;
	s_capW = w;
	s_capH = h;
	s_prev.assign((size_t)w * (size_t)h * 4, 0);   /* 上一帧快照缓冲(必须与采集缓冲同尺寸) */
	s_prevValid = false;
	return true;
}
/* 与上一帧逐行/逐 8 字节比较，得到变化区域的包围盒（外扩 8px 余量） */
static bool DetectDirtyRect(int w, int h, RECT& out)
{
	const unsigned char* cur = (const unsigned char*)s_capBits;
	const size_t rowbytes = (size_t)w * 4;
	if (cur == NULL || s_prev.size() < rowbytes * (size_t)h)
	{
		out.left = 0; out.top = 0; out.right = w; out.bottom = h;   /* 快照不可用 → 整屏 */
		return true;
	}
	const unsigned char* prv = s_prev.data();
	int y0 = -1, y1 = -1;
	for (int y = 0; y < h; ++y)
	{
		if (memcmp(cur + (size_t)y * rowbytes, prv + (size_t)y * rowbytes, rowbytes) != 0)
		{
			if (y0 < 0) y0 = y;
			y1 = y;
		}
	}
	if (y0 < 0)
		return false;                       /* 整屏无变化 */
	int x0 = w, x1 = -1;
	const int nq = (int)(rowbytes / 8);     /* 每 8 字节 = 2 个像素 */
	for (int y = y0; y <= y1; ++y)
	{
		const unsigned long long* a = (const unsigned long long*)(cur + (size_t)y * rowbytes);
		const unsigned long long* b = (const unsigned long long*)(prv + (size_t)y * rowbytes);
		for (int k = 0; k < nq; ++k)
		{
			if (a[k] != b[k])
			{
				int px = k * 2;
				if (px < x0) x0 = px;
				if (px + 1 > x1) x1 = px + 1;
			}
		}
	}
	if (x1 < 0) { x0 = 0; x1 = w - 1; }
	const int M = 8;                        /* 余量，避免边缘残影 */
	x0 -= M; y0 -= M; x1 += M + 1; y1 += M + 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	out.left = x0; out.top = y0; out.right = x1; out.bottom = y1;
	return true;
}
/* 把采集缓冲的指定区域编码成 PNG / JPEG(带质量控制) */
static ULONG_PTR s_gdiplusToken = 0;
static bool EnsureGdiplus()
{
	if (s_gdiplusToken != 0)
		return true;
	Gdiplus::GdiplusStartupInput gsi;
	if (Gdiplus::GdiplusStartup(&s_gdiplusToken, &gsi, NULL) != Gdiplus::Ok)
	{
		s_gdiplusToken = 0;
		return false;
	}
	return true;
}
static bool EncodeRegion(const RECT& r, std::vector<char>& out)
{
	const int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0)
		return false;
	if (!EnsureGdiplus())                        /* 必须先初始化 GDI+ 才能构造 Bitmap */
	{
		Log("encode fail: GDI+ init failed");
		return false;
	}
	/* 直接用采集 DIB 的像素(带 stride)构造 GDI+ 位图，省掉一次中间拷贝 */
	const int    stride = s_capW * 4;
	BYTE*        scan0 = (BYTE*)s_capBits + (size_t)r.top * stride + (size_t)r.left * 4;
	Gdiplus::Bitmap bmp(w, h, stride, PixelFormat32bppRGB, scan0);
	if (bmp.GetLastStatus() != Gdiplus::Ok)
	{
		Log("encode fail: Bitmap status=%d %dx%d stride=%d", (int)bmp.GetLastStatus(), w, h, stride);
		return false;
	}
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
	if (hMem == NULL)
		return false;
	IStream* pStream = NULL;
	if (CreateStreamOnHGlobal(hMem, FALSE, &pStream) != S_OK)
	{
		GlobalFree(hMem);
		return false;
	}
	Gdiplus::Status st = Gdiplus::GenericError;
	bool tryJpeg = (g_screen_ctrl.encode == RC_ENC_JPEG);
	if (tryJpeg)
	{
		CLSID clsid;
		if (GetEncoderClsid(L"image/jpeg", &clsid) >= 0)
		{
			int q = g_screen_ctrl.quality;
			if (q < 10)  q = 10;
			if (q > 100) q = 100;
			ULONG qv = (ULONG)q;
			Gdiplus::EncoderParameters ep;
			ep.Count = 1;
			ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
			ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
			ep.Parameter[0].NumberOfValues = 1;
			ep.Parameter[0].Value = &qv;
			st = bmp.Save(pStream, &clsid, &ep);
		}
	}
	if (st != Gdiplus::Ok)                     /* 未选 JPEG 或 JPEG 失败 → PNG */
	{
		LARGE_INTEGER zero = { 0 };
		pStream->Seek(zero, STREAM_SEEK_SET, NULL);
		st = bmp.Save(pStream, &Gdiplus::ImageFormatPNG, NULL);
	}
	if (st != Gdiplus::Ok)
	{
		pStream->Release();
		GlobalFree(hMem);
		return false;
	}
	LARGE_INTEGER lg = { 0 };
	pStream->Seek(lg, STREAM_SEEK_SET, NULL);
	const char* pdata = (const char*)GlobalLock(hMem);
	const size_t len = GlobalSize(hMem);
	if (pdata != NULL && len > 0)
		out.assign(pdata, pdata + len);
	GlobalUnlock(hMem);
	pStream->Release();
	GlobalFree(hMem);
	return !out.empty();
}
void HandleScreen(Packet* packet)
{
	if (packet->header.body_len >= (int)sizeof(ScreenCtrl))
		memcpy(&g_screen_ctrl, packet->body, sizeof(ScreenCtrl));
	static bool dpiDone = false;
	if (!dpiDone)
	{
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		dpiDone = true;
	}
	const int sw = GetSystemMetrics(SM_CXSCREEN);
	const int sh = GetSystemMetrics(SM_CYSCREEN);
	if (sw <= 0 || sh <= 0 || !EnsureCaptureBuffer(sw, sh))
		return;
	HDC hScreen = GetDC(NULL);
	if (hScreen == NULL)
		return;
	BitBlt(s_capDC, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY | CAPTUREBLT);   /* v13: 必须带 CAPTUREBLT,
		否则右键菜单/开始菜单/工具提示等分层窗口(WS_EX_LAYERED)的区域会被抓成黑色 —— 用户看到"点右键桌面就变黑" */
	ReleaseDC(NULL, hScreen);
	const bool wantFull = (g_screen_ctrl.force_full != 0) || !s_prevValid;
	if (g_screen_ctrl.force_full != 0)
		g_screen_ctrl.force_full = 0;       /* 一次性：只强制一帧整屏，用于客户端建立基准 */
	ScreenFrameHead head;
	memset(&head, 0, sizeof(head));
	head.full_w = sw;
	head.full_h = sh;
	head.format = g_screen_ctrl.encode;
	RECT dirty = { 0, 0, sw, sh };
	bool changed = true;
	if (!wantFull)
		changed = DetectDirtyRect(sw, sh, dirty);
	if (!changed)
	{
		head.w = 0;
		head.h = 0;
		head.flags = RC_FR_NODATA;
		SendCmd(g_client_socket, CMD_SCREEN, (char*)&head, (int)sizeof(head));
		return;
	}
	int dw = dirty.right - dirty.left;
	int dh = dirty.bottom - dirty.top;
	bool full = wantFull;
	if (!full && (long long)dw * dh * 100 > (long long)sw * sh * 55)
		full = true;                        /* 变化面积过大时不值得做局部，直接整屏 */
	if (full)
	{
		dirty.left = 0; dirty.top = 0; dirty.right = sw; dirty.bottom = sh;
		dw = sw; dh = sh;
	}
	std::vector<char> enc;
	if (!EncodeRegion(dirty, enc))
	{
		/* 编码失败也要同步快照，否则下一帧仍会全屏重发 */
		if (s_prev.size() >= (size_t)sw * sh * 4)
		{
			memcpy(s_prev.data(), s_capBits, (size_t)sw * sh * 4);
			s_prevValid = true;
		}
		return;
	}
	head.x = dirty.left;
	head.y = dirty.top;
	head.w = dw;
	head.h = dh;
	head.flags = full ? RC_FR_FULL : RC_FR_PARTIAL;
	std::vector<char> body(sizeof(head) + enc.size());
	memcpy(body.data(), &head, sizeof(head));
	memcpy(body.data() + sizeof(head), enc.data(), enc.size());
	if (!SendCmd(g_client_socket, CMD_SCREEN, body.data(), (int)body.size()))
		Log("screen send failed, len=%d", (int)body.size());
	/* 同步快照(只拷贝变化行，避免每帧整屏 memcpy) */
	const size_t rowbytes = (size_t)sw * 4;
	for (int y = dirty.top; y < dirty.bottom; ++y)
		memcpy(s_prev.data() + (size_t)y * rowbytes, (const char*)s_capBits + (size_t)y * rowbytes, rowbytes);
	s_prevValid = true;
	if ((++s_frameCounter % 120) == 0)
		Log("screen: %s region=%dx%d@%d,%d enc=%d bytes=%d", full ? "FULL" : "PART",
			dw, dh, dirty.left, dirty.top, g_screen_ctrl.encode, (int)enc.size());
}
/* ---------------- v11: 权限 / 输入诊断 ---------------- */
/* 当前进程的提权状态与完整性级别: "elevated/HIGH" 才能操作任务管理器等提权窗口。 */
static const char* PrivState()
{
	static char s[64] = { 0 };
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
		return "unknown";
	DWORD cb = 0;
	TOKEN_ELEVATION te;
	memset(&te, 0, sizeof(te));
	bool elev = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb) && te.TokenIsElevated;
	const char* il = "unknown";
	DWORD need = 0;
	GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &need);
	if (need > 0 && need < 4096)
	{
		char buf[4096];
		if (GetTokenInformation(tok, TokenIntegrityLevel, buf, need, &need))
		{
			TOKEN_MANDATORY_LABEL* ml = (TOKEN_MANDATORY_LABEL*)buf;
			DWORD cnt = *GetSidSubAuthorityCount(ml->Label.Sid);
			DWORD rc = *GetSidSubAuthority(ml->Label.Sid, cnt - 1);
			if (rc >= SECURITY_MANDATORY_SYSTEM_RID)       il = "SYSTEM";
			else if (rc >= SECURITY_MANDATORY_HIGH_RID)    il = "HIGH";
			else if (rc >= SECURITY_MANDATORY_MEDIUM_RID)  il = "MEDIUM";
			else                                           il = "LOW";
		}
	}
	CloseHandle(tok);
	sprintf_s(s, "%s/%s", elev ? "elevated" : "NOT-elevated", il);
	return s;
}

/* 每次点击/按键时记录: 事件到达被控端了吗? 当时前台窗口是谁? UIPI 会不会拦? */
static void LogInputDiag(const char* what)
{
	HWND fg = GetForegroundWindow();
	char cls[128] = { 0 }, title[200] = { 0 };
	DWORD pid = 0;
	if (fg != NULL)
	{
		GetClassNameA(fg, cls, sizeof(cls) - 1);
		GetWindowTextA(fg, title, sizeof(title) - 1);
		GetWindowThreadProcessId(fg, &pid);
	}
	POINT pt;
	GetCursorPos(&pt);
	Log("input %s -> fg=\"%s\" cls=%s pid=%u cur=%d,%d self=%s", what, title, cls, pid, (int)pt.x, (int)pt.y, PrivState());
}

/* v11-fix: 自动放行 Windows 防火墙入站(跨机连接必需)。
   被控端以管理员运行时调用; 失败静默忽略。规则按 exe 路径放行(TCP+UDP 全部端口),
   这样 --port 换端口也不用再改规则。 */
static void RunHiddenWait(const char* cmdline)
{
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", cmdline);
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return;
	WaitForSingleObject(pi.hProcess, 8000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

static void EnsureFirewallRule()
{
	char exe[MAX_PATH] = { 0 };
	if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
		return;
	char cmd[1400];
	snprintf(cmd, sizeof(cmd),
		"cmd /c netsh advfirewall firewall delete rule name=\"RemoteControlAgent\" >nul 2>&1"
		" & netsh advfirewall firewall add rule name=\"RemoteControlAgent\" dir=in action=allow program=\"%s\" enable=yes profile=any",
		exe);
	RunHiddenWait(cmd);
	Log("firewall: inbound rule ensured for %s", exe);
}

/* ---------------- 鼠标 / 键盘 ---------------- */
/* v13: 记录上一次注入的光标位置 —— 控制端高频移动时坐标常常没变,
   重复 SendInput 只会白占输入队列(拖拽/点击会因此变迟钝)。 */
static int s_lastMx = INT_MIN, s_lastMy = INT_MIN;
/* ---------------- 鼠标 / 键盘 (v5: 目标=像本地一样跟手) ---------------- */
static void SendMouseInput(DWORD flags, LONG dx, LONG dy, DWORD data)
{
	INPUT in = { 0 };
	in.type = INPUT_MOUSE;
	in.mi.dx = dx;
	in.mi.dy = dy;
	in.mi.mouseData = data;
	in.mi.dwFlags = flags;
	SendInput(1, &in, sizeof(in));
}

/* 屏幕像素 -> 绝对坐标(虚拟桌面 0..65535)。被控端进程 DPI 感知, 像素即真实物理像素,
   不再被系统 DPI 虚拟化缩放 —— 这是"点不准 / 右键菜单跑到别处"的根因修复。 */
static void SendMouseAbs(int x, int y)
{
	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	SendMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
		(LONG)MulDiv(x - vx, 65535, vw > 1 ? vw - 1 : 1),
		(LONG)MulDiv(y - vy, 65535, vh > 1 ? vh - 1 : 1), 0);
}

void HandleMouse(Packet* packet)
{
	if (packet->header.body_len < (int)sizeof(Mouse))
		return;
	Mouse mouse;
	memcpy(&mouse, packet->body, sizeof(Mouse));

	/* 每个事件先把光标精确落到目标像素, 再送按键/滚轮(拖拽/右键菜单不会错位) */
	if (mouse.action == MOUSE_MOVE)
	{
		/* v13: 坐标没变就丢弃 —— 高频移动时能省掉大量无意义的 SendInput,
		   输入队列不再堆积,点击/拖拽明显更跟手。按键事件不走此捷径。 */
		if (mouse.ptXY.x == s_lastMx && mouse.ptXY.y == s_lastMy)
			return;
		s_lastMx = mouse.ptXY.x;
		s_lastMy = mouse.ptXY.y;
		SendMouseAbs(mouse.ptXY.x, mouse.ptXY.y);
		return;
	}
	SendMouseAbs(mouse.ptXY.x, mouse.ptXY.y);
	s_lastMx = mouse.ptXY.x;
	s_lastMy = mouse.ptXY.y;

	switch (mouse.action)
	{
	case MOUSE_MOVE:    break;                                          /* 上一行已落位 */
	case MOUSE_LDOWN:   SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0, 0); break;
	case MOUSE_LUP:     SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0, 0); break;
	case MOUSE_RDOWN:   SendMouseInput(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0); break;
	case MOUSE_RUP:     SendMouseInput(MOUSEEVENTF_RIGHTUP, 0, 0, 0); break;
	case MOUSE_MDOWN:   SendMouseInput(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0); break;
	case MOUSE_MUP:     SendMouseInput(MOUSEEVENTF_MIDDLEUP, 0, 0, 0); break;
	case MOUSE_LCLICK:  SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0, 0); break;
	case MOUSE_RCLICK:  SendMouseInput(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_RIGHTUP, 0, 0, 0); break;
	case MOUSE_MCLICK:  SendMouseInput(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_MIDDLEUP, 0, 0, 0); break;
	case MOUSE_WHEEL:   SendMouseInput(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(int)mouse.delta); break;
	case MOUSE_HWHEEL:  SendMouseInput(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)(int)mouse.delta); break;
	case MOUSE_X1DOWN:  SendMouseInput(MOUSEEVENTF_XDOWN, 0, 0, XBUTTON1); break;
	case MOUSE_X1UP:    SendMouseInput(MOUSEEVENTF_XUP, 0, 0, XBUTTON1); break;
	case MOUSE_X2DOWN:  SendMouseInput(MOUSEEVENTF_XDOWN, 0, 0, XBUTTON2); break;
	case MOUSE_X2UP:    SendMouseInput(MOUSEEVENTF_XUP, 0, 0, XBUTTON2); break;
	case MOUSE_LDCLICK:
		for (int i = 0; i < 2; ++i) { SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0, 0); }
		break;
	case MOUSE_RDCLICK:
		for (int i = 0; i < 2; ++i) { SendMouseInput(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_RIGHTUP, 0, 0, 0); }
		break;
	case MOUSE_MDCLICK:
		for (int i = 0; i < 2; ++i) { SendMouseInput(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0); SendMouseInput(MOUSEEVENTF_MIDDLEUP, 0, 0, 0); }
		break;
	default:
		Log("unknown mouse action %d", mouse.action);
		break;
	}
	if (mouse.action != MOUSE_MOVE)
		LogInputDiag("mouse");
}

/* 键盘: 同时带 VK + 扫描码 + 扩展位, 被控端按"物理键盘"重放。
   只发 VK 时方向键/小键盘/Alt组合/游戏(DirectInput)无法正确识别。 */
static void SendKeyInput(WORD vk, WORD scan, bool up, bool extended)
{
	INPUT in = { 0 };
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	in.ki.wScan = scan;
	in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
	SendInput(1, &in, sizeof(in));
}

void HandleKeyBoard(Packet* packet)
{
	if (packet->header.body_len < (int)sizeof(KeyBoard))
		return;
	KeyBoard kb;
	memcpy(&kb, packet->body, sizeof(KeyBoard));

	const WORD vk = (WORD)kb.virtual_code;
	WORD scan = (WORD)kb.scan_code;
	if (scan == 0)
		scan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
	const bool up = (kb.key_status == RC_KEY_UP);
	const bool ext = (kb.flags & RC_KF_EXTENDED) != 0;
	SendKeyInput(vk, scan, up, ext);
	if (!up)
		LogInputDiag("key");
}

/* 文本直投(CMD_KEYTEXT): UTF-8 -> KEYEVENTF_UNICODE 逐字符上屏。
   不受被控端键盘布局/输入法状态影响, 中文/日文/表情可直接输入。 */
void HandleKeyText(Packet* packet)
{
	const int n = packet->header.body_len;
	if (n <= 0)
		return;
	std::string utf8(packet->body, packet->body + n);
	int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
	if (wn <= 0)
		return;
	std::vector<wchar_t> w((size_t)wn);
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], wn);
	for (int i = 0; i < wn; ++i)
	{
		INPUT in[2] = { 0 };
		in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = (WORD)w[i]; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
		in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = (WORD)w[i]; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		SendInput(2, in, sizeof(INPUT));
		Sleep(2);
	}
	Log("keytext: injected %d chars", wn);
}

/* ============================================================
 *  文件传输 / 文件管理
 * ============================================================ */
static char LowCh(char c) { return (char)tolower((unsigned char)c); }

/* 是否包含 ".." 路径分量（越权检测） */
static bool HasDotDot(const std::string& p)
{
	size_t i = 0;
	while ((i = p.find("..", i)) != std::string::npos)
	{
		bool leftOk = (i == 0) || p[i - 1] == '\\' || p[i - 1] == '/';
		bool rightOk = (i + 2 >= p.size()) || p[i + 2] == '\\' || p[i + 2] == '/';
		if (leftOk && rightOk) return true;
		i += 2;
	}
	return false;
}

static bool PathAllowed(const std::string& path)
{
	if (HasDotDot(path)) return false;
	if (g_file_root.empty()) return true;
	std::string a = path, b = g_file_root;
	for (size_t i = 0; i < a.size(); ++i) a[i] = LowCh(a[i]);
	for (size_t i = 0; i < b.size(); ++i) b[i] = LowCh(b[i]);
	if (a.size() < b.size()) return false;
	return a.compare(0, b.size(), b) == 0;
}

static std::string FormatFileTime(const FILETIME* ft)
{
	FILETIME local;
	SYSTEMTIME st;
	FileTimeToLocalFileTime(ft, &local);
	FileTimeToSystemTime(&local, &st);
	char buf[32];
	_snprintf_s(buf, _TRUNCATE, "%04d-%02d-%02d %02d:%02d",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
	return std::string(buf);
}

/* 目录列表文本: 每行 "<D|F>\t名称\t大小\t修改时间" */
static std::string ListDirectory(const std::string& path)
{
	std::string dir = path;
	if (dir.empty()) dir = "C:\\";
	if (dir.size() > 1 && (dir.back() == '\\' || dir.back() == '/'))
		dir.pop_back();

	std::string pattern = dir + "\\*";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return "err:无法打开目录 " + dir;
	std::string out;
	do
	{
		if (strcmp(fd.cFileName, ".") == 0)
			continue;
		bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
		char line[1400];
		_snprintf_s(line, _TRUNCATE, "%c\t%s\t%llu\t%s\n",
			isDir ? 'D' : 'F', fd.cFileName, size, FormatFileTime(&fd.ftLastWriteTime).c_str());
		out += line;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return out;
}

static void HandleFileList(Packet* p)
{
	std::string path(p->body, (size_t)p->header.body_len);
	if (path.empty())   /* 空路径 = 浏览起始目录：有 --root 时即沙箱根，否则 C:\ */
		path = g_file_root.empty() ? std::string("C:\\") : g_file_root;
	if (!PathAllowed(path))
	{
		SendText(g_client_socket, CMD_FILE_LIST_RES, "err:路径越权(不允许 .. 或超出 --root 沙箱)");
		return;
	}
	std::string dir = path;
	if (dir.size() > 1 && (dir.back() == '\\' || dir.back() == '/'))
		dir.pop_back();
	/* 首行回传规范化后的绝对路径，控制端据此同步地址栏与相对路径基准 */
	SendText(g_client_socket, CMD_FILE_LIST_RES, "@path\t" + dir + "\n" + ListDirectory(dir));
}

/* 下载：独立线程流式发送，避免阻塞接收线程 */
struct DownloadJob
{
	unsigned int file_id;
	std::string  path;
	unsigned long long offset;
};

/* 路径解码/建目录/开文件: 前置声明(下面 HandleFile* 系列要用) */
static std::wstring PathToWide(const std::string& p);
static void       EnsureDirsW(const std::wstring& wpath);
static FILE*      OpenRemoteFile(const char* pathBytes, const wchar_t* mode);

DWORD WINAPI FileSendThread(LPVOID lpParam)
{
	DownloadJob* job = (DownloadJob*)lpParam;
	FILE* fp = NULL;
	fp = OpenRemoteFile(job->path.c_str(), L"rb");
	if (fp == NULL)
	{
		FileChunk err = { job->file_id, 0, 0, RC_FF_ERROR };
		SendCmd(g_client_socket, CMD_FILE_END, (char*)&err, sizeof(err));
		Log("download failed, cannot open: %s", job->path.c_str());
		delete job;
		return 0;
	}
	_fseeki64(fp, (long long)job->offset, SEEK_SET);

	std::vector<char> buf(RC_FILE_CHUNK);
	unsigned long long offset = job->offset;
	unsigned int flags = RC_FF_BEGIN;
	int err = 0;

	for (;;)
	{
		if (!g_running || g_client_socket == INVALID_SOCKET) { err = 1; break; }
		size_t n = fread(buf.data(), 1, buf.size(), fp);
		if (n == 0) break;

		int bodyLen = (int)sizeof(FileChunk) + (int)n;
		std::vector<char> body(bodyLen);
		FileChunk c = { job->file_id, offset, (unsigned int)n, (unsigned int)flags };
		memcpy(body.data(), &c, sizeof(FileChunk));
		memcpy(body.data() + sizeof(FileChunk), buf.data(), n);

		if (!SendCmd(g_client_socket, CMD_FILE_DATA, body.data(), bodyLen))
		{
			err = 2;
			break;
		}
		offset += n;
		flags = RC_FF_DATA;
	}
	fclose(fp);

	FileChunk end = { job->file_id, offset, 0, err ? RC_FF_ERROR : RC_FF_END };
	SendCmd(g_client_socket, CMD_FILE_END, (char*)&end, sizeof(end));
	Log("download finished id=%u path=%s bytes=%llu err=%d",
		job->file_id, job->path.c_str(), offset, err);
	delete job;
	return 0;
}

static void HandleFileDownload(Packet* p)
{
	if (p->header.body_len < (int)sizeof(FileRequest))
		return;
	FileRequest req;
	memcpy(&req, p->body, sizeof(FileRequest));
	req.path[sizeof(req.path) - 1] = 0;

	if (!PathAllowed(req.path))
	{
		FileChunk err = { req.file_id, 0, 0, RC_FF_ERROR };
		SendCmd(g_client_socket, CMD_FILE_END, (char*)&err, sizeof(err));
		return;
	}

	DownloadJob* job = new DownloadJob();
	job->file_id = req.file_id;
	job->path = req.path;
	job->offset = req.offset;

	DWORD tid = 0;
	HANDLE h = CreateThread(NULL, 0, FileSendThread, job, 0, &tid);
	if (h != NULL)
		CloseHandle(h);
	else
	{
		delete job;
		Log("create download thread failed");
	}
}

/* ---------- 路径编码自适应 + 自动建目录 (v5 修复"无法创建文件") ----------
   收到的路径字节可能是 UTF-8(新客户端) 也可能是本机 ANSI/GBK(旧客户端/命令行),
   这里两种解释都算一遍, 谁的父目录真实存在就用谁, 否则按 UTF-8 处理并自动创建目录。
   这样中文/特殊字符文件名都能正确落盘, 也不会把 GBK 名字写成一堆乱码文件。 */
static bool Utf8ToWideStrict(const std::string& u, std::wstring& out)
{
	out.clear();
	if (u.empty()) return true;
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u.c_str(), (int)u.size(), NULL, 0);
	if (n <= 0) return false;
	out.resize((size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &out[0], n);
	return true;
}
static std::wstring AcpToWide(const std::string& a)
{
	if (a.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), NULL, 0);
	if (n <= 0) return std::wstring();
	std::wstring w((size_t)n, 0);
	MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), &w[0], n);
	return w;
}
static bool DirExistsW(const std::wstring& wdir)
{
	std::wstring d = wdir;
	if (d.empty()) return false;
	if (d.size() == 2 && d[1] == L':') d += L"\\";
	DWORD a = GetFileAttributesW(d.c_str());
	return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0);
}
static std::wstring ParentOf(const std::wstring& w)
{
	size_t k = w.find_last_of(L"\\/");
	if (k == std::wstring::npos) return std::wstring();
	if (k == 0) return std::wstring(L"\\");
	std::wstring d = w.substr(0, k);
	if (d.size() == 2 && d[1] == L':') d += L"\\";
	return d;
}
static std::wstring PathToWide(const std::string& p)
{
	std::wstring wU, wA = AcpToWide(p);
	if (!Utf8ToWideStrict(p, wU)) return wA;      /* 不是合法 UTF-8 → 当成本机 ANSI */
	if (wU == wA) return wU;                      /* 纯 ASCII → 两者相同 */
	if (ParentOf(wU) == ParentOf(wA)) return wU;
	if (DirExistsW(ParentOf(wU))) return wU;
	if (DirExistsW(ParentOf(wA))) return wA;
	return wU;
}
/* 逐级创建父目录(比如被控端不存在 D:\a\b\ 时自动建好) */
static void EnsureDirsW(const std::wstring& wpath)
{
	std::wstring dir = ParentOf(wpath);
	if (dir.empty()) return;
	for (size_t i = 0; i < dir.size(); ++i)
	{
		if ((dir[i] == L'\\' || dir[i] == L'/') && i > 0)
		{
			std::wstring sub = dir.substr(0, i);
			if (sub.size() == 2 && sub[1] == L':') continue;
			CreateDirectoryW(sub.c_str(), NULL);
		}
	}
	CreateDirectoryW(dir.c_str(), NULL);
}
/* 打开远端文件(写/读): 自动建父目录; 返回值同 fopen */
static FILE* OpenRemoteFile(const char* pathBytes, const wchar_t* mode)
{
	std::wstring w = PathToWide(pathBytes);
	FILE* fp = NULL;
	_wfopen_s(&fp, w.c_str(), mode);
	if (fp == NULL && mode[0] == L'w')
	{
		EnsureDirsW(w);                            /* 目录不存在 → 建好再试 */
		_wfopen_s(&fp, w.c_str(), mode);
	}
	return fp;
}

static std::string g_last_fallback_utf8;

/* 目标目录不可写(典型: C:\ 根目录, 需管理员)时的兜底: 改存到被控端桌面 */
static FILE* TryFallbackPath(const char* reqPath)
{
	std::wstring w = PathToWide(reqPath);
	if (w.empty())
		return NULL;
	size_t k = w.find_last_of(L"\\/");
	std::wstring name = (k == std::wstring::npos) ? w : w.substr(k + 1);
	if (name.empty())
		return NULL;
	wchar_t prof[MAX_PATH] = { 0 };
	if (!GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH))
		return NULL;
	std::wstring d = std::wstring(prof) + L"\\Desktop";
	DWORD a = GetFileAttributesW(d.c_str());
	if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
	{
		std::wstring d2 = std::wstring(prof) + L"\\OneDrive\\Desktop";
		DWORD a2 = GetFileAttributesW(d2.c_str());
		if (a2 != INVALID_FILE_ATTRIBUTES && (a2 & FILE_ATTRIBUTE_DIRECTORY))
			d = d2;
		else
			CreateDirectoryW((std::wstring(prof) + L"\\Desktop").c_str(), NULL);
	}
	std::wstring full = d + L"\\" + name;
	FILE* fp = NULL;
	_wfopen_s(&fp, full.c_str(), L"wb");
	if (fp == NULL)
		return NULL;
	int need = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, NULL, 0, NULL, NULL);
	if (need > 0)
	{
		std::string u;
		u.resize((size_t)need);
		WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, &u[0], need, NULL, NULL);
		while (!u.empty() && u[u.size() - 1] == 0)
			u.erase(u.size() - 1);
		g_last_fallback_utf8 = u;
	}
	return fp;
}

static void HandleFileUploadBegin(Packet* p)
{
	if (p->header.body_len < (int)sizeof(FileRequest))
		return;
	FileRequest req;
	memcpy(&req, p->body, sizeof(FileRequest));
	req.path[sizeof(req.path) - 1] = 0;

	auto sendErr = [&](const char* why)
	{
		FileChunk err = { req.file_id, 0, 0, RC_FF_ERROR };
		SendCmd(g_client_socket, CMD_FILE_END, (char*)&err, sizeof(err));
		SendText(g_client_socket, CMD_FILE_PROGRESS, std::string("上传失败: ") + why);
		Log("upload begin failed(%s): %s", why, req.path);
	};

	if (!PathAllowed(req.path)) { sendErr("路径越权"); return; }

	FILE* fp = NULL;
	errno = 0;
	fp = OpenRemoteFile(req.path, (req.offset > 0) ? L"r+b" : L"wb");
	if (fp == NULL)
	{
		const int e = errno;
		/* v6: 目标目录不可写 → 自动改存到被控端桌面, 并把真实位置回报给客户端 */
		if (e == EACCES || e == EPERM || e == ENOENT)
		{
			FILE* fb = TryFallbackPath(req.path);
			if (fb != NULL)
			{
				if (req.offset > 0)
					_fseeki64(fb, (long long)req.offset, SEEK_SET);
				g_uploads[req.file_id] = fb;
				SendText(g_client_socket, CMD_FILE_PROGRESS,
					std::string("上传提示: 原目标不可写, 已自动改存到被控端 ") + g_last_fallback_utf8);
				Log("upload fallback -> %s", g_last_fallback_utf8.c_str());
				return;
			}
		}
		const char* why = "无法创建文件";
		if (e == EACCES) why = "无权限写入该目录(被控端试试以管理员运行, 或换到 D:\\ 或桌面等可写目录)";
		else if (e == ENOENT) why = "父目录不存在且创建失败";
		char m[320];
		_snprintf_s(m, _TRUNCATE, "%s [errno=%d, lasterr=%lu]", why, e, GetLastError());
		sendErr(m);
		return;
	}
	if (req.offset > 0)
		_fseeki64(fp, (long long)req.offset, SEEK_SET);

	g_uploads[req.file_id] = fp;
	Log("upload begin id=%u -> %s (offset=%llu)", req.file_id, req.path, req.offset);
}

static void HandleFileData(Packet* p)
{
	if (p->header.body_len < (int)sizeof(FileChunk))
		return;
	FileChunk c;
	memcpy(&c, p->body, sizeof(FileChunk));

	std::map<unsigned int, FILE*>::iterator it = g_uploads.find(c.file_id);
	if (it == g_uploads.end() || it->second == NULL)
		return;
	int payload = p->header.body_len - (int)sizeof(FileChunk);
	if (payload > 0)
		fwrite(p->body + sizeof(FileChunk), 1, (size_t)payload, it->second);
}

static void HandleFileEnd(Packet* p)
{
	if (p->header.body_len < (int)sizeof(FileChunk))
		return;
	FileChunk c;
	memcpy(&c, p->body, sizeof(FileChunk));

	std::map<unsigned int, FILE*>::iterator it = g_uploads.find(c.file_id);
	if (it != g_uploads.end())
	{
		if (it->second != NULL)
		{
			fflush(it->second);
			fclose(it->second);
		}
		g_uploads.erase(it);
	}

	char msg[128];
	_snprintf_s(msg, _TRUNCATE, "上传完成 file_id=%u 共 %llu 字节", c.file_id, c.offset);
	SendText(g_client_socket, CMD_FILE_PROGRESS, msg);
	Log("upload finished id=%u bytes=%llu", c.file_id, c.offset);
}

static void HandleFileOp(Packet* p)
{
	if (p->header.body_len < (int)sizeof(FileOp))
		return;
	FileOp op;
	memcpy(&op, p->body, sizeof(FileOp));
	op.path1[sizeof(op.path1) - 1] = 0;
	op.path2[sizeof(op.path2) - 1] = 0;

	std::string res = "ok";
	if (!PathAllowed(op.path1) || (op.path2[0] && !PathAllowed(op.path2)))
	{
		res = "err:路径越权";
	}
	else
	{
		switch (op.op)
		{
		case RC_OP_DELETE:
		{
			const std::wstring w1 = PathToWide(op.path1);
			if (DeleteFileW(w1.c_str()))
			{
				if (!RemoveDirectoryW(w1.c_str())) res = "ok";  /* 文件或空目录 */
			}
			else if (RemoveDirectoryW(w1.c_str()))
			{
				res = "ok";
			}
			else
			{
				char m[160];
				_snprintf_s(m, _TRUNCATE, "err:删除失败(%lu)", GetLastError());
				res = m;
			}
			break;
		}
		case RC_OP_MKDIR:
		{
			EnsureDirsW(PathToWide(op.path1));   /* 逐级补齐父目录 */
			if (!CreateDirectoryW(PathToWide(op.path1).c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
			{
				char m[160];
				_snprintf_s(m, _TRUNCATE, "err:创建目录失败(%lu)", GetLastError());
				res = m;
			}
			break;
		}
		case RC_OP_RENAME:
		{
			if (!MoveFileW(PathToWide(op.path1).c_str(), PathToWide(op.path2).c_str()))
			{
				char m[160];
				_snprintf_s(m, _TRUNCATE, "err:重命名失败(%lu)", GetLastError());
				res = m;
			}
			break;
		}
		default:
			res = "err:未知文件操作";
			break;
		}
	}
	Log("file op %d: %s -> %s", op.op, op.path1, res.c_str());
	SendText(g_client_socket, CMD_FILE_OP_RES, res);
}

/* ============================================================
 *  命令分发
 * ============================================================ */
volatile LONG g_session_active = 1;

/* ============================================================
 *  远程控制扩展 v4: 定位 / 键鼠录制 / 剪贴板 / 系统信息
 * ============================================================ */
static std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
	if (n <= 0) return std::string();
	std::string s((size_t)n, 0);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
	return s;
}
static std::string AnsiToUtf8(const std::string& a)
{
	if (a.empty()) return std::string();
	int n = MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), NULL, 0);
	if (n <= 0) return std::string();
	std::wstring w((size_t)n, 0);
	MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), &w[0], n);
	return WideToUtf8(w);
}
static std::string Utf8ToAnsi(const std::string& u)
{
	if (u.empty()) return std::string();
	int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), NULL, 0);
	if (n <= 0) return std::string();
	std::wstring w((size_t)n, 0);
	MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
	int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
	if (m <= 0) return std::string();
	std::string a((size_t)m, 0);
	WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &a[0], m, NULL, NULL);
	return a;
}

/* ---------------- 定位: 查询被控端光标位置 ---------------- */
static void HandleCursor(Packet* p)
{
	(void)p;
	POINT pt;
	GetCursorPos(&pt);
	CursorInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.x = pt.x;                                   /* 与屏幕采集同一坐标系(主屏左上为原点) */
	ci.y = pt.y;
	ci.screen_w = GetSystemMetrics(SM_CXSCREEN);
	ci.screen_h = GetSystemMetrics(SM_CYSCREEN);
	SendCmd(g_client_socket, CMD_CURSOR_RES, (char*)&ci, sizeof(ci));
}

/* ---------------- 键鼠录制 ---------------- */
static CRITICAL_SECTION g_rec_lock;
static bool             g_rec_lock_ready = false;
static volatile LONG    g_rec_active = 0;
static volatile LONG    g_rec_stop = 0;
static std::vector<std::string> g_rec_lines;
static InputRecReq      g_rec_req = { 0, 0, 15, 0 };

static const char* VkName(int vk)
{
	static char buf[32];
	if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
	if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
	if (vk >= VK_F1 && vk <= VK_F24) { _snprintf_s(buf, _TRUNCATE, "F%d", vk - VK_F1 + 1); return buf; }
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { _snprintf_s(buf, _TRUNCATE, "小键盘%d", vk - VK_NUMPAD0); return buf; }
	switch (vk)
	{
	case VK_SPACE: return "空格";
	case VK_RETURN: return "回车";
	case VK_TAB: return "Tab";
	case VK_ESCAPE: return "Esc";
	case VK_BACK: return "退格";
	case VK_DELETE: return "Delete";
	case VK_LSHIFT: case VK_SHIFT: return "Shift";
	case VK_RSHIFT: return "右Shift";
	case VK_LCONTROL: case VK_CONTROL: return "Ctrl";
	case VK_RCONTROL: return "右Ctrl";
	case VK_LMENU: case VK_MENU: return "Alt";
	case VK_RMENU: return "右Alt";
	case VK_LWIN: return "Win";
	case VK_RWIN: return "右Win";
	case VK_CAPITAL: return "大写锁定";
	case VK_LEFT: return "左方向";
	case VK_RIGHT: return "右方向";
	case VK_UP: return "上方向";
	case VK_DOWN: return "下方向";
	case VK_HOME: return "Home";
	case VK_END: return "End";
	case VK_PRIOR: return "PgUp";
	case VK_NEXT: return "PgDn";
	case VK_INSERT: return "Insert";
	case VK_SNAPSHOT: return "PrintScreen";
	case VK_OEM_1: return ";";
	case VK_OEM_2: return "/";
	case VK_OEM_3: return "`";
	case VK_OEM_4: return "[";
	case VK_OEM_5: return "\\";
	case VK_OEM_6: return "]";
	case VK_OEM_7: return "'";
	case VK_OEM_PLUS: return "=";
	case VK_OEM_MINUS: return "-";
	case VK_OEM_COMMA: return ",";
	case VK_OEM_PERIOD: return ".";
	}
	_snprintf_s(buf, _TRUNCATE, "VK%02X", vk);
	return buf;
}

static void RecPush(const char* line)
{
	if (!g_rec_lock_ready) return;
	EnterCriticalSection(&g_rec_lock);
	if (g_rec_lines.size() < 400000) g_rec_lines.push_back(line);
	LeaveCriticalSection(&g_rec_lock);
}

/* 录制结束后把记录分块发回控制端 */
static void RecSendAll()
{
	std::vector<std::string> lines;
	EnterCriticalSection(&g_rec_lock);
	lines.swap(g_rec_lines);
	LeaveCriticalSection(&g_rec_lock);

	std::string all;
	all.reserve(lines.size() * 32 + 64);
	{
		char head[160];
		_snprintf_s(head, _TRUNCATE, "# 录制结束: 共 %d 个事件\r\n", (int)lines.size());
		all += head;
	}
	for (size_t i = 0; i < lines.size(); i++) { all += lines[i]; all += "\r\n"; }

	const size_t CHUNK = 48 * 1024;
	int index = 0;
	for (size_t off = 0; off < all.size(); off += CHUNK)
	{
		size_t n = all.size() - off;
		if (n > CHUNK) n = CHUNK;
		InputLog h;
		h.index = index++;
		h.last = (off + n >= all.size()) ? 1 : 0;
		std::vector<char> body(sizeof(InputLog) + n);
		memcpy(body.data(), &h, sizeof(h));
		memcpy(body.data() + sizeof(h), all.data() + off, n);
		SendCmd(g_client_socket, CMD_INPUT_LOG, body.data(), (int)body.size());
		Sleep(2);
	}
	Log("record log sent: %d bytes / %d chunk(s)", (int)all.size(), index);
}

DWORD WINAPI InputRecThread(LPVOID param)
{
	(void)param;
	const int flags = g_rec_req.flags;
	const int duration = g_rec_req.duration_ms;
	int sample = g_rec_req.sample_ms;
	if (sample < 5 || sample > 500) sample = 15;

	DWORD t0 = GetTickCount();
	POINT prev;
	GetCursorPos(&prev);
	bool prevDown[256];
	for (int i = 0; i < 256; i++) prevDown[i] = (GetAsyncKeyState(i) & 0x8000) != 0;

	static const int   btnVk[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
	static const char* btnName[3] = { "left", "right", "middle" };
	char buf[192];

	while (g_running && InterlockedCompareExchange(&g_rec_stop, 0, 0) == 0)
	{
		Sleep(sample);
		DWORD t = GetTickCount() - t0;
		POINT pt;
		GetCursorPos(&pt);

		if (flags & RC_REC_MOUSE)
		{
			if (pt.x != prev.x || pt.y != prev.y)
			{
				_snprintf_s(buf, _TRUNCATE, "t=%lu\tmouse\tmove\t%ld\t%ld", (unsigned long)t, pt.x, pt.y);
				RecPush(buf);
				prev = pt;
			}
			for (int b = 0; b < 3; b++)
			{
				bool down = (GetAsyncKeyState(btnVk[b]) & 0x8000) != 0;
				if (down != prevDown[btnVk[b]])
				{
					_snprintf_s(buf, _TRUNCATE, "t=%lu\tmouse\t%s\t%s\t%ld\t%ld", (unsigned long)t,
						btnName[b], down ? "down" : "up", pt.x, pt.y);
					RecPush(buf);
					prevDown[btnVk[b]] = down;
				}
			}
		}
		if (flags & RC_REC_KEY)
		{
			for (int vk = 8; vk < 256; vk++)
			{
				if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
				bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
				if (down != prevDown[vk])
				{
					_snprintf_s(buf, _TRUNCATE, "t=%lu\tkey\t%02X\t%s\t%s", (unsigned long)t, vk,
						down ? "down" : "up", VkName(vk));
					RecPush(buf);
					prevDown[vk] = down;
				}
			}
		}
		if (duration > 0 && (int)t >= duration) break;
	}

	InterlockedExchange(&g_rec_active, 0);
	InterlockedExchange(&g_rec_stop, 0);
	RecSendAll();
	return 0;
}

static void HandleInputRec(Packet* p)
{
	InputRecReq req;
	memset(&req, 0, sizeof(req));
	if (p->header.body_len >= (int)sizeof(InputRecReq))
		memcpy(&req, p->body, sizeof(InputRecReq));

	if (!g_rec_lock_ready)
	{
		InitializeCriticalSection(&g_rec_lock);
		g_rec_lock_ready = true;
	}

	if (req.stop)                                  /* 手动停止 */
	{
		if (g_rec_active) { InterlockedExchange(&g_rec_stop, 1); Log("input record: stop requested"); }
		else SendText(g_client_socket, CMD_INPUT_LOG, "# 当前没有正在进行的录制\r\n");
		return;
	}
	if (req.flags == 0)                            /* 仅查询状态 */
	{
		char m[128];
		_snprintf_s(m, _TRUNCATE, "# 录制状态: %s\r\n", g_rec_active ? "进行中" : "空闲");
		SendText(g_client_socket, CMD_INPUT_LOG, m);
		return;
	}
	if (g_rec_active)
	{
		SendText(g_client_socket, CMD_INPUT_LOG, "# 已有录制在进行中，请先停止\r\n");
		return;
	}

	g_rec_req = req;
	InterlockedExchange(&g_rec_active, 1);
	InterlockedExchange(&g_rec_stop, 0);

	DWORD tid = 0;
	HANDLE th = CreateThread(NULL, 0, InputRecThread, NULL, 0, &tid);
	if (th == NULL)
	{
		InterlockedExchange(&g_rec_active, 0);
		SendText(g_client_socket, CMD_INPUT_LOG, "# 录制线程创建失败\r\n");
		return;
	}
	CloseHandle(th);

	char m[200];
	_snprintf_s(m, _TRUNCATE, "# 开始录制 %s%s  时长 %d ms  采样 %d ms\r\n",
		(req.flags & RC_REC_MOUSE) ? "[鼠标]" : "",
		(req.flags & RC_REC_KEY) ? "[键盘]" : "",
		req.duration_ms, (req.sample_ms >= 5) ? req.sample_ms : 15);
	SendText(g_client_socket, CMD_INPUT_LOG, m);
	Log("input record: start flags=%d duration=%d", req.flags, req.duration_ms);
}

/* ============================================================
 *  v9: 剪贴板同步虚拟通道 (CMD_CLIP_CHANNEL)
 *  状态机: HELLO(能力) → OFFER(本端剪贴板变化) → REQUEST → DATA(分块)
 *  文本/位图即时取; CF_HDROP 文件只先发清单(延迟渲染), 请求时再按 64KB 分块读取。
 * ============================================================ */
#define RC_CLIP_CHUNK   (64 * 1024)
#define RC_CLIP_MAXMEM  (16 * 1024 * 1024)   /* 文本/位图单次上限 */

static std::wstring Utf8ToWideS(const std::string& u)
{
	if (u.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), NULL, 0);
	if (n <= 0) return std::wstring();
	std::wstring w((size_t)n, 0);
	MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
	return w;
}

static void ClipChSend(int msg, int fmt, unsigned int seq, unsigned long long offset,
	unsigned long long size, int last, int count, const void* extra, int extraLen)
{
	ClipMsg m;
	memset(&m, 0, sizeof(m));
	m.msg = msg; m.fmt = fmt; m.seq = seq; m.offset = offset;
	m.size = size; m.last = last; m.count = count;
	std::string body((const char*)&m, sizeof(m));
	if (extra != NULL && extraLen > 0)
		body.append((const char*)extra, (size_t)extraLen);
	SendText(g_client_socket, CMD_CLIP_CHANNEL, body);
	Log("clip: send msg=%d fmt=0x%x seq=%u off=%llu size=%llu cnt=%d len=%d",
		msg, fmt, seq, offset, size, count, (int)body.size());
}

/* ---------------- 读本机剪贴板 ---------------- */
static bool ClipLocalText(std::string& out, int& fmtOut)
{
	out.clear();
	fmtOut = 0;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT) && !IsClipboardFormatAvailable(CF_TEXT))
		return false;
	if (!OpenClipboard(NULL)) return false;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h != NULL)
	{
		const wchar_t* p = (const wchar_t*)GlobalLock(h);
		if (p != NULL && p[0] != L'\0') { out = WideToUtf8(p); fmtOut = RC_CF_UNICODE; }
		if (p != NULL) GlobalUnlock(h);
	}
	else if ((h = GetClipboardData(CF_TEXT)) != NULL)
	{
		const char* p = (const char*)GlobalLock(h);
		if (p != NULL && p[0] != '\0') { out = AnsiToUtf8(p); fmtOut = RC_CF_TEXT; }
		if (p != NULL) GlobalUnlock(h);
	}
	CloseClipboard();
	return !out.empty();
}

/* 本机剪贴板位图 → PNG 字节(优先应用已放好的注册格式 "PNG", 否则 DIB 经 GDI+ 转码) */
static bool ClipLocalPng(std::string& out)
{
	out.clear();
	UINT cf = RegisterClipboardFormatA("PNG");
	if (!OpenClipboard(NULL)) return false;
	if (cf != 0 && IsClipboardFormatAvailable(cf))
	{
		HANDLE h = GetClipboardData(cf);
		if (h != NULL)
		{
			const char* p = (const char*)GlobalLock(h);
			SIZE_T n = GlobalSize(h);
			if (p != NULL && n > 8) out.assign(p, n);
			if (p != NULL) GlobalUnlock(h);
		}
	}
	if (out.empty() && IsClipboardFormatAvailable(CF_DIB))
	{
		HANDLE h = GetClipboardData(CF_DIB);
		if (h != NULL)
		{
			BITMAPINFO* bi = (BITMAPINFO*)GlobalLock(h);
			if (bi != NULL && bi->bmiHeader.biSizeImage > 0 && bi->bmiHeader.biSizeImage < RC_CLIP_MAXMEM)
			{
				HDC hdc = GetDC(NULL);
				void* bits = NULL;
				HBITMAP hb = CreateDIBSection(hdc, bi, DIB_RGB_COLORS, &bits, NULL, 0);
				if (hb != NULL && bits != NULL)
				{
					size_t pal = 0;
					if (bi->bmiHeader.biBitCount <= 8)
						pal = (size_t)(bi->bmiHeader.biClrUsed ? bi->bmiHeader.biClrUsed : (1u << bi->bmiHeader.biBitCount)) * sizeof(RGBQUAD);
					memcpy(bits, (const char*)bi + bi->bmiHeader.biSize + pal, bi->bmiHeader.biSizeImage);
					CImage img;
					img.Attach(hb);
					IStream* st = NULL;
					if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &st)) && st != NULL)
					{
						if (SUCCEEDED(img.Save(st, Gdiplus::ImageFormatPNG)))
						{
							STATSTG ss;
							memset(&ss, 0, sizeof(ss));
							if (SUCCEEDED(st->Stat(&ss, STATFLAG_NONAME)) && ss.cbSize.QuadPart > 0 && ss.cbSize.QuadPart < RC_CLIP_MAXMEM)
							{
								HGLOBAL hg = NULL;
								if (SUCCEEDED(GetHGlobalFromStream(st, &hg)) && hg != NULL)
								{
									const char* p = (const char*)GlobalLock(hg);
									if (p != NULL) out.assign(p, (size_t)ss.cbSize.QuadPart);
									GlobalUnlock(hg);
								}
							}
						}
						st->Release();
					}
					img.Detach();
				}
				if (hb != NULL) DeleteObject(hb);
				ReleaseDC(NULL, hdc);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return !out.empty();
}

/* 本机剪贴板文件清单(CF_HDROP) */
static bool ClipLocalFiles(std::vector<ClipFileDesc>& out)
{
	out.clear();
	if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
	if (!OpenClipboard(NULL)) return false;
	HANDLE h = GetClipboardData(CF_HDROP);
	if (h != NULL)
	{
		HDROP dp = (HDROP)h;
		UINT n = DragQueryFileW(dp, 0xFFFFFFFF, NULL, 0);
		for (UINT i = 0; i < n; i++)
		{
			wchar_t path[600] = { 0 };
			if (DragQueryFileW(dp, i, path, 600) == 0) continue;
			ClipFileDesc d;
			memset(&d, 0, sizeof(d));
			std::string u8 = WideToUtf8(path);
			strncpy_s(d.name, u8.c_str(), _TRUNCATE);
			WIN32_FILE_ATTRIBUTE_DATA fa;
			if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa))
			{
				d.size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
				d.is_dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1u : 0u;
			}
			out.push_back(d);
		}
	}
	CloseClipboard();
	return !out.empty();
}

/* ---------------- 上报本机剪贴板变化 ---------------- */
static void ClipOfferSelf()
{
	if (InterlockedCompareExchange(&g_clip_ready, 0, 0) == 0) return;
	if (InterlockedCompareExchange(&g_clip_auto, 0, 0) == 0) return;
	if (InterlockedCompareExchange(&g_clip_hold, 0, 0) != 0) return;

	std::string text, png;
	int fmt = 0;
	std::vector<ClipFileDesc> files;
	unsigned long long size = 0;
	int count = 0;

	if (ClipLocalText(text, fmt)) size = text.size();
	else if (ClipLocalPng(png)) { fmt = RC_CF_PNG; size = png.size(); }
	else if (ClipLocalFiles(files))
	{
		fmt = RC_CF_FILES;
		count = (int)files.size();
		for (size_t i = 0; i < files.size(); i++) size += files[i].size;
	}
	if (fmt == 0) { Log("clip: local clipboard has no supported format"); return; }

	EnterCriticalSection(&g_clip_lock);
	g_clip_peer_files.clear();
	LeaveCriticalSection(&g_clip_lock);

	ClipChSend(RC_CM_OFFER, fmt, 0, 0, size, 0, count,
		files.empty() ? NULL : (const void*)files.data(),
		(int)(files.size() * sizeof(ClipFileDesc)));
}

/* ---------------- 剪贴板 ---------------- */
static void SendClipRes(int op, const std::string& text)
{
	ClipReq r;
	memset(&r, 0, sizeof(r));
	r.op = op;
	std::string body((const char*)&r, sizeof(r));
	body += text;
	SendText(g_client_socket, CMD_CLIPBOARD_RES, body);
}

/* -------- v9 剪贴板通道: 写入本机剪贴板(对端 → 本机) -------- */
static void ClipTouchSeq()
{
	g_clip_last_seq = GetClipboardSequenceNumber();   /* 抑制本端写入触发的上报(防回环) */
}

static void ClipApplyText(const std::string& utf8)
{
	if (utf8.empty()) return;
	std::wstring w = Utf8ToWideS(utf8);
	size_t bytes = (w.size() + 1) * sizeof(wchar_t);
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hg == NULL) return;
	void* p = GlobalLock(hg);
	if (p == NULL) { GlobalFree(hg); return; }
	memcpy(p, w.c_str(), bytes);
	GlobalUnlock(hg);
	if (!OpenClipboard(NULL)) { GlobalFree(hg); return; }
	EmptyClipboard();
	if (SetClipboardData(CF_UNICODETEXT, hg) == NULL) GlobalFree(hg);
	CloseClipboard();
	ClipTouchSeq();
	Log("clip: local clipboard <- text (%d bytes utf8)", (int)utf8.size());
}

static void ClipApplyPng(const std::string& png)
{
	if (png.size() < 8) return;
	UINT cfPng = RegisterClipboardFormatA("PNG");
	HBITMAP hbmp = NULL;
	std::string dib;
	IStream* st = NULL;
	if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &st)) && st != NULL)
	{
		ULONG wr = 0;
		st->Write(png.data(), (ULONG)png.size(), &wr);
		LARGE_INTEGER li; li.QuadPart = 0;
		st->Seek(li, STREAM_SEEK_SET, NULL);
		CImage img;
		if (SUCCEEDED(img.Load(st)))
		{
			hbmp = img.Detach();
			BITMAP bm; memset(&bm, 0, sizeof(bm));
			if (hbmp != NULL && GetObject(hbmp, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
			{
				BITMAPINFOHEADER bh; memset(&bh, 0, sizeof(bh));
				bh.biSize = sizeof(bh);
				bh.biWidth = bm.bmWidth;
				bh.biHeight = -bm.bmHeight;
				bh.biPlanes = 1; bh.biBitCount = 32; bh.biCompression = BI_RGB;
				bh.biSizeImage = (DWORD)((size_t)bm.bmWidth * (size_t)bm.bmHeight * 4);
				dib.assign((const char*)&bh, sizeof(bh));
				dib.resize(sizeof(bh) + bh.biSizeImage);
				HDC hdc = GetDC(NULL);
				GetDIBits(hdc, hbmp, 0, (UINT)bm.bmHeight, &dib[sizeof(BITMAPINFOHEADER)], (BITMAPINFO*)&dib[0], DIB_RGB_COLORS);
				ReleaseDC(NULL, hdc);
			}
		}
		st->Release();
	}
	if (!OpenClipboard(NULL)) { if (hbmp != NULL) DeleteObject(hbmp); return; }
	EmptyClipboard();
	if (cfPng != 0)
	{
		HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, png.size());
		if (hg != NULL)
		{
			void* p = GlobalLock(hg);
			if (p != NULL) { memcpy(p, png.data(), png.size()); GlobalUnlock(hg); if (SetClipboardData(cfPng, hg) == NULL) GlobalFree(hg); }
			else GlobalFree(hg);
		}
	}
	if (!dib.empty())
	{
		HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, dib.size());
		if (hg != NULL)
		{
			void* p = GlobalLock(hg);
			if (p != NULL) { memcpy(p, dib.data(), dib.size()); GlobalUnlock(hg); if (SetClipboardData(CF_DIB, hg) == NULL) GlobalFree(hg); }
			else GlobalFree(hg);
		}
	}
	if (hbmp != NULL && SetClipboardData(CF_BITMAP, hbmp) == NULL) DeleteObject(hbmp);
	CloseClipboard();
	ClipTouchSeq();
	Log("clip: local clipboard <- png (%d bytes)", (int)png.size());
}

/* CF_HDROP 的 DROPFILES 头(自行定义, 避免依赖 shellapi.h 的条件编译) */
#pragma pack(push,1)
struct RcDropFiles
{
	DWORD pFiles;
	POINT pt;
	BOOL  fNC;
	BOOL  fWide;
};
#pragma pack(pop)

static void ClipApplyFiles(const std::vector<std::string>& paths)
{
	if (paths.empty()) return;
	std::wstring list;
	for (size_t i = 0; i < paths.size(); i++)
	{
		list += Utf8ToWideS(paths[i]);
		list.push_back(L'\0');
	}
	size_t bytes = sizeof(RcDropFiles) + (list.size() + 1) * sizeof(wchar_t);
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
	if (hg == NULL) return;
	void* p = GlobalLock(hg);
	if (p == NULL) { GlobalFree(hg); return; }
	RcDropFiles* df = (RcDropFiles*)p;
	df->pFiles = sizeof(RcDropFiles);
	df->fWide = TRUE;
	memcpy((char*)p + sizeof(RcDropFiles), list.c_str(), (list.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(hg);
	if (!OpenClipboard(NULL)) { GlobalFree(hg); return; }
	EmptyClipboard();
	if (SetClipboardData(CF_HDROP, hg) == NULL) GlobalFree(hg);
	CloseClipboard();
	ClipTouchSeq();
	Log("clip: local clipboard <- %d files", (int)paths.size());
}

/* -------- v9 剪贴板通道: 发送本机内容(响应 REQUEST) -------- */
static void ClipSendAbort(int fmt, unsigned int seq, const char* why)
{
	ClipChSend(RC_CM_ABORT, fmt, seq, 0, 0, 1, 0, why, (int)strlen(why));
}

static void ClipSendFileChunk(unsigned int idx, unsigned long long offset)
{
	std::vector<ClipFileDesc> files;
	if (!ClipLocalFiles(files)) { ClipSendAbort(RC_CF_FILES, idx, "local clipboard is not file list"); return; }
	if (idx >= files.size()) { ClipSendAbort(RC_CF_FILES, idx, "file index out of range"); return; }
	if (files[idx].is_dir != 0) { ClipSendAbort(RC_CF_FILES, idx, "directory not supported"); return; }
	std::wstring wp = Utf8ToWideS(files[idx].name);
	FILE* fp = NULL;
	if (_wfopen_s(&fp, wp.c_str(), L"rb") != 0 || fp == NULL) { ClipSendAbort(RC_CF_FILES, idx, "open file failed"); return; }
	_fseeki64(fp, (long long)offset, SEEK_SET);
	std::vector<char> buf(RC_CLIP_CHUNK);
	size_t n = fread(&buf[0], 1, RC_CLIP_CHUNK, fp);
	fclose(fp);
	int last = ((offset + n) >= files[idx].size) ? 1 : 0;
	ClipChSend(RC_CM_DATA, RC_CF_FILES, idx, offset, files[idx].size, last, (int)files.size(),
		(n > 0) ? &buf[0] : NULL, (int)n);
}

static void ClipSendLocalContent(int fmt)
{
	std::string data;
	if (fmt == RC_CF_PNG)
	{
		if (!ClipLocalPng(data)) { ClipSendAbort(RC_CF_PNG, 0, "no image in local clipboard"); return; }
	}
	else
	{
		int f = 0;
		if (!ClipLocalText(data, f)) { ClipSendAbort(RC_CF_TEXT, 0, "no text in local clipboard"); return; }
	}
	if (data.size() > RC_CLIP_MAXMEM) { ClipSendAbort(fmt, 0, "content too large"); return; }
	ClipChSend(RC_CM_DATA, fmt, 0, 0, data.size(), 1, 0, data.data(), (int)data.size());
}

static std::string ClipBaseName(const std::string& p)
{
	size_t k = p.find_last_of('/');
	size_t k2 = p.find_last_of((char)92);
	if (k2 != std::string::npos && (k == std::string::npos || k2 > k)) k = k2;
	return (k == std::string::npos) ? p : p.substr(k + 1);
}

static void EnsureDirUtf8(const std::string& d)
{
	std::wstring w = Utf8ToWideS(d);
	for (size_t i = 0; i < w.size(); i++)
	{
		if (w[i] == (wchar_t)92 || w[i] == (wchar_t)47)
		{
			if (i > 2) CreateDirectoryW(w.substr(0, i).c_str(), NULL);
		}
	}
	CreateDirectoryW(w.c_str(), NULL);
}

/* -------- v9 剪贴板通道: 接收状态机(对端 → 本机) -------- */
static void ClipInReset()
{
	if (g_clip_in_fp != NULL) { fclose(g_clip_in_fp); g_clip_in_fp = NULL; }
	g_clip_in_active = false;
	g_clip_in_idx = 0;
	g_clip_in_count = 0;
	g_clip_in_got = 0;
	g_clip_in_paths.clear();
	g_clip_in_desc.clear();
}

static void ClipBeginRecvFiles()
{
	char tmp[MAX_PATH + 1] = { 0 };
	if (GetTempPathA(MAX_PATH, tmp) == 0) return;
	char dir[MAX_PATH * 2] = { 0 };
	sprintf_s(dir, "%sRCClipIn/%u_%u", tmp, (unsigned)GetCurrentProcessId(), (unsigned)GetTickCount());
	std::string sdir = std::string(dir) + "/";
	EnsureDirUtf8(sdir);
	ClipInReset();
	EnterCriticalSection(&g_clip_lock);
	g_clip_in_desc = g_clip_peer_files;
	LeaveCriticalSection(&g_clip_lock);
	g_clip_in_dir = dir;
	g_clip_in_active = true;
	g_clip_in_count = (int)g_clip_in_desc.size();
	g_clip_in_idx = 0;
	InterlockedExchange(&g_clip_hold, 1);          /* 接收期间抑制上报, 防回环 */
	ClipChSend(RC_CM_LOCK, RC_CF_FILES, 0, 0, 0, 0, 0, NULL, 0);
	ClipChSend(RC_CM_REQUEST, RC_CF_FILES, 0, 0, 0, 0, g_clip_in_count, NULL, 0);
	Log("clip: recv files started dir=%s count=%d", dir, g_clip_in_count);
}

static void ClipHandleRecvData(const ClipMsg& m, const char* extra, int extraLen)
{
	if (m.fmt == RC_CF_FILES)
	{
		if (!g_clip_in_active) return;
		if (m.seq != g_clip_in_idx)
		{
			Log("clip: data seq mismatch got=%u expect=%u", m.seq, g_clip_in_idx);
			return;
		}
		if (g_clip_in_fp == NULL)
		{
			std::string base = ClipBaseName(g_clip_in_desc[m.seq].name);
			if (base.empty()) base = "file.bin";
			std::string full = g_clip_in_dir + "/" + base;
			g_clip_in_paths.push_back(full);
			std::wstring wf = Utf8ToWideS(full);
			if (_wfopen_s(&g_clip_in_fp, wf.c_str(), L"wb") != 0) g_clip_in_fp = NULL;
			g_clip_in_got = 0;
		}
		if (g_clip_in_fp != NULL && extraLen > 0)
		{
			fwrite(extra, 1, (size_t)extraLen, g_clip_in_fp);
			g_clip_in_got += (unsigned long long)extraLen;
		}
		if (m.last)
		{
			if (g_clip_in_fp != NULL) { fclose(g_clip_in_fp); g_clip_in_fp = NULL; }
			unsigned int next = m.seq + 1;
			if ((int)next < g_clip_in_count)
			{
				g_clip_in_idx = next;
				ClipChSend(RC_CM_REQUEST, RC_CF_FILES, next, 0, 0, 0, g_clip_in_count, NULL, 0);
			}
			else
			{
				int got = (int)g_clip_in_paths.size();
				ClipApplyFiles(g_clip_in_paths);
				ClipInReset();
				InterlockedExchange(&g_clip_hold, 0);
				ClipChSend(RC_CM_UNLOCK, RC_CF_FILES, 0, 0, 0, 0, 0, NULL, 0);
				Log("clip: %d files received -> local clipboard", got);
			}
		}
		return;
	}
	if (extraLen <= 0) return;
	std::string data(extra, (size_t)extraLen);
	if (m.fmt == RC_CF_PNG) ClipApplyPng(data);
	else ClipApplyText(data);
}

static void HandleClipChannel(Packet* p)
{
	if (p->header.body_len < (int)sizeof(ClipMsg)) return;
	ClipMsg m;
	memcpy(&m, p->body, sizeof(m));
	const char* extra = p->body + sizeof(ClipMsg);
	int extraLen = p->header.body_len - (int)sizeof(ClipMsg);
	if (extraLen < 0) extraLen = 0;

	switch (m.msg)
	{
	case RC_CM_HELLO:
	{
		g_clip_peer_fmt = m.fmt;
		InterlockedExchange(&g_clip_auto, (m.flags & 1) ? 1 : 0);
		InterlockedExchange(&g_clip_ready, 1);
		ClipTouchSeq();
		Log("clip: hello peer fmt=0x%x autosync=%d", m.fmt, (m.flags & 1));
		ClipChSend(RC_CM_HELLO, RC_CF_TEXT | RC_CF_UNICODE | RC_CF_PNG | RC_CF_FILES | RC_CF_DIB,
			0, 0, 0, 0, 0, NULL, 0);
		ClipOfferSelf();
		break;
	}
	case RC_CM_OFFER:
	{
		EnterCriticalSection(&g_clip_lock);
		g_clip_peer_offer = m;
		g_clip_peer_files.clear();
		if ((m.fmt & RC_CF_FILES) && m.count > 0 && extraLen >= (int)sizeof(ClipFileDesc))
		{
			int cnt = m.count;
			if (extraLen < cnt * (int)sizeof(ClipFileDesc)) cnt = extraLen / (int)sizeof(ClipFileDesc);
			const ClipFileDesc* d = (const ClipFileDesc*)extra;
			for (int i = 0; i < cnt; i++) g_clip_peer_files.push_back(d[i]);
		}
		int nf = (int)g_clip_peer_files.size();
		LeaveCriticalSection(&g_clip_lock);
		Log("clip: peer offer fmt=0x%x size=%llu files=%d", m.fmt, m.size, nf);
		if (InterlockedCompareExchange(&g_clip_auto, 0, 0) == 0) break;
		if (nf > 0) ClipBeginRecvFiles();
		else if (m.fmt & RC_CF_PNG) ClipChSend(RC_CM_REQUEST, RC_CF_PNG, 0, 0, 0, 0, 0, NULL, 0);
		else if (m.fmt & (RC_CF_UNICODE | RC_CF_TEXT)) ClipChSend(RC_CM_REQUEST, RC_CF_UNICODE, 0, 0, 0, 0, 0, NULL, 0);
		break;
	}
	case RC_CM_REQUEST:
	{
		if (m.fmt & RC_CF_FILES) ClipSendFileChunk(m.seq, m.offset);
		else if (m.fmt & RC_CF_PNG) ClipSendLocalContent(RC_CF_PNG);
		else ClipSendLocalContent(RC_CF_UNICODE);
		break;
	}
	case RC_CM_DATA:
		ClipHandleRecvData(m, extra, extraLen);
		break;
	case RC_CM_LOCK:
		InterlockedExchange(&g_clip_hold, 1);
		Log("clip: peer lock");
		break;
	case RC_CM_UNLOCK:
		InterlockedExchange(&g_clip_hold, 0);
		Log("clip: peer unlock");
		break;
	case RC_CM_ABORT:
	{
		std::string why = (extraLen > 0) ? std::string(extra, (size_t)extraLen) : std::string("abort");
		Log("clip: peer abort: %s", why.c_str());
		if (g_clip_in_active) { ClipInReset(); InterlockedExchange(&g_clip_hold, 0); }
		break;
	}
	case RC_CM_PASTE:
		keybd_event(VK_CONTROL, 0, 0, 0);
		keybd_event((BYTE)'V', 0, 0, 0);
		keybd_event((BYTE)'V', 0, KEYEVENTF_KEYUP, 0);
		keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
		Log("clip: paste request -> Ctrl+V sent");
		break;
	default:
		Log("clip: unknown msg %d", m.msg);
		break;
	}
}

/* -------- v9 剪贴板通道: 监听线程(轮询剪贴板变化 → 上报) -------- */
static DWORD WINAPI ClipWatchThread(LPVOID)
{
	Log("clip: watch thread start");
	while (InterlockedCompareExchange(&g_clip_stopping, 0, 0) == 0)
	{
		Sleep(200);
		if (g_client_socket == INVALID_SOCKET) continue;
		if (InterlockedCompareExchange(&g_clip_ready, 0, 0) == 0) continue;
		DWORD s = GetClipboardSequenceNumber();
		if (s == 0 || s == g_clip_last_seq) continue;
		g_clip_last_seq = s;
		if (InterlockedCompareExchange(&g_clip_hold, 0, 0) != 0) continue;
		if (InterlockedCompareExchange(&g_clip_auto, 0, 0) == 0) continue;
		ClipOfferSelf();
	}
	Log("clip: watch thread exit");
	return 0;
}

static void HandleClipboard(Packet* p)
{
	ClipReq req;
	req.op = RC_CLIP_GET;
	if (p->header.body_len >= (int)sizeof(ClipReq))
		memcpy(&req, p->body, sizeof(ClipReq));
	const char* extra = p->body + sizeof(ClipReq);
	int extraLen = p->header.body_len - (int)sizeof(ClipReq);
	if (extraLen < 0) extraLen = 0;

	if (req.op == RC_CLIP_SET)
	{
		std::string s(extra, (size_t)extraLen);
		std::string a = Utf8ToAnsi(s);
		std::string res = "err:无法打开剪贴板";
		if (OpenClipboard(NULL))
		{
			EmptyClipboard();
			size_t bytes = a.size() + 1;
			HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
			if (h != NULL)
			{
				void* dst = GlobalLock(h);
				if (dst != NULL)
				{
					memcpy(dst, a.c_str(), bytes);
					GlobalUnlock(h);
					if (SetClipboardData(CF_TEXT, h) != NULL)
						res = "ok:已写入被控端剪贴板 " + std::to_string((long long)a.size()) + " 字节";
					else { GlobalFree(h); res = "err:SetClipboardData 失败"; }
				}
				else { GlobalFree(h); res = "err:GlobalLock 失败"; }
			}
			else res = "err:GlobalAlloc 失败";
			CloseClipboard();
		}
		SendClipRes(RC_CLIP_SET, res);
		Log("clipboard set: %d bytes", (int)a.size());
		return;
	}
	if (req.op == RC_CLIP_CLEAR)
	{
		if (OpenClipboard(NULL)) { EmptyClipboard(); CloseClipboard(); SendClipRes(RC_CLIP_CLEAR, "ok:剪贴板已清空"); }
		else SendClipRes(RC_CLIP_CLEAR, "err:无法打开剪贴板");
		return;
	}

	/* RC_CLIP_GET */
	std::string text;
	if (OpenClipboard(NULL))
	{
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		if (h != NULL)
		{
			const wchar_t* w = (const wchar_t*)GlobalLock(h);
			if (w) { text = WideToUtf8(w); GlobalUnlock(h); }
		}
		else
		{
			h = GetClipboardData(CF_TEXT);
			if (h != NULL)
			{
				const char* cp = (const char*)GlobalLock(h);
				if (cp) { text = AnsiToUtf8(cp); GlobalUnlock(h); }
			}
		}
		CloseClipboard();
	}
	if (text.empty())
		SendClipRes(RC_CLIP_GET, "err:剪贴板为空或无法读取");
	else
		SendClipRes(RC_CLIP_GET, text);
	Log("clipboard get: %d bytes", (int)text.size());
}

/* ---------------- 系统信息 ---------------- */
static void HandleSysInfo(Packet* p)
{
	(void)p;
	char comp[128] = { 0 }, user[128] = { 0 };
	DWORD n = 127;
	GetComputerNameA(comp, &n);
	n = 127;
	GetUserNameA(user, &n);

	OSVERSIONINFOEXA os;
	memset(&os, 0, sizeof(os));
	os.dwOSVersionInfoSize = sizeof(os);
#pragma warning(push)
#pragma warning(disable:4996)
	GetVersionExA((OSVERSIONINFOA*)&os);
#pragma warning(pop)

	MEMORYSTATUSEX ms;
	memset(&ms, 0, sizeof(ms));
	ms.dwLength = sizeof(ms);
	GlobalMemoryStatusEx(&ms);

	SYSTEM_INFO si;
	memset(&si, 0, sizeof(si));
	GetNativeSystemInfo(&si);

	ULONGLONG upSec = GetTickCount64() / 1000;
	char line[512];
	std::string out;
	out += "# 被控端系统信息\r\n";
	_snprintf_s(line, _TRUNCATE, "计算机名   : %s\r\n", comp);            out += line;
	_snprintf_s(line, _TRUNCATE, "登录用户   : %s\r\n", user[0] ? user : "(未知)"); out += line;
	_snprintf_s(line, _TRUNCATE, "系统版本   : %lu.%lu build %lu %s\r\n",
		os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, os.szCSDVersion);  out += line;
	_snprintf_s(line, _TRUNCATE, "处理器     : %u 核 (架构 %u)\r\n",
		si.dwNumberOfProcessors, si.wProcessorArchitecture); out += line;
	_snprintf_s(line, _TRUNCATE, "物理内存   : 共 %llu MB / 可用 %llu MB (占用 %lu%%)\r\n",
		ms.ullTotalPhys / 1048576ULL, ms.ullAvailPhys / 1048576ULL, ms.dwMemoryLoad); out += line;
	_snprintf_s(line, _TRUNCATE, "屏幕       : %d x %d (主显示器)\r\n",
		GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)); out += line;
	_snprintf_s(line, _TRUNCATE, "进程运行   : %llu 秒\r\n", upSec);      out += line;
	_snprintf_s(line, _TRUNCATE, "沙箱根目录 : %s\r\n", g_file_root.empty() ? "(未限制,全盘可访问)" : g_file_root.c_str()); out += line;
	_snprintf_s(line, _TRUNCATE, "日志文件   : %s\r\n", g_log_path.empty() ? "(无)" : g_log_path.c_str()); out += line;
	SendText(g_client_socket, CMD_SYSINFO_RES, out);
}

/* v8: 开机自启管理(控制端远程下发 op: 0=关闭 1=开启 2=查询) */
static bool InstallAutostartAll()
{
	if (InstallAutostartTask())
		return true;
	return InstallAutostart();   /* 回退: HKCU\...\Run */
}

static void UninstallAutostartAll()
{
	UninstallAutostartTask();
	UninstallAutostart();
}

static void HandleAutostart(Packet* p)
{
	int op = 2;
	if (p->header.body_len >= (int)sizeof(int))
		memcpy(&op, p->body, sizeof(int));

	std::string msg;
	if (op == 1)
	{
		if (InstallAutostartTask())
			msg = "ok: 开机自启已开启【计划任务: 登录后以管理员权限运行, 不弹 UAC】\r\n";
		else if (InstallAutostart())
			msg = "warn: 计划任务创建失败, 已回退为注册表自启(普通权限; 建议以管理员运行被控端后重试)\r\n";
		else
			msg = "err: 开启失败(计划任务与注册表均写入失败, 是否以管理员运行?)\r\n";
	}
	else if (op == 0)
	{
		bool t = UninstallAutostartTask();
		bool u = UninstallAutostart();
		msg = (t || u) ? "ok: 开机自启已关闭(计划任务与注册表项已清理)\r\n"
		               : "warn: 未发现已注册的自启项\r\n";
	}
	else
	{
		bool t = AutostartTaskExists();
		bool u = RunValueExists();
		if (t)      msg = "status: 开机自启 = 已开启【计划任务 / 最高权限】\r\n";
		else if (u) msg = "status: 开机自启 = 已开启【注册表 Run / 普通权限】\r\n";
		else        msg = "status: 开机自启 = 未开启\r\n";
		char tr[MAX_PATH + 64] = { 0 };
		BuildAutostartCmd(tr, sizeof(tr));
		msg += std::string("        自启命令: ") + tr + "\r\n";
	}
	SendText(g_client_socket, CMD_AUTOSTART_RES, msg);
	Log("autostart op=%d", op);
}



void HandleCommand(Packet* packet)
{
	switch (packet->header.cmd)
	{
	case CMD_SCREEN:      HandleScreen(packet); break;
	case CMD_MOUSE:       HandleMouse(packet); break;
	case CMD_KEYBOARD:    HandleKeyBoard(packet); break;
	case CMD_KEYTEXT:     HandleKeyText(packet); break;
	case CMD_HEARTBEAT:   SendCmd(g_client_socket, CMD_HEARTBEAT, NULL, 0); break;
	case CMD_SCREEN_CTRL:
		if (packet->header.body_len >= (int)sizeof(ScreenCtrl))
		{
			memcpy(&g_screen_ctrl, packet->body, sizeof(ScreenCtrl));
			Log("screen ctrl: fps=%d quality=%d encode=%d", g_screen_ctrl.fps, g_screen_ctrl.quality, g_screen_ctrl.encode);
		}
		break;
	case CMD_FILE_LIST:         HandleFileList(packet); break;
	case CMD_FILE_DOWNLOAD:     HandleFileDownload(packet); break;
	case CMD_FILE_UPLOAD_BEGIN: HandleFileUploadBegin(packet); break;
	case CMD_FILE_DATA:         HandleFileData(packet); break;
	case CMD_FILE_END:          HandleFileEnd(packet); break;
	case CMD_FILE_OP:           HandleFileOp(packet); break;
	case CMD_CURSOR:            HandleCursor(packet); break;
	case CMD_INPUT_REC:         HandleInputRec(packet); break;
	case CMD_AUDIO:              HandleAudioPacket(packet); break;
	case CMD_MIC:                HandleMicPacket(packet); break;
	case CMD_CAMERA:             HandleCameraPacket(packet); break;
	case CMD_CLIPBOARD:         HandleClipboard(packet); break;
	case CMD_CLIP_CHANNEL:      HandleClipChannel(packet); break;   /* v9: 剪贴板同步虚拟通道 */
	case CMD_SYSINFO:           HandleSysInfo(packet); break;
	case CMD_AUTOSTART:         HandleAutostart(packet); break;
	case CMD_TESTCONNECT:       SendCmd(g_client_socket, CMD_TESTCONNECT, NULL, 0); break;
	case CMD_HELLO:             /* 版本握手: 让控制端知道本端协议版本/能力 */
		{
			HelloInfo hi;
			memset(&hi, 0, sizeof(hi));
			hi.proto_version = RC_PROTO_VERSION;
			hi.caps = RC_CAP_SCREEN | RC_CAP_FILE | RC_CAP_SYSINFO |
				RC_CAP_CURSOR | RC_CAP_INPUTREC | RC_CAP_CLIPBOARD |
				RC_CAP_AUDIO | RC_CAP_CAMERA | RC_CAP_MIC;
			_snprintf_s(hi.build, sizeof(hi.build), _TRUNCATE,
				"被控端 v%d.0 (build %s %s)", RC_PROTO_VERSION, __DATE__, __TIME__);
			SendCmd(g_client_socket, CMD_HELLO_RES, (const char*)&hi, (int)sizeof(hi));
			Log("hello: proto=%d caps=0x%x", hi.proto_version, hi.caps);
		}
		break;
	case CMD_DISCONNECT:
		Log("client requested disconnect");
		InterlockedExchange(&g_session_active, 0);
		break;
	default:
		Log("unknown cmd %d (len=%d)", packet->header.cmd, packet->header.body_len);
		break;
	}
}

/* ============================================================
 *  连接会话
 * ============================================================ */
static void EnableKeepAlive(SOCKET s)
{
	BOOL opt = TRUE;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&opt, sizeof(opt));
	/* 1 秒接收超时：周期性检查运行状态与空闲超时 */
	int timeout = 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
	/* 屏幕数据较大，加大发送缓冲 */
	int sndbuf = 1024 * 1024;
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
	/* 关闭 Nagle: 鼠标/键盘小包不再被延迟合并(默认最多等 40ms), 输入立刻可见 */
	{
		int nodelay = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
	}
}

void ClientSession(SOCKET s)
{
	EnableKeepAlive(s);
	g_client_socket = s;
	g_last_recv_tick = GetTickCount64();
	InterlockedExchange(&g_session_active, 1);

	PacketStream stream;
	std::vector<char> buf(65536);

	while (g_running && g_session_active)
	{
		int len = recv(s, buf.data(), (int)buf.size(), 0);
		if (len > 0)
		{
			g_last_recv_tick = GetTickCount64();
			stream.Append(buf.data(), len);
			Packet* p = NULL;
			while ((p = stream.Next()) != NULL)
			{
				HandleCommand(p);
				free(p);
			}
		}
		else if (len == 0)
		{
			Log("client closed connection");
			break;
		}
		else
		{
			int err = WSAGetLastError();
			if (err == WSAETIMEDOUT)
			{
				if (GetTickCount64() - g_last_recv_tick > RC_IDLE_TIMEOUT)
				{
					Log("idle timeout (%ds), drop session", RC_IDLE_TIMEOUT / 1000);
					break;
				}
				continue;
			}
			if (err == WSAEINTR)
				continue;
			Log("recv error %d, drop session", err);
			break;
		}
	}

	/* 会话结束: 停止声音/摄像头/麦克风采集线程 */
	HandleAudioStop();
	HandleMicStop();
	HandleCameraStop();

	/* 清理未完成的接收上传 */
	for (std::map<unsigned int, FILE*>::iterator it = g_uploads.begin(); it != g_uploads.end(); ++it)
		if (it->second != NULL)
			fclose(it->second);
	g_uploads.clear();

	InterlockedExchange(&g_session_active, 0);
	if (g_client_socket == s)
		g_client_socket = INVALID_SOCKET;
	closesocket(s);
	Log("session closed");
}

static bool InitListen(int port)
{
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return false;

	g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (g_listen_socket == INVALID_SOCKET)
	{
		Log("socket() failed: %d", WSAGetLastError());
		return false;
	}
	BOOL reuse = TRUE;
	setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

	SOCKADDR_IN addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);

	if (bind(g_listen_socket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		Log("bind port %d failed: %d", port, WSAGetLastError());
		return false;
	}
	if (listen(g_listen_socket, 5) == SOCKET_ERROR)
	{
		Log("listen failed: %d", WSAGetLastError());
		return false;
	}
	return true;
}

static void AcceptLoop(int port)
{
	Log("agent listening on 0.0.0.0:%d, waiting for client...", port);
	while (g_running)
	{
		SOCKADDR_IN cli;
		int cliLen = sizeof(cli);
		SOCKET s = accept(g_listen_socket, (SOCKADDR*)&cli, &cliLen);
		if (s == INVALID_SOCKET)
		{
			if (!g_running)
				break;
			int err = WSAGetLastError();
			if (err == WSAEINTR || err == WSAENOTSOCK)
				break;
			Log("accept failed: %d", err);
			Sleep(500);
			continue;
		}
		char ip[64] = { 0 };
		InetNtopA(AF_INET, &cli.sin_addr, ip, sizeof(ip) - 1);
		Log("client connected: %s:%d", ip, ntohs(cli.sin_port));
		ClientSession(s);
		Log("waiting for new connection...");
	}
}

/* --stop 时唤醒：主动关闭套接字让阻塞中的 accept/recv 立即返回 */
DWORD WINAPI StopWatcherThread(LPVOID)
{
	WaitForSingleObject(g_stop_event, INFINITE);
	Log("stop signal received, shutting down");
	InterlockedExchange(&g_running, 0);
	InterlockedExchange(&g_session_active, 0);
	if (g_client_socket != INVALID_SOCKET)
	{
		shutdown(g_client_socket, SD_BOTH);
		closesocket(g_client_socket);
		g_client_socket = INVALID_SOCKET;
	}
	if (g_listen_socket != INVALID_SOCKET)
	{
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
	}
	return 0;
}

/* 日志路径：优先 %ProgramData%\RemoteControl\agent.log，失败回退 %TEMP% */
static std::string DefaultLogPath()
{
	char dir[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA("ProgramData", dir, MAX_PATH) > 0)
	{
		std::string d = std::string(dir) + "\\RemoteControl";
		CreateDirectoryA(d.c_str(), NULL);
		return d + "\\agent.log";
	}
	char tmp[MAX_PATH] = { 0 };
	if (GetTempPathA(MAX_PATH, tmp) > 0)
		return std::string(tmp) + "rc_agent.log";
	return "rc_agent.log";
}

/* ============================================================
 *  入口：静默(WINDOWS 子系统，无控制台/无窗口)
 * ============================================================ */
/* ============================================================
 *  UDP 设备发现: 控制端广播 "RC_DISCOVER_V1" -> 本端单播回包
 *  (被控端信息: 主机名/协议版本/实际监听端口)
 * ============================================================ */
static int g_disc_port = 9999;

static DWORD WINAPI DiscThread(LPVOID)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) { Log("discover: socket() failed (%d)", WSAGetLastError()); return 0; }
	BOOL on = TRUE;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
	int rcvto = 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvto, sizeof(rcvto));

	sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(RC_DISC_PORT);
	a.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
	{
		Log("discover: bind udp %d failed (%d)", RC_DISC_PORT, WSAGetLastError());
		closesocket(s);
		return 0;
	}

	char host[64] = { 0 };
	DWORD hn = (DWORD)sizeof(host) - 1;
	if (!GetComputerNameA(host, &hn)) strncpy_s(host, "agent", _TRUNCATE);
	Log("discover: udp %d ready, name=%s tcp_port=%d", RC_DISC_PORT, host, g_disc_port);

	while (g_running)
	{
		char buf[128] = { 0 };
		sockaddr_in from;
		int fl = (int)sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
		if (n <= 0) continue;
		if (strncmp(buf, RC_DISC_REQ, strlen(RC_DISC_REQ)) != 0) continue;

		DiscReply r;
		memset(&r, 0, sizeof(r));
		r.magic = RC_DISC_MAGIC;
		r.proto = RC_PROTO_VERSION;
		r.port = (unsigned short)g_disc_port;
		strncpy_s(r.name, host, _TRUNCATE);
		sendto(s, (char*)&r, sizeof(r), 0, (sockaddr*)&from, fl);
		Log("discover: reply -> %s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
	}
	closesocket(s);
	return 0;
}

/* ============ v17: 一键部署(服务 + 计划任务 + Run 键), 双击即用 ============ */

static bool RcIsElevatedNow()
{
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
	TOKEN_ELEVATION te; DWORD sz = sizeof(te); bool ok = false;
	if (GetTokenInformation(tok, TokenElevation, &te, sz, &sz)) ok = (te.TokenIsElevated != 0);
	CloseHandle(tok);
	return ok;
}

static bool RcStrHasNoCase(const char* hay, const char* needle)
{
	if (hay == NULL || needle == NULL || needle[0] == 0) return false;
	size_t nl = strlen(needle);
	for (const char* q = hay; *q; ++q)
		if (_strnicmp(q, needle, nl) == 0) return true;
	return false;
}

/* v18: Run 键不仅要存在, 还必须指向当前这份 exe */
static bool RcRunKeyPointsTo(const char* exe)
{
	HKEY h = NULL;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, RC_RUN_KEY, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
	char buf[1024] = { 0 }; DWORD sz = sizeof(buf) - 1, type = 0;
	LONG r = RegQueryValueExA(h, RC_RUN_NAME, NULL, &type, (BYTE*)buf, &sz);
	RegCloseKey(h);
	if (r != ERROR_SUCCESS) return false;
	return RcStrHasNoCase(buf, exe);
}

static bool RcTaskExists()
{
	char cmd[512] = { 0 };
	sprintf_s(cmd, "schtasks.exe /Query /TN \"%s\"", RC_TASK_NAME);
	return RunHiddenWait(cmd, 15000);
}

/* v18: 「已部署好」= 三层都指向【当前这份 exe】。
   只判"存在"是不够的: 换目录后旧的 服务/任务/Run 都还在, 但都指着老路径,
   重启时拉的是已经不存在的旧 exe —— 这正是"开机不自启"的真凶。 */
static bool RcAllCurrent()
{
	char exe[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	bool svc = RCService_PointsTo(exe);
	bool run = RcRunKeyPointsTo(exe);
	Log("v18 selfcheck: service->this=%d run->this=%d", svc ? 1 : 0, run ? 1 : 0);
	return svc && run;
}

/* 一次装齐三层: 服务 + 计划任务 + Run 键(需管理员权限) */
struct RcDeployResult { bool svc; bool task; bool run; };

/* 一次装齐三层: 服务 + 计划任务 + Run 键(需管理员权限), 装完再复验一遍 */
static RcDeployResult RcDoDeploy()
{
	RcDeployResult r; r.svc = false; r.task = false; r.run = false;
	char exe[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	Log("v18 deploy: start (exe=%s, port=%d)", exe, g_agent_port);
	r.svc  = RCService_Install(exe, g_agent_port);
	r.task = InstallAutostartTask();
	InstallAutostart();
	/* 复验: 以"配置里到底指向谁"为准, 而不是以 API 返回值 */
	r.svc = RCService_PointsTo(exe);
	r.run = RcRunKeyPointsTo(exe);
	Log("v18 deploy result: service=%d task=%d run=%d", r.svc ? 1 : 0, r.task ? 1 : 0, r.run ? 1 : 0);
	return r;
}

/* 部署完成后告诉用户结果(只在"需要修复"的那一次出现) */
static void RcNotify(const RcDeployResult& r)
{
	if (r.svc && r.run)
	{
		MessageBoxA(NULL,
			"开机自启已装好 —— 以后每次开机都会自动启动, 不用再点任何东西。\n\n"
			"三层保险:\n"
			"  1. Windows 服务 (开机即起, 进程被结束 5 秒内自动恢复)\n"
			"  2. 计划任务 (登录时以管理员权限启动)\n"
			"  3. 注册表 Run (兜底)\n\n"
			"被控端现在已经在运行了, 可以直接连接。",
			"RemoteControl 被控端 — 自启安装成功", MB_OK | MB_ICONINFORMATION);
	}
	else
	{
		char msg[640] = { 0 };
		sprintf_s(msg,
			"开机自启只装上了一部分(可能被安全软件拦了):\n\n"
			"  服务: %s\n  计划任务: %s\n  注册表 Run: %s\n\n"
			"被控端现在可以正常使用, 但重启后可能不会自动启动。\n"
			"详细日志: %%ProgramData%%\\RemoteControl\\agent.log",
			r.svc ? "OK" : "失败", r.task ? "OK" : "失败", r.run ? "OK" : "失败");
		MessageBoxA(NULL, msg, "RemoteControl 被控端", MB_OK | MB_ICONWARNING);
	}
}

/* 返回 true = 已自举出提权实例, 本实例应退出(仅第一次需要) */
static bool RcSelfElevate()
{
	char exe[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	char args[128] = { 0 };
	sprintf_s(args, "--deploy --port %d", g_agent_port);
	HINSTANCE h = ShellExecuteA(NULL, "runas", exe, args, NULL, SW_SHOWNORMAL);
	if ((INT_PTR)h > 32)
	{
		Log("v17 deploy: elevated setup instance launched (one-time UAC), this instance exits");
		return true;
	}
	Log("v17 deploy: elevation denied (%d), continue WITHOUT service", (int)(INT_PTR)h);
	return false;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLineW, int nCmdShow)
{
	/* DPI 感知必须在任何 GetSystemMetrics / 输入注入之前设置好:
	   否则鼠标坐标会被系统 DPI 虚拟化缩放, 出现"点不准/右键菜单跑偏"。 */
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	InitializeCriticalSection(&g_send_lock);
	InitializeCriticalSection(&g_log_lock);
	InitializeCriticalSection(&g_clip_lock);      /* v9: 剪贴板通道 */
	g_clip_last_seq = GetClipboardSequenceNumber();
	Log("=== agent start (dpi aware) ===");

	/* 宽字符命令行 -> ANSI，便于统一解析 */
	char cmdlineA[2048] = { 0 };
	if (lpCmdLineW != NULL)
		WideCharToMultiByte(CP_ACP, 0, lpCmdLineW, -1, cmdlineA, sizeof(cmdlineA) - 1, NULL, NULL);

	Options opt;
	ParseArgs(cmdlineA, opt);
	g_console = opt.console;
	g_file_root = opt.root;
	g_log_path = opt.log.empty() ? DefaultLogPath() : opt.log;

	/* v17: --svc —— 由 SCM 启动的守护模式。它只负责把被控端投递到交互式会话;
	   session 0 里没有桌面, 所以守护本身不抓屏、不注入键鼠。 */
	if (opt.svc)
	{
		char self[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, self, MAX_PATH);
		char sdir[MAX_PATH] = { 0 };
		strcpy_s(sdir, self);
		char* sp = strrchr(sdir, '\\');
		if (sp) *sp = 0;
		RCService_SetLogDir(sdir);
		bool okSvc = RCService_Entry(self, opt.port);
		return okSvc ? 0 : 1;
	}

	if (g_console)
	{
		if (!AttachConsole(ATTACH_PARENT_PROCESS))
			AllocConsole();
		FILE* dummy = NULL;
		freopen_s(&dummy, "CONOUT$", "w", stdout);
		printf("RemoteControl Agent v2.0 (console mode)\n");
	}

	Log("===== RemoteControl Agent v2.0 =====");
	Log("log=%s port=%d root=%s", g_log_path.c_str(), opt.port, g_file_root.empty() ? "(full disk)" : g_file_root.c_str());
	Log("privilege: %s (elevated/HIGH 才能操作任务管理器等提权窗口)", PrivState());
	EnsureFirewallRule();   /* v11-fix: 跨机连接需放行防火墙入站 */

	g_agent_port = opt.port;

	/* v17: 双击即用 —— 缺哪层补哪层; 需要时自举一次管理员授权(仅第一次)。
	   之后这台机器就再也不用管了: 服务开机即起, 进程被杀 5 秒内自愈。 */
	if (!opt.install && !opt.uninstall && !opt.stop && !opt.backup && !opt.svc)
	{
		if (opt.deploy)
		{
			/* 已提权的部署实例: 装齐三层 -> 告知结果 -> 继续作为主实例常驻 */
			RcDeployResult res = RcDoDeploy();
			RcNotify(res);
		}
		else if (!RcAllCurrent())
		{
			if (RcIsElevatedNow())
			{
				RcDeployResult res = RcDoDeploy();
				RcNotify(res);
			}
			else if (RcSelfElevate())
			{
				return 0;   /* 交给提权实例完成部署并常驻 */
			}
			else
			{
				/* UAC 被拒也不能什么都不做: Run 键无需管理员, 先保证用户级自启 */
				InstallAutostart();
				Log("v18: elevation denied -> user-level autostart (HKCU Run) written as fallback");
			}
		}
	}

	if (opt.install)
	{
		RcDoDeploy();
		return 0;
	}
	if (opt.uninstall) { UninstallAutostartTask(); UninstallAutostart(); RCService_Remove(); return 0; }
	if (opt.stop)      { StopRunningInstance(opt.port); return 0; }

	/* 单实例: 同一端口已有实例在运行 → 默认自动结束旧实例并接管(方便直接用新版本替换);

	   加 --keep-old 保持旧行为(直接退出); 不同端口可并存。 */

	char mutexName[128];

	sprintf_s(mutexName, "Global\\RemoteControlAgent_Mutex_%d", opt.port);

	HANDLE mutex = CreateMutexA(NULL, FALSE, mutexName);

	bool tookOver = (mutex != NULL && GetLastError() != ERROR_ALREADY_EXISTS);

	if (!tookOver)

	{

		if (opt.backup)
		{
			/* v14: Run 键兜底实例 —— 主实例(计划任务/提权)已在跑, 本实例安静退出 */
			Log("backup instance: primary agent already running on port %d, exit", opt.port);
			return 0;
		}
		if (opt.keepOld)

		{

			Log("another instance is running on port %d, exit (--keep-old)", opt.port);

			return 0;

		}

		/* v17: 非提权实例绝不"接管"已在跑的实例 —— 否则双击一下就把
		   管理员权限的被控端换成普通权限的, 任务管理器点不动。 */
		if (!RcIsElevatedNow())
		{
			Log("another instance already running on port %d; this non-elevated instance exits", opt.port);
			if (!opt.console)
				MessageBoxA(NULL, "被控端已经在运行了(端口 9999),无需重复启动。\n"
					"本机每次开机都会自动启动它。", "RemoteControl 被控端", MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		Log("another instance on port %d detected, taking over", opt.port);

		StopRunningInstance(opt.port);

		for (int i = 0; i < 40 && !tookOver; ++i)

		{

			Sleep(100);

			if (mutex != NULL)

				CloseHandle(mutex);

			mutex = CreateMutexA(NULL, FALSE, mutexName);

			tookOver = (mutex != NULL && GetLastError() != ERROR_ALREADY_EXISTS);

		}

		if (!tookOver)

		{

			int n = KillOtherAgentProcesses();

			Log("takeover: force killed %d process(es)", n);

			Sleep(500);

			if (mutex != NULL)

				CloseHandle(mutex);

			mutex = CreateMutexA(NULL, FALSE, mutexName);

			tookOver = (mutex != NULL && GetLastError() != ERROR_ALREADY_EXISTS);

		}

		if (!tookOver)

		{

			Log("takeover failed: another instance still holds port %d, exit", opt.port);

			return 0;

		}

		Log("takeover done: old agent replaced (port %d)", opt.port);

	}

	RefreshAutostartIfPresent(opt.port);

	char stopName[128];
	sprintf_s(stopName, "Global\\RemoteControlAgentStop_%d", opt.port);
	g_stop_event = CreateEventA(NULL, TRUE, FALSE, stopName);
	if (g_stop_event != NULL)
	{
		HANDLE h = CreateThread(NULL, 0, StopWatcherThread, NULL, 0, NULL);
		if (h != NULL)
			CloseHandle(h);
	}

	if (!InitListen(opt.port))
	{
		Log("init network failed, exit");
		return -1;
	}

	/* 局域网发现: UDP 监听线程 (供控制端广播嗅探) */
	g_disc_port = opt.port;
	{
		HANDLE hd = CreateThread(NULL, 0, DiscThread, NULL, 0, NULL);
		if (hd != NULL) CloseHandle(hd);
		else Log("discover: create thread failed");
	}

	/* v9: 剪贴板同步监听(轮询 GetClipboardSequenceNumber) */
	{
		HANDLE hc = CreateThread(NULL, 0, ClipWatchThread, NULL, 0, NULL);
		if (hc != NULL) CloseHandle(hc);
		else Log("clip: create watch thread failed");
	}

	AcceptLoop(opt.port);

	WSACleanup();
	Log("agent exited");
	return 0;
}
