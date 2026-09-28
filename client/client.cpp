// ============================================================
//  RemoteControl 控制端 (Client)  v2.0
//  新特性: 文件浏览/上传/下载/删除/重命名/新建目录
//          心跳保活 + 断线自动重连 / 画质帧率调节 / 参数化连接
// ============================================================
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atlimage.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdarg>
#include "..\\common\\protocol.h"
#include "android_apk_data.h"   /* 内嵌安卓被控端 APK(一键生成用, 由 temp\android\embed_apk.py 生成) */
#include <mmsystem.h>
#include <gdiplus.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"comdlg32.lib")
#include <shellapi.h>
#include <windowsx.h>
#include <imm.h>
#include <commctrl.h>
#pragma comment(lib,"imm32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shell32.lib")

/* 前置声明: 下面鼠标/拖放增强块要用到(定义在文件后部) */
static void ConPrint(const char* fmt, ...);
static std::string ExtGbk2Utf8(const std::string& g);
static std::string ExtUtf8ToGbk(const std::string& u);
static void ExtStartUpload(const std::string& localPath, const std::string& remotePath);

/* ---------------- 控件 ID ---------------- */
#define IDC_EDIT_IP        1001
#define IDC_EDIT_PORT      1002
#define IDC_BTN_CONNECT    1003
#define IDC_BTN_DISCONNECT 1004
#define IDC_EDIT_PATH      1005
#define IDC_BTN_REFRESH    1006
#define IDC_LIST_FILES     1007
#define IDC_BTN_UP         1008
#define IDC_BTN_DOWNLOAD   1009
#define IDC_BTN_UPLOAD     1010
#define IDC_BTN_MKDIR      1011
#define IDC_BTN_DELETE     1012
#define IDC_BTN_RENAME     1013
#define IDC_COMBO_FPS      1014
#define IDC_EDIT_LOG       1015
#define IDC_STATIC_STATUS  1016
#define IDC_COMBO_ENC      1017
#define IDC_EDIT_NAME      1018
#define IDC_COMBO_Q        1019

#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_STATUS   (WM_APP + 2)
#define WM_APP_FILLLIST (WM_APP + 3)
#define WM_APP_SCREEN   (WM_APP + 4)

#define VIEW_CLASS  "RCViewClass"
#define MAIN_CLASS  "RCMainClass"

/* ---------------- 全局状态 ---------------- */
HWND  g_hwnd = NULL;          /* 主窗口 */
HWND  g_view = NULL;          /* 远程画面子窗口 */
SOCKET g_sock = INVALID_SOCKET;
CRITICAL_SECTION g_send_lock;
CRITICAL_SECTION g_state_lock;
volatile LONG g_running = 1;
volatile LONG g_connected = 0;
volatile LONG g_need_full_render = 0;   /* 窗口尺寸刚变过: 下一帧起整屏重建, 直到收到全帧帧 */
volatile ULONGLONG g_last_hb = 0;
volatile LONG g_auto_reconnect = 1;
char  g_ip[64] = "192.168.0.103";   /* v12: 实测 .103 才是被控端设备(首次连上后由 client.ini 记忆) */
int   g_port = 9999;
char  g_cur_path[512] = "";   /* 空 = 首次连接时由被控端返回起始目录(有 --root 即沙箱根) */
int   g_fps = 30;   /* 默认 30fps: 越高越跟手(上限 60) */
int   g_encoder = RC_ENC_PNG;
volatile LONG g_id_counter = 1000;   /* 文件传输会话ID分配器 */
volatile LONG g_manual_disconnect = 0; /* 用户主动断开：不自动重连 */
/* 版本握手: 防止"连到了旧版被控端，命令发出去却永远没有回包"的困惑 */
volatile ULONGLONG g_hello_tick = 0;   /* 发出 CMD_HELLO 的时刻(0 = 未发) */
volatile LONG g_server_hello = 0;      /* 是否已收到被控端 CMD_HELLO_RES */
volatile LONG g_hello_warned = 0;      /* 是否已提示过"被控端版本过旧" */
int g_server_proto = 0;                /* 被控端协议版本 */

/* 远程画面 */
CImage g_image;
bool   g_has_image = false;

/* ---- v3：增量帧 + 双缓冲渲染 + 实时状态 ---- */
HDC      g_cacheDC = NULL;             /* 与视图客户区等大的后台缓冲，绘制时一次性 SRCCOPY，杜绝闪烁 */
HBITMAP  g_cacheBmp = NULL;
HGDIOBJ  g_cacheOld = NULL;
int      g_cacheW = 0, g_cacheH = 0;
volatile LONG g_need_full = 1;         /* 下次请求整屏(建立/重建基准帧) */
volatile LONG g_frame_pending = 0;     /* 已发包、等待回帧(乒乓式请求，避免请求堆积) */
HANDLE   g_frame_evt = NULL;
volatile ULONGLONG g_last_req_tick = 0;
volatile LONG g_last_rtt = 0;
volatile LONG g_stat_frames = 0;
volatile LONG g_stat_bytes = 0;
volatile ULONGLONG g_stat_base = 0;
int      g_stat_fps_x10 = 0;
int      g_stat_kbps = 0;
int      g_quality = 70;               /* JPEG 质量 */

/* 文件列表缓存 */
struct FileEntry { char kind; std::string name; unsigned long long size; std::string time; };
std::vector<FileEntry> g_entries;

/* 下载状态 (仅接收线程访问) */
FILE* g_down_fp = NULL;
unsigned int g_down_id = 0;
volatile LONG g_down_active = 0;
volatile LONG g_up_active = 0;       /* v13: 上传中也让出带宽给文件 */
std::string g_down_dir;

std::deque<std::string> g_logs;

void LogLine(const char* fmt, ...);
void PostStatus(const char* text);
void SetConnected(bool on);
bool SendCmdPacket(int cmd, const char* body, int len);
static bool SendClipboard(int op, const std::string& text);   /* 扩展区实现(自动 GBK->UTF-8) */
static void ExtStartUpload(const std::string& local, const std::string& remoteIn);   /* 拖放上传要用 */
static void ExtDrawMarker(HWND hwnd, HDC hdc);
static bool ExtOnPacket(Packet* p);
static LRESULT CALLBACK ExtCmdInProc(HWND, UINT, WPARAM, LPARAM);

/* ---------------- 日志/状态 (线程安全) ---------------- */
void LogLine(const char* fmt, ...)
{
	char msg[1200];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(msg, sizeof(msg) - 1, _TRUNCATE, fmt, ap);
	va_end(ap);

	SYSTEMTIME st;
	GetLocalTime(&st);
	char line[1300];
	_snprintf_s(line, _TRUNCATE, "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, msg);

	EnterCriticalSection(&g_state_lock);
	g_logs.push_back(line);
	if (g_logs.size() > 500)
		g_logs.pop_front();
	LeaveCriticalSection(&g_state_lock);

	if (g_hwnd != NULL)
		PostMessageA(g_hwnd, WM_APP_LOG, 0, 0);
	OutputDebugStringA(line);
}

void PostStatus(const char* text)
{
	if (g_hwnd == NULL)
		return;
	std::string* p = new std::string(text);
	PostMessageA(g_hwnd, WM_APP_STATUS, 0, (LPARAM)p);
}

/* ---------------- 网络发送 ---------------- */
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

bool SendCmdPacket(int cmd, const char* body, int len)
{
	if (g_sock == INVALID_SOCKET)
		return false;
	Packet* p = PackPacket(cmd, body, len);
	EnterCriticalSection(&g_send_lock);
	bool ok = SendAll(g_sock, (char*)&p->header.magic, GetPacketLen(p));
	LeaveCriticalSection(&g_send_lock);
	free(p);
	return ok;
}

static bool SendText(int cmd, const std::string& text)
{
	return SendCmdPacket(cmd, text.data(), (int)text.size());
}

/* ---------------- 连接管理 ---------------- */
void SetConnected(bool on)
{
	InterlockedExchange(&g_connected, on ? 1 : 0);
	if (on)
	{
		InterlockedExchange(&g_need_full, 1);   /* 新连接：先要一帧整屏建立基准 */
		InterlockedExchange(&g_frame_pending, 0);
		g_stat_frames = 0;
		g_stat_bytes = 0;
		g_stat_base = GetTickCount64();
	}
	if (g_hwnd != NULL)
		PostMessageA(g_hwnd, WM_APP_STATUS, 0, NULL);
}

/* v12: 记住上次成功连接的地址(同目录 client.ini) -> 下次打开自动填 */
static char g_ini_path[MAX_PATH] = { 0 };
static void IniPath()
{
	if (g_ini_path[0]) return;
	GetModuleFileNameA(NULL, g_ini_path, MAX_PATH);
	char* p = strrchr(g_ini_path, '\\');
	if (p) *(p + 1) = 0;
	strncat_s(g_ini_path, "client.ini", _TRUNCATE);
}
static void LoadSettings()
{
	IniPath();
	char buf[128] = { 0 };
	GetPrivateProfileStringA("conn", "ip", g_ip, buf, sizeof(buf) - 1, g_ini_path);
	if (buf[0]) strncpy_s(g_ip, buf, _TRUNCATE);
	int port = GetPrivateProfileIntA("conn", "port", g_port, g_ini_path);
	if (port > 0 && port < 65536) g_port = port;
}
static void SaveSettings()
{
	IniPath();
	WritePrivateProfileStringA("conn", "ip", g_ip, g_ini_path);
	char pb[16] = { 0 };
	sprintf_s(pb, "%d", g_port);
	WritePrivateProfileStringA("conn", "port", pb, g_ini_path);
}

static SOCKET ConnectTo(const char* ip, int port)
{
	SOCKADDR_IN addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.S_un.S_addr = inet_addr(ip);
	if (addr.sin_addr.S_un.S_addr == INADDR_NONE)
		return INVALID_SOCKET;

	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;

	/* v11-fix: 原来的写法是阻塞 connect —— 连不可达地址时要等 Windows 的
	   SYN 重试(约 21 秒)才返回，期间 UI 线程被卡住 = 用户看到的"一连接就卡死"。
	   改成: 非阻塞 connect + select 等待 3 秒 + 期间抽空处理窗口消息(界面不冻结)。 */
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	int rc = connect(s, (SOCKADDR*)&addr, sizeof(addr));
	if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		closesocket(s);
		return INVALID_SOCKET;
	}
	if (rc == SOCKET_ERROR)
	{
		const ULONGLONG t0 = GetTickCount64();
		bool ok = false;
		while (GetTickCount64() - t0 < 3000)
		{
			fd_set wf, ef;
			FD_ZERO(&wf); FD_ZERO(&ef);
			FD_SET(s, &wf); FD_SET(s, &ef);
			timeval tv;
			tv.tv_sec = 0; tv.tv_usec = 50 * 1000;
			int n = select(0, NULL, &wf, &ef, &tv);
			if (n > 0)
			{
				int err = 0, elen = sizeof(err);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
				ok = (err == 0);
				break;
			}
			MSG msg;
			while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT) break;
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		if (!ok)
		{
			closesocket(s);
			WSASetLastError(WSAETIMEDOUT);
			return INVALID_SOCKET;
		}
	}
	nb = 0;
	ioctlsocket(s, FIONBIO, &nb);      /* 恢复阻塞模式(收发超时按下面设置) */

	int timeout = 3000;
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

	/* 连接成功后：1 秒接收超时用于周期性检查状态 */
	int rcvto = 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvto, sizeof(rcvto));
	BOOL ka = TRUE;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&ka, sizeof(ka));
	/* 关闭 Nagle: 键鼠小包立即发出，不再等 40ms 合并 —— 手感"跟手"的关键之一 */
	{
		int nodelay = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
	}
	return s;
}

/* ============================================================
 *  远程画面子窗口：缓存式绘制(无闪烁) + 鼠标/键盘回传
 * ============================================================ */
/* ============================================================
 *  v10: 画面铺展模式 / 全屏模式
 *  铺展: 0=适应窗口(等比, 留黑边)  1=铺满窗口(拉伸到满, 忽略宽高比)  2=手动缩放(百分比)
 * ============================================================ */
static LONG g_view_fit = 0;            /* 当前铺展模式 */
static LONG g_view_zoom = 100;         /* 手动模式缩放百分比 (20~400) */
static LONG g_fullscreen = 0;
#define RC_FS_MAXWND 200
static HWND g_fs_saved[RC_FS_MAXWND];  /* 全屏时被隐藏的控件 */
static int  g_fs_count = 0;
static WINDOWPLACEMENT g_fs_wp = { 0 };

/* 等比缩放后的目标矩形(居中放置，多出的边走黑) */
static void CalcDstRect(int cw, int ch, int iw, int ih, RECT& d)
{
	const LONG fit = InterlockedCompareExchange(&g_view_fit, 0, 0);
	int dw = cw, dh = ch;
	if (fit == 1)
	{
		dw = cw;
		dh = ch;                       /* 铺满: 拉伸到整个窗口, 不再留黑边 */
	}
	else if (fit == 2 && iw > 0 && ih > 0)
	{
		const LONG zoom = InterlockedCompareExchange(&g_view_zoom, 0, 0);
		dw = (int)((long long)iw * (zoom > 0 ? zoom : 100) / 100);
		dh = (int)((long long)ih * (zoom > 0 ? zoom : 100) / 100);
	}
	else if (iw > 0 && ih > 0)
	{
		if ((long long)cw * ih > (long long)ch * iw)
			dw = (int)((long long)ch * iw / ih);
		else
			dh = (int)((long long)cw * ih / iw);
	}
	if (fit != 2)
	{
		if (dw > cw) dw = cw;
		if (dh > ch) dh = ch;
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	d.left = (cw - dw) / 2;
	d.top = (ch - dh) / 2;
	d.right = d.left + dw;
	d.bottom = d.top + dh;
}

/* 释放后台缓冲 */
static void CacheFree()
{
	if (g_cacheDC != NULL)
	{
		if (g_cacheOld != NULL) { SelectObject(g_cacheDC, g_cacheOld); g_cacheOld = NULL; }
		DeleteDC(g_cacheDC);
		g_cacheDC = NULL;
	}
	if (g_cacheBmp != NULL) { DeleteObject(g_cacheBmp); g_cacheBmp = NULL; }
	g_cacheW = g_cacheH = 0;
}

/* 保证后台缓冲与客户区同尺寸(变化时重建并涂黑) */
static bool CacheEnsure(int cw, int ch)
{
	if (g_cacheDC != NULL && g_cacheW == cw && g_cacheH == ch)
		return true;
	CacheFree();
	g_cacheDC = CreateCompatibleDC(NULL);
	if (g_cacheDC == NULL)
		return false;
	BITMAPINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = cw;
	bi.bmiHeader.biHeight = -ch;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = NULL;
	g_cacheBmp = CreateDIBSection(g_cacheDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (g_cacheBmp == NULL)
	{
		DeleteDC(g_cacheDC);
		g_cacheDC = NULL;
		return false;
	}
	g_cacheOld = SelectObject(g_cacheDC, g_cacheBmp);
	g_cacheW = cw;
	g_cacheH = ch;
	RECT rc = { 0, 0, cw, ch };
	FillRect(g_cacheDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
	return true;
}

/* 把源图上的一个矩形区域缩放后画进后台缓冲(增量刷新用，代价与区域大小成正比) */
static void CacheRenderRegion(int sx, int sy, int sw, int sh)
{
	if (g_image.IsNull() || g_view == NULL || sw <= 0 || sh <= 0)
		return;
	RECT cr;
	GetClientRect(g_view, &cr);
	const int cw = cr.right, ch = cr.bottom;
	if (cw <= 0 || ch <= 0 || !CacheEnsure(cw, ch))
		return;

	const int iw = g_image.GetWidth(), ih = g_image.GetHeight();
	if (iw <= 0 || ih <= 0)
		return;
	RECT d;
	CalcDstRect(cw, ch, iw, ih, d);
	const int dw = d.right - d.left, dh = d.bottom - d.top;

	int dx0 = d.left + (int)((long long)sx * dw / iw);
	int dy0 = d.top  + (int)((long long)sy * dh / ih);
	int dx1 = d.left + (int)(((long long)(sx + sw) * dw + iw - 1) / iw);
	int dy1 = d.top  + (int)(((long long)(sy + sh) * dh + ih - 1) / ih);
	if (dx1 <= dx0) dx1 = dx0 + 1;
	if (dy1 <= dy0) dy1 = dy0 + 1;
	if (dx0 < 0) dx0 = 0;
	if (dy0 < 0) dy0 = 0;
	if (dx1 > cw) dx1 = cw;
	if (dy1 > ch) dy1 = ch;

	HDC sdc = g_image.GetDC();
	SetStretchBltMode(g_cacheDC, HALFTONE);
	StretchBlt(g_cacheDC, dx0, dy0, dx1 - dx0, dy1 - dy0, sdc, sx, sy, sw, sh, SRCCOPY);
	g_image.ReleaseDC();

	/* 图像两侧的黑边(不同宽高比)也要刷一次，避免残影 */
	RECT rc = { 0, 0, cw, ch };
	if (d.left > 0) { RECT l = { 0, 0, d.left, ch };           FillRect(g_cacheDC, &l, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
	if (d.right < cw) { RECT r = { d.right, 0, cw, ch };       FillRect(g_cacheDC, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
	if (d.top > 0) { RECT u = { d.left, 0, d.right, d.top };   FillRect(g_cacheDC, &u, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
	if (d.bottom < ch) { RECT b = { d.left, d.bottom, d.right, ch }; FillRect(g_cacheDC, &b, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
	(void)rc;
}

/* 整图重画进后台缓冲 */
static void CacheRenderFull()
{
	if (g_image.IsNull())
		return;
	CacheRenderRegion(0, 0, g_image.GetWidth(), g_image.GetHeight());
}

/* 源图区域 → 客户区坐标(用于只重画变化的那一块) */
static bool CacheMapSrcToClient(int sx, int sy, int sw, int sh, RECT& out)
{
	if (g_view == NULL || g_image.IsNull())
		return false;
	RECT cr;
	GetClientRect(g_view, &cr);
	const int cw = cr.right, ch = cr.bottom;
	const int iw = g_image.GetWidth(), ih = g_image.GetHeight();
	if (cw <= 0 || ch <= 0 || iw <= 0 || ih <= 0)
		return false;
	RECT d;
	CalcDstRect(cw, ch, iw, ih, d);
	const int dw = d.right - d.left, dh = d.bottom - d.top;
	out.left   = d.left + (int)((long long)sx * dw / iw);
	out.top    = d.top  + (int)((long long)sy * dh / ih);
	out.right  = d.left + (int)(((long long)(sx + sw) * dw + iw - 1) / iw);
	out.bottom = d.top  + (int)(((long long)(sy + sh) * dh + ih - 1) / ih);
	if (out.right  <= out.left) out.right  = out.left + 1;
	if (out.bottom <= out.top)  out.bottom = out.top + 1;
	if (out.left < 0) out.left = 0;
	if (out.top < 0) out.top = 0;
	if (out.right > cw) out.right = cw;
	if (out.bottom > ch) out.bottom = ch;
	return true;
}

static void ViewSendMouse(int action, int xPos, int yPos, int delta = 0)
{
	if (g_view == NULL)
		return;
	RECT rc;
	GetClientRect(g_view, &rc);
	const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	if (cw <= 0 || ch <= 0)
		return;

	int rw = 0, rh = 0;
	EnterCriticalSection(&g_state_lock);
	if (g_has_image)
	{
		rw = g_image.GetWidth();
		rh = g_image.GetHeight();
	}
	LeaveCriticalSection(&g_state_lock);
	if (rw <= 0 || rh <= 0)
		return;

	RECT d;
	CalcDstRect(cw, ch, rw, rh, d);
	const int dw = d.right - d.left, dh = d.bottom - d.top;
	if (dw <= 0 || dh <= 0)
		return;
	if (xPos < d.left || yPos < d.top || xPos >= d.right || yPos >= d.bottom)
		return;                    /* 黑边区域不发送 */

	Mouse m;
	memset(&m, 0, sizeof(m));
	m.action = action;
	m.ptXY.x = (xPos - d.left) * rw / dw;
	m.ptXY.y = (yPos - d.top) * rh / dh;
	m.delta = delta;                 /* 滚轮增量(±120 的倍数), 非滚轮事件为 0 */
	SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
}

/* v13: 鼠标移动节流/合并。
   原实现每次 WM_MOUSEMOVE 都立刻发一个包 —— 鼠标一动就是每秒几百个包,被控端要逐个 SendInput,
   输入队列越堆越长,点击/拖拽就会"卡一下"。这里把移动合并到最多约 120 次/秒,坐标只保留最新值;
   按键/滚轮事件发送前先补发最后位置,保证点击落点精确。 */
static int   s_mv_pend_x = -1, s_mv_pend_y = -1;
static DWORD s_mv_last_tick = 0;
static void ViewFlushMove()
{
	if (s_mv_pend_x < 0)
		return;
	const int mx = s_mv_pend_x, my = s_mv_pend_y;
	s_mv_pend_x = s_mv_pend_y = -1;
	s_mv_last_tick = GetTickCount();
	ViewSendMouse(MOUSE_MOVE, mx, my);
}
static void ViewSendMove(int mx, int my)
{
	s_mv_pend_x = mx;
	s_mv_pend_y = my;
	if (GetTickCount() - s_mv_last_tick >= 8)
		ViewFlushMove();
}

/* ---------------- 拖放 / 粘贴 上传 (本地文件 -> 被控端当前目录) ---------------- */
static std::string WPathToAnsi(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
	if (n <= 0) return std::string();
	std::string a((size_t)n, '\0');
	WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &a[0], n, NULL, NULL);
	return a;
}

#define RC_DROP_MAX 128
static int FilesFromHDROP(HDROP hd, std::wstring* out, int maxN)
{
	int cnt = 0;
	UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0);
	for (UINT i = 0; i < n && cnt < maxN; ++i)
	{
		wchar_t buf[MAX_PATH * 2] = { 0 };
		if (DragQueryFileW(hd, i, buf, _countof(buf)) == 0) continue;
		out[cnt++] = buf;
	}
	return cnt;
}

static void UploadLocalList(const std::wstring* paths, int cnt)
{
	if (!g_connected) { ConPrint("  未连接被控端, 无法上传"); return; }
	const std::string dir = g_cur_path[0] ? std::string(g_cur_path) : std::string("(被控端默认目录)");
	int ok = 0, skip = 0;
	for (int i = 0; i < cnt; ++i)
	{
		const DWORD attr = GetFileAttributesW(paths[i].c_str());
		if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) { ++skip; continue; }
		ExtStartUpload(WPathToAnsi(paths[i]), std::string());   /* 远端目录留空 = 被控端当前目录 */
		++ok;
	}
	ConPrint("  已提交 %d 个文件上传到 %s%s", ok, dir.c_str(), skip ? " (跳过文件夹/无效路径)" : "");
}

static bool PasteLocalFilesFromClipboard(void)
{
	if (!g_connected) return false;
	if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
	if (!OpenClipboard(g_hwnd)) { ConPrint("  粘贴上传: 打开剪贴板失败"); return false; }
	bool handled = false;
	HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
	if (hd != NULL)
	{
		std::wstring files[RC_DROP_MAX];
		const int n = FilesFromHDROP(hd, files, RC_DROP_MAX);
		if (n > 0) { UploadLocalList(files, n); handled = true; }
		else ConPrint("  粘贴上传: 剪贴板里的文件列表为空");
	}
	CloseClipboard();
	return handled;
}

static void OnFilesDropped(HDROP hd)
{
	std::wstring files[RC_DROP_MAX];
	const int n = FilesFromHDROP(hd, files, RC_DROP_MAX);
	if (!g_connected)
		ConPrint("  拖放上传: 未连接被控端");
	else if (n > 0)
		UploadLocalList(files, n);
	else
		ConPrint("  拖放上传: 未解析到文件(请从资源管理器拖动文件/文件夹里的文件)");
	DragFinish(hd);
}

/* ============================================================
 *  v7: 摄像头 —— 控制端独立小窗口实时显示
 *    独立窗口按钮/命令 -> CMD_CAMERA(请求被控端采集)
 *    -> CMD_CAMERA_FRAME(JPEG) -> GDI+ 解码 -> 等比绘制
 * ============================================================ */
#define WM_APP_CAMFRAME   (WM_APP + 11)
#define CAM_WND_CLASS     "RcCamWnd"

static HWND g_cam_wnd = NULL;                 /* 独立小窗口 */
static CRITICAL_SECTION g_cam_lock;
static bool g_cam_lock_ready = false;
static std::vector<char> g_cam_jpg;           /* 最近一帧 JPEG(收包线程写) */
static int  g_cam_frm_w = 0, g_cam_frm_h = 0; /* 帧原始尺寸 */
static Gdiplus::Bitmap* g_cam_bmp = NULL;     /* 解码位图(仅窗口线程访问) */
static volatile LONG g_cam_on = 0;            /* 是否已请求采集 */
static volatile LONG g_cam_pending = 0;       /* 排队待显示帧数(丢旧帧用) */
static int  g_cam_reqW = 640, g_cam_reqH = 360;/* 请求采集分辨率(0=原生) */
static int  g_cam_reqFps = 15, g_cam_reqQ = 70;/* 请求帧率/质量 */
static bool g_cam_fallback_tried = false;      /* 协商失败后是否已回退原生分辨率 */
static char g_cam_status[192] = "未开始";
static ULONG_PTR g_cam_gdip_token = 0;
static int  g_cam_frames_n = 0, g_cam_fps_val = 0, g_cam_total = 0;
static ULONGLONG g_cam_fps_t0 = 0;

static void ExtCamLock()
{
	if (!g_cam_lock_ready) { InitializeCriticalSection(&g_cam_lock); g_cam_lock_ready = true; }
}
static bool ExtCamGdip()
{
	if (g_cam_gdip_token != 0) return true;
	Gdiplus::GdiplusStartupInput gsi;
	if (Gdiplus::GdiplusStartup(&g_cam_gdip_token, &gsi, NULL) != Gdiplus::Ok) { g_cam_gdip_token = 0; return false; }
	return true;
}
static bool ExtCamSend(int op, int index, int w, int h, int fps, int quality)
{
	CameraReq r;
	memset(&r, 0, sizeof(r));
	r.op = op; r.index = index; r.width = w; r.height = h;
	r.fps = (fps >= 1 && fps <= 60) ? fps : 10;
	r.quality = (quality >= 1 && quality <= 100) ? quality : 75;
	return SendCmdPacket(CMD_CAMERA, (char*)&r, sizeof(r));
}
/* JPEG 字节流 -> 独立的 Gdiplus::Bitmap(与 HGLOBAL 生命周期解耦) */
static Gdiplus::Bitmap* ExtCamDecode(const std::vector<char>& jpg)
{
	if (jpg.size() < 8) return NULL;
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, jpg.size());
	if (hg == NULL) return NULL;
	void* dst = GlobalLock(hg);
	if (dst == NULL) { GlobalFree(hg); return NULL; }
	memcpy(dst, &jpg[0], jpg.size());
	GlobalUnlock(hg);
	IStream* stm = NULL;
	if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stm)) || stm == NULL) { GlobalFree(hg); return NULL; }
	Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromStream(stm);
	Gdiplus::Bitmap* out = NULL;
	if (src != NULL && src->GetLastStatus() == Gdiplus::Ok && src->GetWidth() > 0 && src->GetHeight() > 0)
	{
		out = new Gdiplus::Bitmap((INT)src->GetWidth(), (INT)src->GetHeight(), PixelFormat32bppPARGB);
		if (out != NULL)
		{
			Gdiplus::Graphics g(out);
			g.DrawImage(src, 0, 0, (INT)src->GetWidth(), (INT)src->GetHeight());
		}
	}
	if (src != NULL) delete src;
	stm->Release();
	return out;
}
static void ExtCamSetTitle()
{
	if (g_cam_wnd == NULL) return;
	char t[256];
	_snprintf_s(t, _TRUNCATE, "摄像头 - %s   %dx%d  %d fps", g_cam_status, g_cam_frm_w, g_cam_frm_h, g_cam_fps_val);
	SetWindowTextA(g_cam_wnd, t);
}
static void ExtCamStop(bool notify)
{
	if (notify && InterlockedCompareExchange((volatile LONG*)&g_cam_on, 0, 0) != 0)
		ExtCamSend(RC_CAM_STOP, 0, 0, 0, 10, 75);
	InterlockedExchange((volatile LONG*)&g_cam_on, 0);
	if (g_cam_bmp != NULL) { delete g_cam_bmp; g_cam_bmp = NULL; }
	strcpy_s(g_cam_status, "已停止");
	if (g_cam_wnd != NULL) { ExtCamSetTitle(); InvalidateRect(g_cam_wnd, NULL, FALSE); }
}
static void ExtCamPaint(HWND hwnd, HDC hdc)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
	const int stH = 20;
	RECT body = rc;
	body.bottom = rc.bottom - stH;
	if (body.bottom < body.top + 1) body.bottom = body.top + 1;
	if (g_cam_bmp != NULL)
	{
		Gdiplus::Graphics g(hdc);
		g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
		const int bw = (int)g_cam_bmp->GetWidth();
		const int bh = (int)g_cam_bmp->GetHeight();
		double sx = (double)(body.right - body.left) / (bw > 0 ? bw : 1);
		double sy = (double)(body.bottom - body.top) / (bh > 0 ? bh : 1);
		double s = (sx < sy) ? sx : sy;
		int dw = (int)(bw * s), dh = (int)(bh * s);
		if (dw < 1) dw = 1;
		if (dh < 1) dh = 1;
		g.DrawImage(g_cam_bmp, (body.right - dw) / 2, (body.bottom - dh) / 2, dw, dh);
	}
	else
	{
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(190, 190, 190));
		RECT tt = body;
		tt.top = (body.top + body.bottom) / 2 - 10;
		DrawTextA(hdc, "等待摄像头画面…(被控端需有可用摄像头)", -1, &tt, DT_CENTER | DT_SINGLELINE);
	}
	{
		char st[256];
		_snprintf_s(st, _TRUNCATE, "状态: %s    %dx%d   %d fps   累计 %d 帧",
			g_cam_status, g_cam_frm_w, g_cam_frm_h, g_cam_fps_val, g_cam_total);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(0, 220, 120));
		RECT rb = rc;
		rb.top = rc.bottom - stH;
		DrawTextA(hdc, st, -1, &rb, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
	}
}
static LRESULT CALLBACK CamProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		ExtCamPaint(hwnd, hdc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_SIZE:
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	case WM_APP_CAMFRAME:
	{
		/* 收帧线程每来一帧先 ++, 这里 --; 若仍有排队帧说明解码跟不上,
		   直接跳过本次解码, 让后面的消息去处理最新帧, 避免延迟越积越大 */
		InterlockedDecrement(&g_cam_pending);
		if (InterlockedCompareExchange(&g_cam_pending, 0, 0) > 0)
		{
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		}
		std::vector<char> jpg;
		ExtCamLock();
		EnterCriticalSection(&g_cam_lock);
		jpg.swap(g_cam_jpg);
		LeaveCriticalSection(&g_cam_lock);
		if (!jpg.empty())
		{
			Gdiplus::Bitmap* nb = ExtCamDecode(jpg);
			if (nb != NULL)
			{
				if (g_cam_bmp != NULL) delete g_cam_bmp;
				g_cam_bmp = nb;
			}
			++g_cam_total;
			++g_cam_frames_n;
			const ULONGLONG now = GetTickCount64();
			if (g_cam_fps_t0 == 0) g_cam_fps_t0 = now;
			if (now - g_cam_fps_t0 >= 1000)
			{
				g_cam_fps_val = (int)(g_cam_frames_n * 1000 / (now - g_cam_fps_t0));
				g_cam_frames_n = 0;
				g_cam_fps_t0 = now;
				ExtCamSetTitle();
			}
		}
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}
	case WM_CLOSE:
		ExtCamStop(true);
		ShowWindow(hwnd, SW_HIDE);   /* 只隐藏, 按钮/命令可再次打开 */
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}
/* 控制端以管理员运行(UIPI)时默认收不到资源管理器(中完整性)发来的拖放消息, 需显式放行 */
static void ExtAllowDropMessages(HWND h)
{
	if (h == NULL) return;
	typedef BOOL (WINAPI *PFN_CWMFE)(HWND, UINT, DWORD, PVOID);
	static PFN_CWMFE pFn = NULL;
	static bool asked = false;
	if (!asked)
	{
		asked = true;
		HMODULE u = GetModuleHandleA("user32.dll");
		if (u != NULL) pFn = (PFN_CWMFE)GetProcAddress(u, "ChangeWindowMessageFilterEx");
	}
	if (pFn == NULL) return;
	const DWORD ALLOW = 1;         /* MSGFLT_ALLOW */
	pFn(h, WM_DROPFILES, ALLOW, NULL);
	pFn(h, 0x0049, ALLOW, NULL);   /* WM_COPYGLOBALDATA: DragQueryFile 取数据需要 */
	pFn(h, WM_COPYDATA, ALLOW, NULL);
}
static bool ExtCamMakeWindow()
{
	if (g_cam_wnd != NULL) return true;
	if (!ExtCamGdip()) { ConPrint("  摄像头: GDI+ 初始化失败"); return false; }
	ExtCamLock();
	WNDCLASSEXA wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = CamProc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = CAM_WND_CLASS;
	RegisterClassExA(&wc);
	g_cam_wnd = CreateWindowExA(WS_EX_TOOLWINDOW, CAM_WND_CLASS, "摄像头 - 未开始",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 580,
		NULL, NULL, wc.hInstance, NULL);
	if (g_cam_wnd == NULL) { ConPrint("  摄像头: 创建窗口失败"); return false; }
	return true;
}
static void ExtCamToggle(HWND owner)
{
	(void)owner;
	if (!g_connected) { ConPrint("  摄像头: 未连接被控端"); return; }
	if (!ExtCamMakeWindow()) return;
	if (InterlockedCompareExchange((volatile LONG*)&g_cam_on, 0, 0) != 0)
	{
		ExtCamStop(true);
		ConPrint("  摄像头: 已停止(窗口保留, 再次点击可恢复)");
		return;
	}
	g_cam_fallback_tried = false;
	if (ExtCamSend(RC_CAM_START, 0, g_cam_reqW, g_cam_reqH, g_cam_reqFps, g_cam_reqQ))
	{
		InterlockedExchange((volatile LONG*)&g_cam_on, 1);
		_snprintf_s(g_cam_status, _TRUNCATE, "正在启动 %dx%d %dfps q%d…", g_cam_reqW, g_cam_reqH, g_cam_reqFps, g_cam_reqQ);
		ShowWindow(g_cam_wnd, SW_SHOW);
		SetForegroundWindow(g_cam_wnd);
		InvalidateRect(g_cam_wnd, NULL, FALSE);
		ExtCamSetTitle();
		ConPrint("  摄像头: 已请求被控端开启(默认设备, 10fps, 独立小窗口)");
	}
	else
	{
		ConPrint("  摄像头: 请求发送失败(连接是否已断?)");
	}
}

static void ApplyScreenCtrl(bool forceFull);   /* 前置声明: WM_SIZE 中请求整帧用 */

static void ExtSetFullScreen(HWND hwnd, bool on);                        /* v10: 前置声明 */
static void ExtSetFitMode(HWND hwnd, LONG mode, int zoomDelta);          /* v10: 前置声明 */

LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	int x = (int)(short)LOWORD(lParam);
	int y = (int)(short)HIWORD(lParam);

	switch (msg)
	{
	case WM_ERASEBKGND:
		return 1;                  /* 全部由后台缓冲绘制，禁止系统擦背景(消除闪烁) */

	case WM_SIZE:
	{
		/* 尺寸变化(拖大/最大化): 必须立刻用现有基准图整屏重建缓存, 并向被控端请求一帧整帧。
		   原实现只 CacheFree() 清空缓存, 之后只等来脏矩形局部帧,
		   于是放大后大片区域长时间不刷新(表现为"卡顿、很慢才变成大画面")。 */
		CacheFree();
		EnterCriticalSection(&g_state_lock);
		if (!g_image.IsNull())
			CacheRenderFull();
		LeaveCriticalSection(&g_state_lock);
		InterlockedExchange(&g_need_full_render, 1);
		if (g_connected)
		{
			static ULONGLONG s_lastFullReq = 0;      /* 拖拽过程中防抖 */
			const ULONGLONG nowMs = GetTickCount64();
			if (nowMs - s_lastFullReq > 250)
			{
				s_lastFullReq = nowMs;
				ApplyScreenCtrl(true);
			}
		}
		InvalidateRect(hwnd, NULL, FALSE);
		return 0;
	}

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		EnterCriticalSection(&g_state_lock);
		if (g_cacheDC != NULL)
		{
			/* 后台缓冲已是完整画面：只做一次 SRCCOPY，无黑底闪烁 */
			int w = ps.rcPaint.right - ps.rcPaint.left;
			int h = ps.rcPaint.bottom - ps.rcPaint.top;
			if (w > 0 && h > 0)
				BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, w, h, g_cacheDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
		}
		else
		{
			RECT rc;
			GetClientRect(hwnd, &rc);
			FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
		}
		LeaveCriticalSection(&g_state_lock);
		ExtDrawMarker(hwnd, hdc);
		EndPaint(hwnd, &ps);
	}
	return 0;

	case WM_MOUSEMOVE:
	{
		/* 鼠标一移到远程画面上就把键盘焦点交给画面窗口:
		   否则焦点还停在 IP/命令行输入框里, 用户直接敲键盘时按键根本到不了被控端。
		   正在文本框/列表里操作(焦点是 Edit/ListBox)时不抢焦点。 */
		HWND f = GetFocus();
		if (f != hwnd)
		{
			char cls[40] = { 0 };
			bool typing = false;
			if (f != NULL && GetClassNameA(f, cls, sizeof(cls) - 1) != 0)
				typing = (_stricmp(cls, "Edit") == 0 || _stricmp(cls, "ListBox") == 0);
			if (!typing)
				SetFocus(hwnd);
		}
		ViewSendMove(x, y);
	}
	break;
	/* 按下时抓捕获、抬起才释放: 拖拽/框选时鼠标移出窗口也不会丢抬起事件 */
	case WM_LBUTTONDOWN:    SetFocus(hwnd); SetCapture(hwnd); ViewFlushMove(); ViewSendMouse(MOUSE_LDOWN, x, y); break;
	case WM_LBUTTONUP:      if (GetCapture() == hwnd) ReleaseCapture(); ViewFlushMove(); ViewSendMouse(MOUSE_LUP, x, y); break;
	case WM_RBUTTONDOWN:    SetFocus(hwnd); SetCapture(hwnd); ViewFlushMove(); ViewSendMouse(MOUSE_RDOWN, x, y); break;
	case WM_RBUTTONUP:      if (GetCapture() == hwnd) ReleaseCapture(); ViewFlushMove(); ViewSendMouse(MOUSE_RUP, x, y); break;
	case WM_MBUTTONDOWN:    SetFocus(hwnd); SetCapture(hwnd); ViewFlushMove(); ViewSendMouse(MOUSE_MDOWN, x, y); break;
	case WM_MBUTTONUP:      if (GetCapture() == hwnd) ReleaseCapture(); ViewFlushMove(); ViewSendMouse(MOUSE_MUP, x, y); break;
	case WM_LBUTTONDBLCLK:  ViewFlushMove(); ViewSendMouse(MOUSE_LDCLICK, x, y); break;
	case WM_RBUTTONDBLCLK:  ViewFlushMove(); ViewSendMouse(MOUSE_RDCLICK, x, y); break;
	case WM_XBUTTONDOWN:
	{
		const int b = GET_XBUTTON_WPARAM(wParam);
		ViewFlushMove(); ViewSendMouse((b == XBUTTON1) ? MOUSE_X1DOWN : MOUSE_X2DOWN, x, y);
		break;
	}
	case WM_XBUTTONUP:
	{
		const int b = GET_XBUTTON_WPARAM(wParam);
		ViewFlushMove(); ViewSendMouse((b == XBUTTON1) ? MOUSE_X1UP : MOUSE_X2UP, x, y);
		break;
	}
	/* 滚轮: lParam 是屏幕坐标, 先换算成客户区坐标, 再映射成被控端坐标 */
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
	{
		POINT sp; sp.x = GET_X_LPARAM(lParam); sp.y = GET_Y_LPARAM(lParam);
		ScreenToClient(hwnd, &sp);
		ViewFlushMove(); ViewSendMouse((msg == WM_MOUSEWHEEL) ? MOUSE_WHEEL : MOUSE_HWHEEL, sp.x, sp.y,
		              GET_WHEEL_DELTA_WPARAM(wParam));
		break;
	}
	/* 把本地文件直接拖到远程画面上 = 上传到被控端当前目录 */
	case WM_DROPFILES:
		OnFilesDropped((HDROP)wParam);
		break;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		if ((int)wParam == VK_PROCESSKEY) break;   /* 输入法已吃掉该键, 不发假码 */
		/* v10: F11 切换全屏; 全屏下 Esc 退出全屏(不进远程) */
		if ((int)wParam == VK_F11)
		{
			HWND root = GetAncestor(hwnd, GA_ROOT);
			ExtSetFullScreen(root, InterlockedCompareExchange(&g_fullscreen, 0, 0) == 0);
			break;
		}
		if ((int)wParam == VK_ESCAPE && InterlockedCompareExchange(&g_fullscreen, 0, 0) != 0)
		{
			HWND root = GetAncestor(hwnd, GA_ROOT);
			ExtSetFullScreen(root, false);
			break;
		}
		/* Ctrl+F11: 铺满窗口 / Ctrl+F12: 适应窗口 快捷切换 */
		if ((int)wParam == VK_F12 && (GetKeyState(VK_CONTROL) & 0x8000))
		{
			HWND root = GetAncestor(hwnd, GA_ROOT);
			ExtSetFitMode(root, 0, 0);
			break;
		}
		/* Ctrl+V: 本地剪贴板里如果是文件(在资源管理器里复制的), 直接上传到被控端当前目录 */
		if ((int)wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && PasteLocalFilesFromClipboard())
			break;
		KeyBoard kb;
		memset(&kb, 0, sizeof(kb));
		kb.virtual_code = (int)wParam;
		kb.key_status = RC_KEY_DOWN;
		kb.scan_code = (unsigned int)((lParam >> 16) & 0xFF);
		kb.flags = ((lParam >> 24) & 1) ? RC_KF_EXTENDED : 0;
		SendCmdPacket(CMD_KEYBOARD, (char*)&kb, sizeof(kb));
	}
	break;

	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		KeyBoard kb;
		memset(&kb, 0, sizeof(kb));
		kb.virtual_code = (int)wParam;
		kb.key_status = RC_KEY_UP;
		kb.scan_code = (unsigned int)((lParam >> 16) & 0xFF);
		kb.flags = ((lParam >> 24) & 1) ? RC_KF_EXTENDED : 0;
		SendCmdPacket(CMD_KEYBOARD, (char*)&kb, sizeof(kb));
	}
	break;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ============================================================
 *  接收线程 / 心跳 / 屏幕请求
 * ============================================================ */
static void TryReconnect();
static void ApplyScreenCtrl(bool forceFull);

/* 下载状态补充变量 */
static unsigned long long g_down_bytes = 0;

static void HandleScreenPacket(Packet* p)
{
	if (p->header.body_len < (int)sizeof(ScreenFrameHead))
		return;
	ScreenFrameHead head;
	memcpy(&head, p->body, sizeof(head));
	const char* data = p->body + sizeof(head);
	const int dataLen = p->header.body_len - (int)sizeof(head);

	/* 统计学：帧率 / 码率 / 往返延迟 */
	InterlockedExchange(&g_frame_pending, 0);
	if (g_frame_evt != NULL)
		SetEvent(g_frame_evt);
	InterlockedIncrement(&g_stat_frames);
	InterlockedExchangeAdd(&g_stat_bytes, dataLen > 0 ? dataLen : 0);
	if (g_last_req_tick != 0)
		g_last_rtt = (LONG)(GetTickCount64() - g_last_req_tick);

	if (head.w <= 0 || head.h <= 0 || dataLen <= 0)
		return;                         /* 无变化帧：什么都不用画 */

	CImage region;
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)dataLen);
	if (hMem == NULL)
		return;
	void* dst = GlobalLock(hMem);
	memcpy(dst, data, (size_t)dataLen);
	GlobalUnlock(hMem);

	IStream* pStream = NULL;
	bool ok = false;
	if (CreateStreamOnHGlobal(hMem, FALSE, &pStream) == S_OK)
	{
		if (SUCCEEDED(region.Load(pStream)))
			ok = true;
		pStream->Release();
	}
	GlobalFree(hMem);
	if (!ok || region.IsNull())
		return;

	EnterCriticalSection(&g_state_lock);
	bool needNew = g_image.IsNull() || g_image.GetWidth() != head.full_w ||
		g_image.GetHeight() != head.full_h;
	if (needNew)
	{
		if (!g_image.IsNull())
			g_image.Destroy();
		if (head.full_w > 0 && head.full_h > 0)
		{
			g_image.Create(head.full_w, head.full_h, 32);   /* 深拷贝到自己的基准图 */
			g_has_image = true;
		}
	}
	bool fullRendered = false;
	if (!g_image.IsNull())
	{
		HDC sdc = region.GetDC();
		HDC ddc = g_image.GetDC();
		if (sdc != NULL && ddc != NULL)
			BitBlt(ddc, head.x, head.y, head.w, head.h, sdc, 0, 0, SRCCOPY);
		if (ddc != NULL) g_image.ReleaseDC();
		if (sdc != NULL) region.ReleaseDC();

		if ((head.flags & RC_FR_FULL) || needNew)
		{
			CacheRenderFull();
			InterlockedExchange(&g_need_full_render, 0);    /* 整帧已到, 恢复正常局部刷新 */
			fullRendered = true;
		}
		else if (InterlockedCompareExchange(&g_need_full_render, 1, 1) != 0)
		{
			CacheRenderFull();                              /* 尺寸刚变: 局部帧也整屏重建 */
			fullRendered = true;
		}
		else
			CacheRenderRegion(head.x, head.y, head.w, head.h);
	}
	LeaveCriticalSection(&g_state_lock);

	/* 只重画变化区域 → 主线程几乎不做缩放，彻底消除整屏闪烁 */
	if (g_view != NULL)
	{
		RECT ir;
		if (fullRendered || !CacheMapSrcToClient(head.x, head.y, head.w, head.h, ir))
			InvalidateRect(g_view, NULL, FALSE);
		else
			InvalidateRect(g_view, &ir, FALSE);
	}
}

static void HandleFileListPacket(Packet* p)
{
	std::string text(p->body, (size_t)p->header.body_len);
	if (text.compare(0, 6, "@path\t") == 0)
	{
		/* 被控端回传的规范化绝对路径：同步地址栏，作为后续相对路径基准 */
		size_t nl = text.find('\n');
		std::string dir = text.substr(6, (nl == std::string::npos) ? std::string::npos : nl - 6);
		if (!dir.empty())
		{
			strncpy_s(g_cur_path, dir.c_str(), _TRUNCATE);
			if (g_hwnd != NULL)
				SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_PATH), g_cur_path);
		}
		text.erase(0, (nl == std::string::npos) ? text.size() : nl + 1);
	}
	if (text.compare(0, 4, "err:") == 0)
		LogLine("目录读取失败: %s", text.c_str() + 4);

	std::vector<FileEntry> list;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
		pos = (nl == std::string::npos) ? text.size() : nl + 1;
		if (line.size() < 6)
			continue;
		size_t t1 = line.find('\t');
		if (t1 == std::string::npos) continue;
		size_t t2 = line.find('\t', t1 + 1);
		if (t2 == std::string::npos) continue;
		size_t t3 = line.find('\t', t2 + 1);
		if (t3 == std::string::npos) continue;

		FileEntry e;
		e.kind = line[0];
		e.name = line.substr(t1 + 1, t2 - t1 - 1);
		e.size = _strtoui64(line.substr(t2 + 1, t3 - t2 - 1).c_str(), NULL, 10);
		e.time = line.substr(t3 + 1);
		list.push_back(e);
	}

	EnterCriticalSection(&g_state_lock);
	g_entries.swap(list);
	LeaveCriticalSection(&g_state_lock);

	if (g_hwnd != NULL)
		PostMessageA(g_hwnd, WM_APP_FILLLIST, 0, 0);
}

static void HandleServerPacket(Packet* p)
{
	if (ExtOnPacket(p))
		return;
	switch (p->header.cmd)
	{
	case CMD_SCREEN:
		HandleScreenPacket(p);
		break;

	case CMD_HEARTBEAT:
		g_last_hb = GetTickCount64();
		break;

	case CMD_FILE_LIST_RES:
		HandleFileListPacket(p);
		break;

	case CMD_FILE_DATA:
	{
		if (p->header.body_len < (int)sizeof(FileChunk))
			break;
		FileChunk c;
		memcpy(&c, p->body, sizeof(FileChunk));
		if (g_down_active && c.file_id == g_down_id && g_down_fp != NULL)
		{
			int payload = p->header.body_len - (int)sizeof(FileChunk);
			if (payload > 0)
			{
				fwrite(p->body + sizeof(FileChunk), 1, (size_t)payload, g_down_fp);
				g_down_bytes += payload;
			}
		}
	}
	break;

	case CMD_FILE_END:
	{
		if (p->header.body_len < (int)sizeof(FileChunk))
			break;
		FileChunk c;
		memcpy(&c, p->body, sizeof(FileChunk));
		if (c.file_id == g_down_id)
		{
			if (g_down_fp != NULL)
			{
				fclose(g_down_fp);
				g_down_fp = NULL;
			}
			InterlockedExchange(&g_down_active, 0);
			if (c.flags == RC_FF_ERROR)
				LogLine("下载失败(被控端返回错误): %s", g_down_dir.c_str());
			else
				LogLine("下载完成: %s (%llu 字节)", g_down_dir.c_str(), g_down_bytes);
		}
	}
	break;

	case CMD_FILE_PROGRESS:
		LogLine("被控端: %s", std::string(p->body, (size_t)p->header.body_len).c_str());
		break;

	case CMD_FILE_OP_RES:
		LogLine("文件操作: %s", std::string(p->body, (size_t)p->header.body_len).c_str());
		break;

	case CMD_TESTCONNECT:
		LogLine("连接测试成功");
		break;

	default:
		LogLine("未知响应 cmd=%d len=%d", p->header.cmd, p->header.body_len);
		break;
	}
}

DWORD WINAPI RecvThread(LPVOID)
{
	PacketStream stream;
	std::vector<char> buf(65536);

	while (g_running)
	{
		SOCKET s = g_sock;
		if (s == INVALID_SOCKET)
		{
			Sleep(100);
			continue;
		}

		int len = recv(s, buf.data(), (int)buf.size(), 0);
		if (len > 0)
		{
			stream.Append(buf.data(), len);
			Packet* p = NULL;
			while ((p = stream.Next()) != NULL)
			{
				HandleServerPacket(p);
				free(p);
			}
		}
		else if (len == 0)
		{
			LogLine("被控端关闭了连接");
			stream.Reset();
			TryReconnect();
		}
		else
		{
			int err = WSAGetLastError();
			if (err == WSAETIMEDOUT)
			{
				if (g_connected && GetTickCount64() - g_last_hb > RC_HB_TIMEOUT)
				{
					LogLine("心跳超时，判定掉线");
					stream.Reset();
					TryReconnect();
				}
				continue;
			}
			if (err == WSAEINTR)
				continue;
			LogLine("recv 错误 %d", err);
			stream.Reset();
			TryReconnect();
		}
	}
	return 0;
}

DWORD WINAPI HeartbeatThread(LPVOID)
{
	while (g_running)
	{
		if (g_connected)
			SendCmdPacket(CMD_HEARTBEAT, NULL, 0);
		Sleep(RC_HB_INTERVAL);
	}
	return 0;
}

DWORD WINAPI ScreenThread(LPVOID)
{
	while (g_running)
	{
		if (!g_connected)
		{
			Sleep(200);
			continue;
		}

		/* 乒乓式请求：上一帧没回来就不发下一帧，避免请求堆积导致越来越卡 */
		if (InterlockedCompareExchange(&g_frame_pending, 1, 0) != 0)
		{
			WaitForSingleObject(g_frame_evt, 100);   /* g_frame_evt 可能为 NULL → 退化为 100ms 等待 */
			continue;
		}

		int fps = g_fps > 0 ? g_fps : 30;
		if (fps > 60) fps = 60;
		/* v13: 文件上传/下载进行中时把画面刷新降到 5fps —— 带宽和 CPU 优先给文件,
		   传输速度明显提升(传完自动恢复)。 */
		if ((InterlockedCompareExchange(&g_down_active, 0, 0) != 0 ||
		     InterlockedCompareExchange(&g_up_active, 0, 0) != 0) && fps > 5)
			fps = 5;
		const ULONGLONG t0 = GetTickCount64();
		g_last_req_tick = t0;
		SendCmdPacket(CMD_SCREEN, NULL, 0);

		/* 按设定帧率补足间隔；一旦回帧立刻进入下一轮(自适应提速) */
		for (;;)
		{
			const ULONGLONG dt = GetTickCount64() - t0;
			const ULONGLONG target = (ULONGLONG)(1000 / fps);
			if (dt >= target)
				break;
			if (g_frame_evt != NULL)
			{
				if (WaitForSingleObject(g_frame_evt, 5) == WAIT_OBJECT_0)
					break;                       /* 已经回来了，赶紧请求下一帧 */
			}
			else
			{
				Sleep(2);
				if (g_frame_pending == 0)
					break;
			}
			if (!g_running || !g_connected)
				break;
		}
	}
	return 0;
}

/* ============================================================
 *  重连 / 连接
 * ============================================================ */
/* 版本握手: 询问被控端协议版本/能力。
   旧版被控端不认识 CMD_HELLO(201)，不会回包 —— WM_TIMER 里会据此明确提示"被控端版本过旧"，
   避免出现"命令已发送但永远没有回包"却无从判断原因的情况。 */
static void RequestServerHello()
{
	InterlockedExchange(&g_server_hello, 0);
	InterlockedExchange(&g_hello_warned, 0);
	SendCmdPacket(CMD_HELLO, NULL, 0);
	g_hello_tick = GetTickCount64();
}

static void TryReconnect()
{
	SOCKET old = g_sock;
	g_sock = INVALID_SOCKET;
	if (old != INVALID_SOCKET)
		closesocket(old);
	SetConnected(false);

	if (!g_auto_reconnect || g_manual_disconnect)
	{
		InterlockedExchange(&g_manual_disconnect, 0);
		LogLine("连接已断开（点击“连接”可重新连接）");
		return;
	}
	while (g_running)
	{
		LogLine("尝试重连 %s:%d ...", g_ip, g_port);
		SOCKET s = ConnectTo(g_ip, g_port);
		if (s != INVALID_SOCKET)
		{
			g_sock = s;
			g_last_hb = GetTickCount64();
			SetConnected(true);
			LogLine("重连成功");
			SendCmdPacket(CMD_TESTCONNECT, NULL, 0);
			RequestServerHello();     /* 查询被控端版本(判断是否旧版) */
			ApplyScreenCtrl(true);    /* 重连后重建基准帧 */
			return;
		}
		Sleep(3000);
	}
}

static void ApplyScreenCtrl(bool forceFull)
{
	ScreenCtrl sc;
	memset(&sc, 0, sizeof(sc));
	sc.fps = g_fps;
	sc.quality = g_quality;
	sc.encode = g_encoder;
	sc.monitor = 1;
	sc.force_full = forceFull ? 1 : 0;
	SendCmdPacket(CMD_SCREEN_CTRL, (char*)&sc, sizeof(sc));
}

static void DoConnect()
{
	if (g_connected)
	{
		LogLine("已经处于连接状态");
		return;
	}
	GetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_IP), g_ip, sizeof(g_ip) - 1);
	char portbuf[16] = { 0 };
	GetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_PORT), portbuf, sizeof(portbuf) - 1);
	int port = atoi(portbuf);
	if (port <= 0 || port > 65535)
		port = 9999;
	g_port = port;

	LogLine("正在连接 %s:%d ...", g_ip, g_port);
	SOCKET s = ConnectTo(g_ip, g_port);
	if (s == INVALID_SOCKET)
	{
		LogLine("连接失败: %d（检查被控端是否运行、IP/端口是否正确）", WSAGetLastError());
		return;
	}
	g_sock = s;
	g_last_hb = GetTickCount64();
	SetConnected(true);
	SaveSettings();            /* v12: 记住本次成功地址 */
	LogLine("连接成功");
	SendCmdPacket(CMD_TESTCONNECT, NULL, 0);
	RequestServerHello();             /* 查询被控端版本(旧版被控端不会回包) */
	ApplyScreenCtrl(true);            /* 请求整屏基准帧 */
	SendText(CMD_FILE_LIST, g_cur_path);
}

/* ============================================================
 *  文件浏览 / 传输
 * ============================================================ */
static std::string JoinPath(const std::string& dir, const std::string& name)
{
	std::string d = dir;
	if (!d.empty() && d.back() != '\\' && d.back() != '/')
		d += '\\';
	return d + name;
}

static void GetPathEdit(std::string& out)
{
	char buf[512] = { 0 };
	GetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_PATH), buf, sizeof(buf) - 1);
	out = buf;   /* 允许空：空路径由被控端解析为起始目录(--root 沙箱根) */
}

static void GetNameEdit(std::string& out)
{
	char buf[260] = { 0 };
	GetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_NAME), buf, sizeof(buf) - 1);
	out = buf;
}

static bool GetSelectedEntry(FileEntry& out)
{
	HWND lb = GetDlgItem(g_hwnd, IDC_LIST_FILES);
	int sel = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
	if (sel < 0)
		return false;
	EnterCriticalSection(&g_state_lock);
	bool ok = sel < (int)g_entries.size();
	if (ok)
		out = g_entries[sel];
	LeaveCriticalSection(&g_state_lock);
	return ok;
}

static void FillFileList()
{
	HWND lb = GetDlgItem(g_hwnd, IDC_LIST_FILES);
	if (lb == NULL)
		return;
	SendMessageA(lb, LB_RESETCONTENT, 0, 0);

	EnterCriticalSection(&g_state_lock);
	for (size_t i = 0; i < g_entries.size(); ++i)
	{
		const FileEntry& e = g_entries[i];
		char line[700] = { 0 };
		if (e.kind == 'D')
			_snprintf_s(line, _TRUNCATE, "[目录]  %s", e.name.c_str());
		else
			_snprintf_s(line, _TRUNCATE, "%-32s %12llu  %s", e.name.c_str(), e.size, e.time.c_str());
		SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
	}
	LeaveCriticalSection(&g_state_lock);
}

static void DoDownload(const FileEntry& e, const std::string& remotePath)
{
	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	char file[MAX_PATH] = { 0 };
	strncpy_s(file, e.name.c_str(), _TRUNCATE);
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_hwnd;
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrFilter = "所有文件\0*.*\0";
	ofn.lpstrTitle = "选择保存位置";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameA(&ofn))
		return;

	FILE* fp = NULL;
	if (fopen_s(&fp, file, "wb") != 0 || fp == NULL)
	{
		LogLine("无法创建本地文件: %s", file);
		return;
	}

	if (g_down_fp != NULL)
	{
		fclose(g_down_fp);
		g_down_fp = NULL;
	}
	g_down_fp = fp;
	g_down_id = (unsigned int)InterlockedIncrement(&g_id_counter);
	g_down_bytes = 0;
	g_down_dir = file;
	InterlockedExchange(&g_down_active, 1);

	FileRequest req;
	memset(&req, 0, sizeof(req));
	req.file_id = g_down_id;
	req.offset = 0;
	strncpy_s(req.path, remotePath.c_str(), _TRUNCATE);
	SendCmdPacket(CMD_FILE_DOWNLOAD, (char*)&req, sizeof(req));
	LogLine("开始下载 %s -> %s", remotePath.c_str(), file);
}

struct UploadJob
{
	unsigned int file_id;
	std::string  local;
	std::string  remote;
};

DWORD WINAPI UploadThread(LPVOID lpParam)
{
	UploadJob* job = (UploadJob*)lpParam;
	FILE* fp = NULL;
	if (fopen_s(&fp, job->local.c_str(), "rb") != 0 || fp == NULL)
	{
		LogLine("上传失败，无法打开本地文件: %s", job->local.c_str());
		delete job;
		return 0;
	}
	_fseeki64(fp, 0, SEEK_END);
	unsigned long long total = (unsigned long long)_ftelli64(fp);
	_fseeki64(fp, 0, SEEK_SET);

	FileRequest req;
	memset(&req, 0, sizeof(req));
	req.file_id = job->file_id;
	req.offset = 0;
	req.file_size = total;
	strncpy_s(req.path, job->remote.c_str(), _TRUNCATE);
	SendCmdPacket(CMD_FILE_UPLOAD_BEGIN, (char*)&req, sizeof(req));
	LogLine("开始上传 %s -> %s (%llu 字节)", job->local.c_str(), job->remote.c_str(), total);

	InterlockedExchange(&g_up_active, 1);   /* v13 */
	std::vector<char> buf(RC_FILE_CHUNK);
	unsigned long long offset = 0;
	unsigned int flags = RC_FF_BEGIN;

	while (g_connected)
	{
		size_t n = fread(buf.data(), 1, buf.size(), fp);
		if (n == 0)
			break;

		std::vector<char> body(sizeof(FileChunk) + n);
		FileChunk c;
		memset(&c, 0, sizeof(c));
		c.file_id = job->file_id;
		c.offset = offset;
		c.data_len = (unsigned int)n;
		c.flags = flags;
		memcpy(body.data(), &c, sizeof(FileChunk));
		memcpy(body.data() + sizeof(FileChunk), buf.data(), n);

		if (!SendCmdPacket(CMD_FILE_DATA, body.data(), (int)body.size()))
		{
			LogLine("上传中断（网络发送失败）");
			break;
		}
		offset += n;
		flags = RC_FF_DATA;
	}
	fclose(fp);
	InterlockedExchange(&g_up_active, 0);   /* v13 */

	FileChunk end;
	memset(&end, 0, sizeof(end));
	end.file_id = job->file_id;
	end.offset = offset;
	end.flags = RC_FF_END;
	SendCmdPacket(CMD_FILE_END, (char*)&end, sizeof(end));
	LogLine("上传结束: %s (%llu 字节)", job->remote.c_str(), offset);
	delete job;
	return 0;
}

static void StartUpload()
{
	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	char file[MAX_PATH] = { 0 };
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_hwnd;
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrFilter = "所有文件\0*.*\0";
	ofn.lpstrTitle = "选择要上传的文件";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameA(&ofn))
		return;

	const char* base = strrchr(file, '\\');
	base = (base != NULL) ? base + 1 : file;

	std::string remote = JoinPath(g_cur_path, base);
	UploadJob* job = new UploadJob();
	job->file_id = (unsigned int)InterlockedIncrement(&g_id_counter);
	job->local = file;
	job->remote = remote;

	DWORD tid = 0;
	HANDLE h = CreateThread(NULL, 0, UploadThread, job, 0, &tid);
	if (h != NULL)
		CloseHandle(h);
	else
	{
		delete job;
		LogLine("创建上传线程失败");
	}
}

static void SendFileOp(int op, const std::string& p1, const std::string& p2)
{
	FileOp fop;
	memset(&fop, 0, sizeof(fop));
	fop.op = op;
	strncpy_s(fop.path1, p1.c_str(), _TRUNCATE);
	if (!p2.empty())
		strncpy_s(fop.path2, p2.c_str(), _TRUNCATE);
	SendCmdPacket(CMD_FILE_OP, (char*)&fop, sizeof(fop));
}

/* ============================================================
 *  主窗口 UI
 * ============================================================ */
/* 统一 UI 字体: 矢量字体(微软雅黑), 避免点阵宋体中文糊/错位 */
static HFONT g_ui_font = NULL;
static HFONT ExtUiFont(void)
{
	if (g_ui_font == NULL)
	{
		g_ui_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
		if (g_ui_font == NULL)
			g_ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	}
	return g_ui_font;
}


static HWND Mk(HWND parent, const char* cls, const char* text, DWORD style,
	int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
	if (c != NULL)
		SendMessageA(c, WM_SETFONT, (WPARAM)ExtUiFont(), TRUE);
	return c;
}

/* ============================================================
 *  client_ext_top.inc —— 统一控制端扩展：控件ID / 前置声明 / 全局状态
 * ============================================================ */
#define IDC_TAB           1020
#define IDC_CMD_OUT       1021
#define IDC_CMD_IN        1022
/* --- v6: 声音(播放到本机)开关 --- */
#define IDC_BTN_AUDSYS    1070
#define IDC_SLD_AUDSYS    1071
#define IDC_BTN_AUDMIC    1072
#define IDC_SLD_AUDMIC    1073
#define IDC_AUD_STATUS    1074
#define IDC_BTN_RUN       1023
#define IDC_BTN_CLEAR     1024
#define IDC_BTN_HELP      1025
#define IDC_BTN_SNAP      1026
#define IDC_BTN_LOCATE    1027
#define IDC_BTN_CLIPGET   1028
#define IDC_BTN_CLIPSET   1029
#define IDC_BTN_SYSINFO   1030
#define IDC_BTN_CAM       1080   /* v7: 摄像头独立小窗口开关 */
#define IDC_EDIT_RECSEC   1031
#define IDC_BTN_RECSTART  1032
#define IDC_BTN_RECSTOP   1033
#define IDC_EDIT_RECOUT   1034
#define IDC_BTN_RECSAVE   1035
#define IDC_BTN_RECPLAY   1036
#define IDC_CHK_MOUSE     1037
#define IDC_CHK_CLIPSYNC  1081   /* v9: 剪贴板自动同步开关 */
/* v10: 画面铺展比例 / 全屏 */
#define IDC_BTN_FITFILL    1082   /* 铺满窗口(拉伸, 不留黑边) */
#define IDC_BTN_FITADAPT   1083   /* 适应窗口(等比, 保留黑边) */
#define IDC_BTN_FULLSCREEN 1084   /* 全屏切换 (F11) */
#define IDC_BTN_ZOOMIN     1085   /* 画面放大 +10% */
#define IDC_BTN_ZOOMOUT    1086   /* 画面缩小 -10% */
#define IDC_BTN_ZOOM100    1087   /* 1:1 原始尺寸 */
#define IDC_LBL_VIEWMODE   1088   /* 当前铺展模式文本 */
#define IDC_CHK_KEY       1038
#define IDC_EDIT_CLIPTXT  1039
#define IDC_BTN_CLIPCLR   1040
#define IDC_TOOL_OUT      1041
#define IDC_BTN_SNAPSAVE  1042
#define IDC_LBL_PATH      1043
#define IDC_LBL_NAME      1044
#define IDC_CMD_TIP       1045
#define IDC_REC_TIP       1046
#define IDC_TOOL_TIP      1047
#define IDC_REC_STATUS    1048
#define IDC_BTN_RECCLEAR  1049
#define IDC_PAGEBTN0      1060
#define IDC_PAGEBTN1      1061
#define IDC_PAGEBTN2      1062
#define IDC_PAGEBTN3      1063
#define WM_APP_CONSOLE    (WM_APP + 5)

#define WM_APP_SETPAGE    (WM_APP + 9)   /* wParam=页号: 供外部/测试切换页 */

/* 扩展页: 0=文件 1=命令行 2=键鼠录制 3=工具 */
enum { EXT_PAGE_FILES = 0, EXT_PAGE_CMD = 1, EXT_PAGE_REC = 2, EXT_PAGE_TOOL = 3 };

static void ExtDrawMarker(HDC hdc);
static bool ExtOnPacket(Packet* p);
static void ExtConsoleRefresh(HWND hwnd);
static bool ExtOnCommand(HWND hwnd, int id, int code);
static bool ExtOnNotify(HWND hwnd, LPARAM lParam);
static void ExtCreateControls(HWND hwnd);
static void ExtLayout(HWND hwnd);
static void ExtSetPage(HWND hwnd, int page);
static void ExtRunCommand(const std::string& line);
static void ExtOnConnected(HWND hwnd);
static void ExtGenerateAndroidApk(HWND hwnd);

/* ---- 扩展全局状态 ---- */
static HWND g_ext_tab = NULL;
static HWND g_ext_pagebtn[4] = { NULL, NULL, NULL, NULL };
static int  g_ext_page = EXT_PAGE_FILES;
static std::deque<std::string> g_con_lines;      /* 控制台输出(环形) */
static std::deque<std::string> g_cmd_hist;       /* 命令历史 */
static int  g_hist_pos = 0;
static std::string g_rec_text;                   /* 最近一次录制文本 */
static bool g_rec_expect = false;                /* 正在等待录制回传 */
static int  g_locate_x = -1, g_locate_y = -1;
static int  g_locate_sw = 0, g_locate_sh = 0;
static bool g_locate_on = false;                 /* 是否显示定位十字 */
static WNDPROC g_cmd_in_old = NULL;

/* ============================================================
 *  client_ext.inc —— 统一控制端扩展实现
 *  (Tab 面板 / 命令行控制台 / 截图 / 定位 / 键鼠录制 / 剪贴板 / 系统信息)
 * ============================================================ */
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

/* ---------------- 通用小工具 ---------------- */
static std::string ExtTrim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
	while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) b--;
	return s.substr(a, b - a);
}
static std::string ExtLower(std::string s)
{
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
	return s;
}
/* 按空白切词(保留引号内的空格) */
static std::vector<std::string> ExtSplit(const std::string& line)
{
	std::vector<std::string> out;
	std::string cur;
	bool inQ = false;
	for (size_t i = 0; i < line.size(); ++i)
	{
		char c = line[i];
		if (c == '"') { inQ = !inQ; continue; }
		if (!inQ && (c == ' ' || c == '\t')) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
		cur += c;
	}
	if (!cur.empty()) out.push_back(cur);
	return out;
}
static std::wstring ExtA2W(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
	if (n <= 0) return std::wstring();
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}
static std::string ExtExeDir()
{
	char p[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, p, MAX_PATH);
	char* s = strrchr(p, '\\');
	if (s != NULL) *s = '\0';
	return std::string(p);
}

static void ConPush(const std::string& s);
static void ConPrint(const char* fmt, ...);

/* ============================================================
 *  音频回传播放 (waveOut): 系统声音 + 麦克风 双播放器
 *    audio -> CMD_AUDIO/CMD_AUDIO_FRAME/CMD_AUDIO_RES
 *    mic   -> CMD_MIC  /CMD_MIC_FRAME  /CMD_MIC_RES
 *  两者各自独立 waveOut 设备, 可同时播放; 由控制端命令选择
 * ============================================================ */
#define RC_AUD_BUFS 12

struct SndOut
{
	const char*       name;      /* "audio" / "mic" */
	const char*       label;     /* 中文名 */
	int               cmd;       /* 请求命令 */
	int               cmdRes;    /* 状态回包命令 */
	HWAVEOUT          w;
	WAVEHDR           hdr[RC_AUD_BUFS];
	std::vector<char> buf[RC_AUD_BUFS];
	int               rate, ch;
	volatile LONG     on;
	int               vol;       /* 本地播放音量 0-100 */
	int               gain;      /* 被控端采集增益 10-800(%) */
	int               dev;       /* 被控端设备序号 0=默认 */
	int               bufms;     /* 每包时长 ms */
	volatile LONG     frames, lost, bytes;
	CRITICAL_SECTION  lock;
	bool              lockInit;
	char              fmt[48];
};

static SndOut g_sndSys;
static SndOut g_sndMic;

static SndOut* SndSys() { return &g_sndSys; }
static SndOut* SndMic() { return &g_sndMic; }

static void SndInit(SndOut* P, const char* name, const char* label, int cmd, int cmdRes)
{
	if (P->lockInit) return;
	P->name = name; P->label = label; P->cmd = cmd; P->cmdRes = cmdRes;
	P->vol = 100; P->gain = 100; P->dev = 0; P->bufms = 40;
	P->rate = 0; P->ch = 0; P->w = NULL;
	strcpy_s(P->fmt, sizeof(P->fmt), "-");
	InitializeCriticalSection(&P->lock);
	P->lockInit = true;
}

static void SndLock(SndOut* P)   { if (P->lockInit) EnterCriticalSection(&P->lock); }
static void SndUnlock(SndOut* P) { if (P->lockInit) LeaveCriticalSection(&P->lock); }

static void SndApplyVolume(SndOut* P)
{
	if (P->w == NULL) return;
	DWORD v = (DWORD)((P->vol * 0xFFFF) / 100);
	waveOutSetVolume(P->w, (v & 0xFFFF) | (v << 16));
}

static void SndClose(SndOut* P)
{
	SndLock(P);
	if (P->w != NULL)
	{
		waveOutReset(P->w);
		for (int i = 0; i < RC_AUD_BUFS; ++i)
		{
			if (P->hdr[i].lpData != NULL)
			{
				waveOutUnprepareHeader(P->w, &P->hdr[i], sizeof(WAVEHDR));
				P->hdr[i].lpData = NULL;
			}
		}
		waveOutClose(P->w);
		P->w = NULL;
	}
	P->rate = 0;
	P->ch = 0;
	SndUnlock(P);
}

/* 按收到的格式打开/切换 waveOut 设备 */
static bool SndOpen(SndOut* P, int rate, int ch)
{
	SndLock(P);
	if (P->w != NULL && P->rate == rate && P->ch == ch)
	{
		SndUnlock(P);
		return true;
	}
	if (P->w != NULL)
	{
		waveOutReset(P->w);
		for (int i = 0; i < RC_AUD_BUFS; ++i)
		{
			if (P->hdr[i].lpData != NULL)
			{
				waveOutUnprepareHeader(P->w, &P->hdr[i], sizeof(WAVEHDR));
				P->hdr[i].lpData = NULL;
			}
		}
		waveOutClose(P->w);
		P->w = NULL;
	}

	WAVEFORMATEX wf;
	memset(&wf, 0, sizeof(wf));
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = (WORD)ch;
	wf.nSamplesPerSec = (DWORD)rate;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
	wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

	if (waveOutOpen(&P->w, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
	{
		P->w = NULL;
		SndUnlock(P);
		ConPrint("  打开音频播放设备失败(%s)", P->label);
		return false;
	}
	for (int i = 0; i < RC_AUD_BUFS; ++i)
	{
		memset(&P->hdr[i], 0, sizeof(WAVEHDR));
		P->hdr[i].lpData = NULL;
	}
	P->rate = rate;
	P->ch = ch;
	_snprintf_s(P->fmt, _TRUNCATE, "%d Hz / %d ch", rate, ch);
	SndApplyVolume(P);
	SndUnlock(P);
	return true;
}

/* 播放一块 PCM(16bit) */
static void SndPlay(SndOut* P, const char* pcm, int bytes, int rate, int ch)
{
	if (pcm == NULL || bytes <= 0) return;
	if (rate <= 0 || ch <= 0) return;
	if (!SndOpen(P, rate, ch)) return;

	SndLock(P);
	HWAVEOUT w = P->w;
	if (w == NULL) { SndUnlock(P); return; }

	for (int i = 0; i < RC_AUD_BUFS; ++i)
	{
		bool free_ = false;
		if (P->hdr[i].lpData == NULL) free_ = true;
		else if (P->hdr[i].dwFlags & WHDR_DONE)
		{
			waveOutUnprepareHeader(w, &P->hdr[i], sizeof(WAVEHDR));
			P->hdr[i].lpData = NULL;
			free_ = true;
		}
		if (!free_) continue;

		P->buf[i].assign(pcm, pcm + bytes);
		memset(&P->hdr[i], 0, sizeof(WAVEHDR));
		P->hdr[i].lpData = P->buf[i].data();
		P->hdr[i].dwBufferLength = (DWORD)bytes;
		if (waveOutPrepareHeader(w, &P->hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
			waveOutWrite(w, &P->hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
		{
			InterlockedIncrement(&P->frames);
			InterlockedExchangeAdd(&P->bytes, bytes);
		}
		else
		{
			InterlockedIncrement(&P->lost);
		}
		SndUnlock(P);
		return;
	}
	InterlockedIncrement(&P->lost);   /* 12 块都占用: 丢弃 */
	SndUnlock(P);
}
/* 发送音频控制请求 */
static bool SndReq(SndOut* P, int op, int bufms)
{
	if (g_sock == INVALID_SOCKET)
	{
		ConPrint("  未连接, 无法操作%s", P->label);
		return false;
	}
	AudioReq req;
	memset(&req, 0, sizeof(req));
	req.op = op;
	req.buf_ms = (bufms > 0) ? bufms : P->bufms;
	req.dev = P->dev;
	req.gain = P->gain;
	return SendCmdPacket(P->cmd, (const char*)&req, (int)sizeof(req));
}

static void SndStart(SndOut* P, int bufms)
{
	if (bufms >= 10 && bufms <= 500) P->bufms = bufms;
	if (!SndReq(P, RC_AUD_START, P->bufms)) return;
	InterlockedExchange(&P->frames, 0);
	InterlockedExchange(&P->lost, 0);
	InterlockedExchange(&P->bytes, 0);
	InterlockedExchange(&P->on, 1);
	ConPrint("  已请求被控端回传%s (每包 %d ms, 设备 %s, 增益 %d%%)",
		P->label, P->bufms, P->dev > 0 ? "指定" : "默认", P->gain);
	if (P->dev > 0)
	{
		char b[96];
		_snprintf_s(b, _TRUNCATE, "  (设备序号 %d, 可用 '%s list' 查看)", P->dev, P->name);
		ConPush(b);
	}
}

/* 目标是否本机(本机回环时自动开声音会导致扬声器->采集->播放死循环啸叫) */
static bool ExtIsLoopbackTarget()
{
	if (strncmp(g_ip, "127.", 4) == 0 || strcmp(g_ip, "localhost") == 0) return true;
	char host[256] = { 0 };
	if (gethostname(host, sizeof(host)) != 0) return false;
	struct hostent* he = gethostbyname(host);
	if (he == NULL) return false;
	for (int i = 0; he->h_addr_list[i] != NULL; ++i)
	{
		struct in_addr a; memcpy(&a, he->h_addr_list[i], 4);
		if (strcmp(inet_ntoa(a), g_ip) == 0) return true;
	}
	return false;
}

static void SndStop(SndOut* P)
{
	if (g_sock != INVALID_SOCKET) SndReq(P, RC_AUD_STOP, 0);
	InterlockedExchange(&P->on, 0);
	SndClose(P);
	ConPrint("  已停止%s回传", P->label);
}

static void SndStat(SndOut* P)
{
	char buf[360];
	_snprintf_s(buf, _TRUNCATE,
		"  %s: %s   格式 %s   帧 %d   丢弃 %d   接收 %d KB   音量 %d%%   采集增益 %d%%   设备 %d",
		P->label,
		InterlockedCompareExchange(&P->on, 1, 1) ? "回传中" : "已停止",
		P->fmt,
		(int)InterlockedCompareExchange(&P->frames, 0, 0),
		(int)InterlockedCompareExchange(&P->lost, 0, 0),
		(int)InterlockedCompareExchange(&P->bytes, 0, 0) / 1024,
		P->vol, P->gain, P->dev);
	ConPush(buf);
}

static void SndSetVolume(SndOut* P, int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	P->vol = v;
	SndApplyVolume(P);
	char buf[96];
	_snprintf_s(buf, _TRUNCATE, "  %s播放音量: %d%%", P->label, v);
	ConPush(buf);
}

static void SndList(SndOut* P)
{
	if (SndReq(P, RC_AUD_LIST, 0))
		ConPrint("  已请求被控端%s设备列表(回复打印在下方)", P->label);
}

static void SndSetDev(SndOut* P, int dev)
{
	if (dev < 0) dev = 0;
	P->dev = dev;
	char buf[128];
	_snprintf_s(buf, _TRUNCATE, "  %s使用设备: %s (下次 start 生效, 0=系统默认)", P->label,
		dev == 0 ? "默认" : "列表序号");
	ConPush(buf);
}

static void SndSetGain(SndOut* P, int g)
{
	if (g < 10) g = 10;
	if (g > 800) g = 800;
	P->gain = g;
	char buf[128];
	_snprintf_s(buf, _TRUNCATE, "  %s采集增益: %d%% (下次 start 生效; 麦克风太小声可调大)", P->label, g);
	ConPush(buf);
}

/* 收包入口: 系统声音 */
static void AudioPlayFrame(const char* pcm, int bytes, int rate, int ch) { SndPlay(SndSys(), pcm, bytes, rate, ch); }
static void MicPlayFrame(const char* pcm, int bytes, int rate, int ch)   { SndPlay(SndMic(), pcm, bytes, rate, ch); }

/* ---------------- 控制台缓冲 ---------------- */
static void ConPush(const std::string& s)
{
	EnterCriticalSection(&g_state_lock);
	g_con_lines.push_back(s);
	while (g_con_lines.size() > 1000) g_con_lines.pop_front();
	LeaveCriticalSection(&g_state_lock);
	if (g_hwnd != NULL)
		PostMessageA(g_hwnd, WM_APP_CONSOLE, 0, 0);
}
static void ConPrint(const char* fmt, ...)
{
	char msg[4000];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(msg, _TRUNCATE, fmt, ap);
	va_end(ap);
	ConPush(msg);
}
static void ConAllToString(std::string& all)
{
	EnterCriticalSection(&g_state_lock);
	for (size_t i = 0; i < g_con_lines.size(); ++i) { all += g_con_lines[i]; all += "\r\n"; }
	LeaveCriticalSection(&g_state_lock);
}
static void ConClear()
{
	EnterCriticalSection(&g_state_lock);
	g_con_lines.clear();
	LeaveCriticalSection(&g_state_lock);
	if (g_hwnd != NULL) PostMessageA(g_hwnd, WM_APP_CONSOLE, 0, 0);
}

/* ---------------- 截图: 保存当前远程画面 ---------------- */
static int ExtGetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
	UINT num = 0, size = 0;
	int found = -1;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;
	Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
	if (info == NULL) return -1;
	Gdiplus::GetImageEncoders(num, size, info);
	for (UINT i = 0; i < num; ++i)
	{
		if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; found = (int)i; break; }
	}
	free(info);
	return found;
}
/* path 为空 → 程序目录 rc_shot_年月日_时分秒.jpg */
static std::string ExtSaveShot(const std::string& pathIn)
{
	std::string path = ExtTrim(pathIn);
	if (path.empty())
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		char name[160];
		_snprintf_s(name, _TRUNCATE, "\\rc_shot_%04d%02d%02d_%02d%02d%02d.jpg",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		path = ExtExeDir() + name;
	}
	bool png = false;
	size_t dot = path.find_last_of('.');
	if (dot != std::string::npos && ExtLower(path.substr(dot + 1)) == "png")
		png = true;

	EnterCriticalSection(&g_state_lock);
	bool ok = false;
	if (!g_image.IsNull() && g_image.GetWidth() > 0)
	{
		HBITMAP hb = (HBITMAP)g_image;
		Gdiplus::Bitmap bmp(hb, NULL);
		CLSID clsid;
		if (ExtGetEncoderClsid(png ? L"image/png" : L"image/jpeg", &clsid) >= 0)
		{
			std::wstring wp = ExtA2W(path);
			ok = (bmp.Save(wp.c_str(), &clsid, NULL) == Gdiplus::Ok);
		}
	}
	LeaveCriticalSection(&g_state_lock);
	if (!ok) { ConPrint("  截图失败: 还没有画面(请先连接被控端并等待第一帧)"); return std::string(); }
	ConPrint("  已保存截图: %s", path.c_str());
	LogLine("截图已保存: %s", path.c_str());
	return path;
}

/* ---------------- 按键名 → VK ---------------- */
struct ExtKeyMap { const char* name; int vk; };
static const ExtKeyMap kKeys[] =
{
	{ "back", VK_BACK }, { "backspace", VK_BACK }, { "tab", VK_TAB }, { "enter", VK_RETURN },
	{ "return", VK_RETURN }, { "shift", VK_SHIFT }, { "ctrl", VK_CONTROL }, { "control", VK_CONTROL },
	{ "alt", VK_MENU }, { "menu", VK_MENU }, { "pause", VK_PAUSE }, { "caps", VK_CAPITAL },
	{ "esc", VK_ESCAPE }, { "escape", VK_ESCAPE }, { "space", VK_SPACE }, { "pageup", VK_PRIOR },
	{ "pagedown", VK_NEXT }, { "end", VK_END }, { "home", VK_HOME }, { "left", VK_LEFT },
	{ "up", VK_UP }, { "right", VK_RIGHT }, { "down", VK_DOWN }, { "insert", VK_INSERT },
	{ "delete", VK_DELETE }, { "del", VK_DELETE }, { "win", VK_LWIN }, { "lwin", VK_LWIN },
	{ "num0", VK_NUMPAD0 }, { "num1", VK_NUMPAD1 }, { "num2", VK_NUMPAD2 }, { "num3", VK_NUMPAD3 },
	{ "num4", VK_NUMPAD4 }, { "num5", VK_NUMPAD5 }, { "num6", VK_NUMPAD6 }, { "num7", VK_NUMPAD7 },
	{ "num8", VK_NUMPAD8 }, { "num9", VK_NUMPAD9 }
};
static int ExtVkFromName(const std::string& raw)
{
	std::string n = ExtLower(raw);
	if (n.size() >= 2 && n[0] == 'f')
	{
		int idx = atoi(n.c_str() + 1);
		if (idx >= 1 && idx <= 12) return VK_F1 + idx - 1;
	}
	if (n.size() == 1)
	{
		char c = n[0];
		if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 'A');
		if (c >= '0' && c <= '9') return (int)c;
	}
	for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i)
		if (n == kKeys[i].name) return kKeys[i].vk;
	if (n.compare(0, 2, "0x") == 0)
	{
		int v = (int)strtol(n.c_str() + 2, NULL, 16);
		if (v > 0 && v < 256) return v;
	}
	return 0;
}
static void ExtSendKey(int vk, bool up)
{
	KeyBoard kb;
	memset(&kb, 0, sizeof(kb));
	kb.virtual_code = vk;
	kb.key_status = up ? RC_KEY_UP : RC_KEY_DOWN;
	SendCmdPacket(CMD_KEYBOARD, (char*)&kb, sizeof(kb));
}
/* ASCII 文本 → 逐字符按键(中文请用 clip set + 远端 Ctrl+V) */
static int ExtSendTextAscii(const std::string& text, bool& hasNonAscii)
{
	int sent = 0;
	hasNonAscii = false;
	for (size_t i = 0; i < text.size(); ++i)
	{
		unsigned char c = (unsigned char)text[i];
		if (c >= 0x80) { hasNonAscii = true; continue; }
		short vk = VkKeyScanA((char)c);
		if (vk == -1) continue;
		int code = vk & 0xFF;
		bool shift = (vk & 0x100) != 0;
		if (shift) ExtSendKey(VK_SHIFT, false);
		ExtSendKey(code, false);
		ExtSendKey(code, true);
		if (shift) ExtSendKey(VK_SHIFT, true);
		sent++;
	}
	return sent;
}

/* ---------------- 键鼠录制: 接收/保存/回放 ---------------- */
static void ExtRecAppend(const std::string& block, bool last)
{
	g_rec_text += block;
	if (last)
	{
		ConPrint("  录制数据接收完毕(%d 字节)", (int)g_rec_text.size());
		HWND ed = (g_hwnd != NULL) ? GetDlgItem(g_hwnd, IDC_EDIT_RECOUT) : NULL;
		if (ed != NULL)
		{
			SetWindowTextA(ed, g_rec_text.c_str());
			SendMessageA(ed, EM_SETSEL, (WPARAM)-1, -1);
			SendMessageA(ed, EM_SCROLLCARET, 0, 0);
		}
		HWND st = (g_hwnd != NULL) ? GetDlgItem(g_hwnd, IDC_REC_STATUS) : NULL;
		if (st != NULL)
		{
			char buf[128];
			_snprintf_s(buf, _TRUNCATE, "状态: 已完成 (%d 字节)", (int)g_rec_text.size());
			SetWindowTextA(st, buf);
		}
		LogLine("键鼠录制完成: %d 字节", (int)g_rec_text.size());
		g_rec_expect = false;
	}
}
static void ExtRecSaveToFile(const std::string& pathIn)
{
	std::string path = ExtTrim(pathIn);
	if (g_rec_text.empty()) { ConPrint("  还没有录制数据可保存"); return; }
	if (path.empty())
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		char name[160];
		_snprintf_s(name, _TRUNCATE, "\\rc_rec_%04d%02d%02d_%02d%02d%02d.txt",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		path = ExtExeDir() + name;
	}
	FILE* fp = NULL;
	if (fopen_s(&fp, path.c_str(), "wb") != 0 || fp == NULL) { ConPrint("  保存失败: %s", path.c_str()); return; }
	fwrite(g_rec_text.data(), 1, g_rec_text.size(), fp);
	fclose(fp);
	ConPrint("  录制已保存: %s (%d 字节)", path.c_str(), (int)g_rec_text.size());
}

struct ExtReplayCtx { std::string text; };

DWORD WINAPI ExtReplayThread(LPVOID param)
{
	ExtReplayCtx* ctx = (ExtReplayCtx*)param;
	std::string text = ctx->text;
	delete ctx;
	if (!g_connected) { ConPrint("  回放失败: 未连接"); return 0; }

	struct Ev { unsigned int t; int kind; int x; int y; int btn; int vk; int down; };
	std::vector<Ev> evs;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string ln = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
		pos = (nl == std::string::npos) ? text.size() : nl + 1;
		ln = ExtTrim(ln);
		if (ln.size() < 2 || ln[0] != 't' || ln[1] != '=') continue;
		std::vector<std::string> f;
		size_t p2 = 0;
		while (p2 <= ln.size())
		{
			size_t tb = ln.find('\t', p2);
			f.push_back(ExtTrim((tb == std::string::npos) ? ln.substr(p2) : ln.substr(p2, tb - p2)));
			if (tb == std::string::npos) break;
			p2 = tb + 1;
		}
		if (f.size() < 3) continue;
		Ev e;
		memset(&e, 0, sizeof(e));
		e.t = (unsigned int)strtoul(f[0].c_str() + 2, NULL, 10);
		if (f[1] == "mouse" && f.size() >= 5 && f[2] == "move")
		{
			e.kind = 0; e.x = atoi(f[3].c_str()); e.y = atoi(f[4].c_str());
			evs.push_back(e);
		}
		else if (f[1] == "mouse" && f.size() >= 6)
		{
			e.kind = 1;
			e.btn = (f[2] == "left") ? 0 : ((f[2] == "right") ? 1 : 2);
			e.down = (f[3] == "down") ? 1 : 0;
			e.x = atoi(f[4].c_str()); e.y = atoi(f[5].c_str());
			evs.push_back(e);
		}
		else if (f[1] == "key" && f.size() >= 4)
		{
			e.kind = 2;
			e.vk = (int)strtol(f[2].c_str(), NULL, 16);
			e.down = (f[3] == "down") ? 1 : 0;
			evs.push_back(e);
		}
	}
	if (evs.empty()) { ConPrint("  回放失败: 没有可解析的事件"); return 0; }

	ConPrint("  回放开始: %d 个事件(可按 停止录制 取消)", (int)evs.size());
	unsigned int prev = 0;
	int done = 0;
	for (size_t i = 0; i < evs.size(); ++i)
	{
		if (!g_connected) break;
		unsigned int dt = (evs[i].t > prev) ? (evs[i].t - prev) : 0;
		prev = evs[i].t;
		if (dt > 0) Sleep(dt > 3000 ? 3000 : dt);
		if (evs[i].kind == 0)
		{
			Mouse m; memset(&m, 0, sizeof(m));
			m.action = MOUSE_MOVE; m.ptXY.x = evs[i].x; m.ptXY.y = evs[i].y;
			SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
		}
		else if (evs[i].kind == 1)
		{
			Mouse m; memset(&m, 0, sizeof(m));
			m.action = MOUSE_MOVE; m.ptXY.x = evs[i].x; m.ptXY.y = evs[i].y;
			SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
			memset(&m, 0, sizeof(m));
			if (evs[i].btn == 0) m.action = evs[i].down ? MOUSE_LDOWN : MOUSE_LUP;
			else if (evs[i].btn == 1) m.action = evs[i].down ? MOUSE_RDOWN : MOUSE_RUP;
			else m.action = evs[i].down ? MOUSE_MDOWN : MOUSE_MUP;
			m.ptXY.x = evs[i].x; m.ptXY.y = evs[i].y;
			SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
		}
		else
		{
			ExtSendKey(evs[i].vk, evs[i].down == 0);
		}
		done++;
	}
	ConPrint("  回放结束: 已发送 %d 个事件", done);
	return 0;
}

/* ---------------- 命令行控制台: 执行 ---------------- */
static const char* kHelp[] =
{
	"==== RemoteControl 命令行帮助 ====",
	"  help | ?                     显示本帮助",
	"  clear                        清空控制台输出",
	"  ping                         测试连通与延迟",
	"  stat                         显示连接/帧率/码率/被控端目录",
	"  shot [文件路径]              截图(控制端当前画面), 默认程序目录 rc_shot_*.jpg",
	"  locate                       定位: 查询被控端鼠标坐标并在画面上打红色十字",
	"  sysinfo                      被控端系统信息(分辨率/内存/CPU/运行时长/沙箱)",
	"  clip get | clip set <文本> | clip clear     读/写/清空被控端剪贴板",
	"  text <文本>                  向被控端逐字发送(仅 ASCII)",
	"  type <文本>                  输入文本(含中文): 写被控端剪贴板 + 自动 Ctrl+V",
	"  [键盘] 鼠标移到远程画面上即可直接打字(画面窗口自动接管键盘焦点)",
	"  mouse <x> <y>                移动被控端鼠标到坐标",
	"  click <x> <y> [l|r|d]        移动并点击(默认左键, d=双击)",
	"  key <名称> [down|up]         按键: a-z 0-9 enter esc tab space shift ctrl alt up down f1-f12 0x41",
	"  ls [路径]                    列出被控端目录(缺省=当前目录)",
	"  cd <路径>                    切换被控端当前目录",
	"  get <远端文件> [本机路径]    下载",
	"  put <本机文件> [远端路径]    上传",
	"  mkdir <名称> | rm <路径> | rename <旧> <新>",
	"  screen fps <n> | q <n> | enc <jpeg|png>     调整画面参数",
	"  rec <秒> [m] [k]             录制键鼠(默认鼠标+键盘)",
	"  recstop | recsave [文件] | recplay | recread",
	"  audio start [包长ms] | stop | vol <0-100> | stat    系统声音回传(听被控端的声音)",
	"  cam [on|off|stat|res <宽x高>|fps <n>|q <n>]    被控端摄像头: 独立小窗口实时显示(默认640x360/15fps)",
	"  autostart [on|off|status]     被控端开机自启(计划任务/最高权限, 不弹UAC; 无参数=查询)",
	"  mic start [包长ms] | stop | stat | list | dev <n> | gain <10-800> | vol <0-100>   麦克风(环境声)回传",
	"  lrec | lrecstop | lrecsave [文件] | lrecload [文件] | lplay [倍速]   控制端本地录制/回放(录控制端自己的鼠标键盘)",
	"  dev | dev list               设备列表: 历史设备 + 在线状态(自动保存连接过的设备)",
	"  dev sniff [秒] | dev scan [网段] | dev connect <序号|ip[:port]> | dev add/del/probe   设备发现/管理",
	"=================================="
};
static void ExtReqListPath(const std::string& path)
{
	SendText(CMD_FILE_LIST, path);
	if (!path.empty())
	{
		strncpy_s(g_cur_path, path.c_str(), _TRUNCATE);
		HWND ed = (g_hwnd != NULL) ? GetDlgItem(g_hwnd, IDC_EDIT_PATH) : NULL;
		if (ed != NULL) SetWindowTextA(ed, path.c_str());
	}
}
static void ExtStartDownload(const std::string& remote, const std::string& localIn)
{
	std::string local = ExtTrim(localIn);
	if (local.empty())
	{
		const char* base = remote.c_str();
		const char* s1 = strrchr(remote.c_str(), '\\');
		const char* s2 = strrchr(remote.c_str(), '/');
		if (s1 != NULL && s1 + 1 > base) base = s1 + 1;
		if (s2 != NULL && s2 + 1 > base) base = s2 + 1;
		local = std::string(base);
	}
	FILE* fp = NULL;
	if (fopen_s(&fp, local.c_str(), "wb") != 0 || fp == NULL)
	{
		ConPrint("  无法创建本地文件: %s", local.c_str());
		return;
	}
	if (g_down_fp != NULL) { fclose(g_down_fp); g_down_fp = NULL; }
	g_down_fp = fp;
	g_down_id = (unsigned int)InterlockedIncrement(&g_id_counter);
	g_down_bytes = 0;
	g_down_dir = local;
	InterlockedExchange(&g_down_active, 1);

	FileRequest req;
	memset(&req, 0, sizeof(req));
	req.file_id = g_down_id;
	req.offset = 0;
	strncpy_s(req.path, remote.c_str(), _TRUNCATE);
	SendCmdPacket(CMD_FILE_DOWNLOAD, (char*)&req, sizeof(req));
	ConPrint("  开始下载 %s -> %s", remote.c_str(), local.c_str());
}
static void ExtStartUpload(const std::string& local, const std::string& remoteIn)
{
	std::string remote = ExtTrim(remoteIn);
	if (remote.empty())
	{
		const char* base = local.c_str();
		const char* s1 = strrchr(local.c_str(), '\\');
		const char* s2 = strrchr(local.c_str(), '/');
		if (s1 != NULL && s1 + 1 > base) base = s1 + 1;
		if (s2 != NULL && s2 + 1 > base) base = s2 + 1;
		remote = JoinPath(std::string(g_cur_path), std::string(base));
	}
	UploadJob* job = new UploadJob();
	job->file_id = (unsigned int)InterlockedIncrement(&g_id_counter);
	job->local = local;
	job->remote = remote;
	DWORD tid = 0;
	HANDLE h = CreateThread(NULL, 0, UploadThread, job, 0, &tid);
	if (h != NULL) CloseHandle(h);
	else { delete job; ConPrint("  创建上传线程失败"); }
}
static void ExtStartRecord(int seconds, int flags)
{
	if (!g_connected) { ConPrint("  录制失败: 未连接"); return; }
	if (seconds < 1) seconds = 1;
	if (seconds > 3600) seconds = 3600;
	InputRecReq req;
	memset(&req, 0, sizeof(req));
	req.flags = flags;
	req.duration_ms = seconds * 1000;
	req.sample_ms = 15;
	req.stop = 0;
	g_rec_text.clear();
	g_rec_expect = true;
	SendCmdPacket(CMD_INPUT_REC, (char*)&req, sizeof(req));
	ConPrint("  开始录制 %d 秒 (鼠标%s 键盘%s) ... 期间请在被控端正常操作", seconds,
		(flags & RC_REC_MOUSE) ? "√" : "×", (flags & RC_REC_KEY) ? "√" : "×");
	HWND st = (g_hwnd != NULL) ? GetDlgItem(g_hwnd, IDC_REC_STATUS) : NULL;
	if (st != NULL) SetWindowTextA(st, "状态: 录制中 ...");
}
static void ExtStopRecord()
{
	InputRecReq req;
	memset(&req, 0, sizeof(req));
	req.stop = 1;
	SendCmdPacket(CMD_INPUT_REC, (char*)&req, sizeof(req));
	ConPrint("  已请求停止录制(等被控端回传数据)");
}
static void ExtReplayStart()
{
	if (g_rec_text.empty()) { ConPrint("  还没有录制数据(先 rec 10 录制一段)"); return; }
	ExtReplayCtx* ctx = new ExtReplayCtx();
	ctx->text = g_rec_text;
	DWORD tid = 0;
	HANDLE h = CreateThread(NULL, 0, ExtReplayThread, ctx, 0, &tid);
	if (h != NULL) CloseHandle(h);
	else { delete ctx; ConPrint("  创建回放线程失败"); }
}

/* ============================================================
 *  设备列表 (历史设备 / 在线探测 / 局域网嗅探)
 *  - 历史设备持久化: %APPDATA%\RemoteControl\devices.txt
 *    行格式: ip|port|last_connect_unix|备注名
 *  - 在线探测: TCP 连接被控端端口(非阻塞 + select 精确超时)
 *  - 局域网嗅探: UDP 广播 RC_DISCOVER_V1 -> 被控端单播回 DiscReply
 *  - 网段扫描: 并发 TCP 扫描 x.x.x.1-254
 * ============================================================ */
#include <time.h>
struct DeviceInfo
{
	std::string ip;
	int port;
	std::string name;         /* 备注名 / 被控端主机名 */
	unsigned long long last;  /* 最后成功连接(Unix 秒) */
	int online;               /* -1 未知 0 离线 1 在线 */
	int rtt;                  /* 探测延迟 ms */
	DeviceInfo() : port(9999), last(0), online(-1), rtt(-1) {}
};

static std::vector<DeviceInfo> g_devs;
static CRITICAL_SECTION g_dev_lock;
static volatile LONG g_dev_lock_ready = 0;
static volatile LONG g_dev_headless = 0;     /* --dev 无头模式 */
static volatile LONG g_sweep_next = 0;
static volatile LONG g_sweep_found = 0;
static std::string   g_sweep_prefix;
static int           g_sweep_port = 9999;
static int           g_sweep_to = 200;

static void DevEnsureLock()
{
	if (InterlockedCompareExchange(&g_dev_lock_ready, 1, 0) == 0)
		InitializeCriticalSection(&g_dev_lock);
}

/* 输出: GUI 走控制台, 无头模式走 stdout */
static void DevOut(const char* fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
	va_end(ap);
	if (g_dev_headless)
	{
		EnterCriticalSection(&g_dev_lock);
		printf("%s\n", msg);
		fflush(stdout);
		LeaveCriticalSection(&g_dev_lock);
	}
	else
		ConPrint("%s", msg);
}

static std::string DevStorePath()
{
	char buf[MAX_PATH] = { 0 };
	DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
	std::string dir = (n > 0 && n < MAX_PATH) ? std::string(buf) : ExtExeDir();
	if (!dir.empty() && dir.back() == '\\') dir.pop_back();
	dir += "\\RemoteControl";
	CreateDirectoryA(dir.c_str(), NULL);
	return dir + "\\devices.txt";
}

/* 需持有 g_dev_lock */
static void DevSaveRaw()
{
	FILE* fp = NULL;
	if (fopen_s(&fp, DevStorePath().c_str(), "wb") != 0 || fp == NULL) return;
	for (size_t i = 0; i < g_devs.size(); ++i)
		fprintf(fp, "%s|%d|%llu|%s\n", g_devs[i].ip.c_str(), g_devs[i].port,
			(unsigned long long)g_devs[i].last, g_devs[i].name.c_str());
	fclose(fp);
}

static void DevLoad()
{
	DevEnsureLock();
	EnterCriticalSection(&g_dev_lock);
	g_devs.clear();
	FILE* fp = NULL;
	if (fopen_s(&fp, DevStorePath().c_str(), "rb") == 0 && fp != NULL)
	{
		char line[512];
		while (fgets(line, sizeof(line), fp) != NULL)
		{
			std::string s = ExtTrim(line);
			if (s.empty() || s[0] == '#') continue;
			size_t p1 = s.find('|');
			if (p1 == std::string::npos) continue;
			size_t p2 = s.find('|', p1 + 1);
			size_t p3 = (p2 == std::string::npos) ? std::string::npos : s.find('|', p2 + 1);
			DeviceInfo d;
			d.ip = ExtTrim(s.substr(0, p1));
			std::string ps = s.substr(p1 + 1, (p2 == std::string::npos ? s.size() : p2) - p1 - 1);
			d.port = atoi(ps.c_str());
			if (d.port <= 0 || d.port > 65535) d.port = 9999;
			if (p2 != std::string::npos)
			{
				size_t le = (p3 == std::string::npos) ? s.size() : p3;
				d.last = _strtoui64(s.substr(p2 + 1, le - p2 - 1).c_str(), NULL, 10);
			}
			if (p3 != std::string::npos) d.name = ExtTrim(s.substr(p3 + 1));
			if (!d.ip.empty()) g_devs.push_back(d);
		}
		fclose(fp);
	}
	LeaveCriticalSection(&g_dev_lock);
}

/* 需持有 g_dev_lock */
static int DevFindIndex(const std::string& ip, int port)
{
	for (size_t i = 0; i < g_devs.size(); ++i)
		if (g_devs[i].ip == ip && g_devs[i].port == port) return (int)i;
	return -1;
}

/* 发现/新增设备(线程安全)。proto>0 表示来自嗅探回包(记录主机名) */
static void DevAddOrUpdate(const std::string& ip, int port, const std::string& name)
{
	DevEnsureLock();
	EnterCriticalSection(&g_dev_lock);
	int idx = DevFindIndex(ip, port);
	if (idx < 0)
	{
		DeviceInfo d;
		d.ip = ip; d.port = port; d.name = name; d.online = 1; d.rtt = 0;
		g_devs.push_back(d);
	}
	else
	{
		g_devs[idx].online = 1;
		if (!name.empty()) g_devs[idx].name = name;
	}
	DevSaveRaw();
	LeaveCriticalSection(&g_dev_lock);
}

/* 连接成功时更新最后连接时间 */
static void DevTouch(const std::string& ip, int port)
{
	DevEnsureLock();
	EnterCriticalSection(&g_dev_lock);
	int idx = DevFindIndex(ip, port);
	if (idx < 0)
	{
		DeviceInfo d; d.ip = ip; d.port = port; d.online = 1;
		g_devs.push_back(d);
		idx = (int)g_devs.size() - 1;
	}
	g_devs[idx].last = (unsigned long long)time(NULL);
	g_devs[idx].online = 1;
	DevSaveRaw();
	LeaveCriticalSection(&g_dev_lock);
}

/* TCP 在线探测: 返回延迟 ms, 失败 -1 */
static int DevProbe(const std::string& ip, int port, int timeout_ms)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return -1;
	SOCKADDR_IN a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.S_un.S_addr = inet_addr(ip.c_str());
	if (a.sin_addr.S_un.S_addr == INADDR_NONE) { closesocket(s); return -1; }

	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	ULONGLONG t0 = GetTickCount64();
	int ok = -1;
	if (connect(s, (sockaddr*)&a, sizeof(a)) == 0)
		ok = 0;
	else if (WSAGetLastError() == WSAEWOULDBLOCK)
	{
		fd_set wf, ef;
		FD_ZERO(&wf); FD_ZERO(&ef);
		FD_SET(s, &wf); FD_SET(s, &ef);
		timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		if (select(0, NULL, &wf, &ef, &tv) > 0)
		{
			int err = 0, elen = sizeof(err);
			getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
			if (err == 0) ok = 0;
		}
	}
	int ms = (int)(GetTickCount64() - t0);
	closesocket(s);
	return (ok == 0) ? ms : -1;
}

/* 解析 "ip[:port]" / "序号" -> ip, port。 返回 false 表示解析失败 */
static bool DevResolveTarget(const std::vector<std::string>& a, size_t idx, std::string& ip, int& port)
{
	port = 9999;
	if (idx >= a.size() || a[idx].empty()) return false;
	std::string spec = ExtTrim(a[idx]);
	bool numeric = true;
	for (size_t i = 0; i < spec.size(); ++i)
		if (spec[i] < '0' || spec[i] > '9') { numeric = false; break; }
	if (numeric)   /* 纯数字 = 列表序号 */
	{
		int n = atoi(spec.c_str());
		EnterCriticalSection(&g_dev_lock);
		bool ok = (n >= 1 && n <= (int)g_devs.size());
		if (ok) { ip = g_devs[n - 1].ip; port = g_devs[n - 1].port; }
		LeaveCriticalSection(&g_dev_lock);
		return ok;
	}
	ip = spec;
	size_t c = spec.find(':');
	if (c != std::string::npos)
	{
		ip = spec.substr(0, c);
		int p = atoi(spec.substr(c + 1).c_str());
		if (p > 0 && p < 65536) port = p;
	}
	return !ip.empty();
}

/* 打印设备表(results 为探测后的延迟, -2 表示未探测) */
static void DevPrintList(bool probe)
{
	DevEnsureLock();
	std::vector<DeviceInfo> list;
	EnterCriticalSection(&g_dev_lock);
	list = g_devs;
	LeaveCriticalSection(&g_dev_lock);

	if (probe)
	{
		for (size_t i = 0; i < list.size(); ++i)
		{
			list[i].rtt = DevProbe(list[i].ip, list[i].port, 400);
			list[i].online = (list[i].rtt >= 0) ? 1 : 0;
			EnterCriticalSection(&g_dev_lock);
			int idx = DevFindIndex(list[i].ip, list[i].port);
			if (idx >= 0) { g_devs[idx].rtt = list[i].rtt; g_devs[idx].online = list[i].online; }
			LeaveCriticalSection(&g_dev_lock);
		}
	}

	DevOut("");
	if (list.empty())
	{
		DevOut("  (设备列表为空) 可用 dev add <ip[:port]> [名称] 添加, 或 dev sniff 自动发现局域网被控端");
		return;
	}
	DevOut("  序号 设备地址                 状态      延迟     最后连接             备注");
	DevOut("  --------------------------------------------------------------------------------");
	for (size_t i = 0; i < list.size(); ++i)
	{
		std::string st;
		if (list[i].rtt >= 0) st = "在线";
		else if (list[i].online == 0) st = "离线";
		else if (list[i].online == 1) st = probe ? "离线" : "在线";
		else st = "未知";
		char rttb[24] = "-";
		if (list[i].rtt >= 0) sprintf_s(rttb, "%dms", list[i].rtt);
		char lastb[40] = "-";
		if (list[i].last != 0)
		{
			time_t t = (time_t)list[i].last;
			struct tm tmv;
			if (localtime_s(&tmv, &t) == 0)
				strftime(lastb, sizeof(lastb), "%Y-%m-%d %H:%M:%S", &tmv);
		}
		bool cur = (g_connected != 0) && (strcmp(g_ip, list[i].ip.c_str()) == 0) && (g_port == list[i].port);
		char addr[96];
		sprintf_s(addr, "%s:%d", list[i].ip.c_str(), list[i].port);
		DevOut("  %-4d %-22s %-8s %-8s %-20s %s%s", (int)i + 1,
			addr, st.c_str(), rttb, lastb, list[i].name.c_str(), cur ? "  <== 当前连接" : "");
	}
}

/* 本机所在网段前缀(如 192.168.1), 取不到返回空 */
static std::vector<std::string> DevLocalPrefixes()
{
	std::vector<std::string> out;
	char host[256] = { 0 };
	if (gethostname(host, sizeof(host) - 1) != 0) return out;
	struct hostent* he = gethostbyname(host);
	if (he == NULL) return out;
	for (int i = 0; he->h_addr_list[i] != NULL; ++i)
	{
		struct in_addr in;
		memcpy(&in, he->h_addr_list[i], 4);
		std::string ip = inet_ntoa(in);
		if (ip.rfind("127.", 0) == 0) continue;
		size_t p = ip.rfind('.');
		if (p == std::string::npos) continue;
		std::string pre = ip.substr(0, p);
		bool dup = false;
		for (size_t k = 0; k < out.size(); ++k) if (out[k] == pre) { dup = true; break; }
		if (!dup) out.push_back(pre);
	}
	return out;
}

/* UDP 广播嗅探: 返回发现数量 */
static int DevUdpDiscover(int wait_ms)
{
	int found = 0;
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) return 0;
	BOOL on = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);

	SOCKADDR_IN d;
	memset(&d, 0, sizeof(d));
	d.sin_family = AF_INET;
	d.sin_port = htons(RC_DISC_PORT);
	d.sin_addr.S_un.S_addr = INADDR_BROADCAST;
	const char* req = RC_DISC_REQ;
	sendto(s, req, (int)strlen(req), 0, (sockaddr*)&d, sizeof(d));

	ULONGLONG t0 = GetTickCount64();
	while ((int)(GetTickCount64() - t0) < wait_ms)
	{
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(s, &rf);
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 150000;
		if (select(0, &rf, NULL, NULL, &tv) > 0)
		{
			DiscReply r;
			memset(&r, 0, sizeof(r));
			SOCKADDR_IN from;
			int fl = sizeof(from);
			int n = recvfrom(s, (char*)&r, sizeof(r), 0, (sockaddr*)&from, &fl);
			if (n >= (int)sizeof(DiscReply) && r.magic == RC_DISC_MAGIC)
			{
				std::string ip = inet_ntoa(from.sin_addr);
				int port = r.port ? (int)r.port : 9999;
				std::string host = r.name;
				DevOut("  [嗅探] 发现被控端 %s:%d  主机名=%s  协议=v%d", ip.c_str(), port, host.c_str(), r.proto);
				DevAddOrUpdate(ip, port, host);
				++found;
			}
		}
	}
	closesocket(s);
	return found;
}

static DWORD WINAPI DevSweepWorker(LPVOID)
{
	for (;;)
	{
		int i = InterlockedIncrement(&g_sweep_next) - 1;
		if (i >= 254) break;
		char ip[64];
		sprintf_s(ip, "%s.%d", g_sweep_prefix.c_str(), i + 1);
		int ms = DevProbe(ip, g_sweep_port, g_sweep_to);
		if (ms >= 0)
		{
			InterlockedIncrement(&g_sweep_found);
			DevOut("  [扫描] 在线被控端 %s:%d (%dms)", ip, g_sweep_port, ms);
			DevAddOrUpdate(ip, g_sweep_port, "");
		}
	}
	return 0;
}

/* 并发扫描网段 x.x.x.1-254, 返回发现数量 */
static int DevTcpSweep(const std::string& prefix, int port, int timeout_ms)
{
	g_sweep_prefix = prefix;
	g_sweep_port = port;
	g_sweep_to = (timeout_ms > 0) ? timeout_ms : 200;
	InterlockedExchange(&g_sweep_next, 0);
	InterlockedExchange(&g_sweep_found, 0);
	const int NT = 32;
	HANDLE th[NT];
	int created = 0;
	for (int i = 0; i < NT; ++i)
	{
		th[i] = CreateThread(NULL, 0, DevSweepWorker, NULL, 0, NULL);
		if (th[i] != NULL) ++created;
	}
	if (created > 0) WaitForMultipleObjects(created, th, TRUE, INFINITE);
	for (int i = 0; i < NT; ++i) if (th[i] != NULL) CloseHandle(th[i]);
	return (int)g_sweep_found;
}

/* 切换到指定设备(复用现有连接/重连流程) */
static void DevConnectTo(const std::string& ip, int port)
{
	if (g_connected)
	{
		/* 断开当前连接 -> 接收线程按新的 g_ip/g_port 自动重连 */
		strncpy_s(g_ip, ip.c_str(), _TRUNCATE);
		g_port = port;
		if (g_hwnd != NULL)
		{
			SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_IP), g_ip);
			char pb[16];
			sprintf_s(pb, "%d", port);
			SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_PORT), pb);
		}
		SOCKET old = g_sock;
		g_sock = INVALID_SOCKET;
		if (old != INVALID_SOCKET) closesocket(old);
		SetConnected(false);
		DevOut("  已断开旧连接, 正在切换到 %s:%d (自动重连中...)", g_ip, g_port);
		DevTouch(ip, port);
		return;
	}

	strncpy_s(g_ip, ip.c_str(), _TRUNCATE);
	g_port = port;
	if (g_hwnd != NULL)
	{
		SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_IP), g_ip);
		char pb[16];
		sprintf_s(pb, "%d", port);
		SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_PORT), pb);
		DoConnect();          /* 读取上面刚写入的 IP/端口 并连接 */
	}
	else
	{
		SOCKET s = ConnectTo(g_ip, g_port);
		if (s != INVALID_SOCKET)
		{
			g_sock = s;
			g_last_hb = GetTickCount64();
			SetConnected(true);
			SendCmdPacket(CMD_TESTCONNECT, NULL, 0);
			RequestServerHello();
		}
	}
	if (g_connected)
	{
		DevOut("  已连接: %s:%d", g_ip, g_port);
		DevTouch(ip, port);
	}
	else
		DevOut("  连接失败: %s:%d (被控端是否在运行? 用 dev probe 检查)", ip.c_str(), port);
}

static void DevHelp()
{
	DevOut("设备列表命令:");
	DevOut("  dev | dev list            列出历史设备并实时探测在线状态");
	DevOut("  dev fast                  只列历史设备(不探测, 快)");
	DevOut("  dev add <ip[:port]> [名]  添加/更新设备(端口默认 9999)");
	DevOut("  dev del <序号|ip[:port]>  删除设备");
	DevOut("  dev probe <序号|ip[:port]> 探测指定设备在线状态");
	DevOut("  dev sniff [秒]            广播嗅探局域网被控端(默认 2 秒)");
	DevOut("  dev scan [网段前缀] [端口] 扫描网段(默认本机网段/9999)");
	DevOut("  dev connect <序号|地址>   切换到该设备(自动断开重连)");
	DevOut("  dev clear                 清空设备列表");
	DevOut("  提示: 无头用法 client.exe --dev \"list|sniff|scan 192.168.1|connect 1\"");
}

static void DevCommand(std::vector<std::string>& a)
{
	DevEnsureLock();
	std::string sub = (a.size() > 1) ? ExtLower(a[1]) : std::string("list");
	DevLoad();

	if (sub == "help" || sub == "?" || sub == "-h")
	{
		DevHelp();
		return;
	}
	if (sub == "list" || sub == "ls" || sub == "show" || sub == "stat" || sub == "status")
	{
		DevOut("  正在探测 %d 个历史设备...", (int)g_devs.size());
		DevPrintList(true);
		return;
	}
	if (sub == "fast" || sub == "cached")
	{
		DevPrintList(false);
		return;
	}
	if (sub == "clear")
	{
		EnterCriticalSection(&g_dev_lock);
		g_devs.clear();
		DevSaveRaw();
		LeaveCriticalSection(&g_dev_lock);
		DevOut("  设备列表已清空");
		return;
	}
	if (sub == "add")
	{
		if (a.size() < 3) { DevOut("  用法: dev add <ip[:port]> [备注名]"); return; }
		std::string ip;
		int port = 9999;
		std::string spec = ExtTrim(a[2]);
		ip = spec;
		size_t c = spec.find(':');
		if (c != std::string::npos)
		{
			ip = spec.substr(0, c);
			int p = atoi(spec.substr(c + 1).c_str());
			if (p > 0 && p < 65536) port = p;
		}
		if (ip.empty()) { DevOut("  非法地址"); return; }
		std::string nm = (a.size() > 3) ? a[3] : std::string();
		DevAddOrUpdate(ip, port, nm);
		DevOut("  已添加: %s:%d %s", ip.c_str(), port, nm.c_str());
		DevPrintList(false);
		return;
	}
	if (sub == "del" || sub == "rm" || sub == "remove" || sub == "delete")
	{
		std::string ip;
		int port = 9999;
		if (!DevResolveTarget(a, 2, ip, port)) { DevOut("  用法: dev del <序号|ip[:port]>"); return; }
		EnterCriticalSection(&g_dev_lock);
		int idx = DevFindIndex(ip, port);
		if (idx >= 0)
		{
			g_devs.erase(g_devs.begin() + idx);
			DevSaveRaw();
		}
		LeaveCriticalSection(&g_dev_lock);
		if (idx >= 0) DevOut("  已删除: %s:%d", ip.c_str(), port);
		else DevOut("  未找到: %s:%d", ip.c_str(), port);
		DevPrintList(false);
		return;
	}
	if (sub == "probe" || sub == "ping")
	{
		if (a.size() < 3 || ExtLower(a[2]) == "all")
		{
			DevOut("  正在探测全部历史设备...");
			DevPrintList(true);
			return;
		}
		std::string ip;
		int port = 9999;
		if (!DevResolveTarget(a, 2, ip, port)) { DevOut("  用法: dev probe <序号|ip[:port]>"); return; }
		int ms = DevProbe(ip, port, 800);
		EnterCriticalSection(&g_dev_lock);
		int idx = DevFindIndex(ip, port);
		if (idx >= 0) { g_devs[idx].rtt = ms; g_devs[idx].online = (ms >= 0) ? 1 : 0; }
		LeaveCriticalSection(&g_dev_lock);
		if (ms >= 0) DevOut("  %s:%d 在线 (延迟 %dms)", ip.c_str(), port, ms);
		else DevOut("  %s:%d 离线/不可达", ip.c_str(), port);
		return;
	}
	if (sub == "sniff" || sub == "discover" || sub == "udp")
	{
		int sec = (a.size() > 2) ? atoi(a[2].c_str()) : 2;
		if (sec <= 0 || sec > 30) sec = 2;
		DevOut("  正在广播嗅探局域网被控端 (%d 秒)...", sec);
		int f = DevUdpDiscover(sec * 1000);
		DevOut("  嗅探完成: 发现 %d 个被控端", f);
		DevPrintList(false);
		return;
	}
	if (sub == "scan" || sub == "sweep")
	{
		std::vector<std::string> pres;
		int port = 9999;
		if (a.size() > 2) pres.push_back(ExtTrim(a[2]));
		if (a.size() > 3)
		{
			int p = atoi(a[3].c_str());
			if (p > 0 && p < 65536) port = p;
		}
		if (pres.empty())
		{
			pres = DevLocalPrefixes();
			if (pres.empty())
			{
				DevOut("  无法自动获取本机网段, 请手动指定: dev scan 192.168.1 [端口]");
				return;
			}
		}
		int total = 0;
		for (size_t i = 0; i < pres.size(); ++i)
		{
			DevOut("  正在扫描网段 %s.1-254 (端口 %d, 32 线程)...", pres[i].c_str(), port);
			int f = DevTcpSweep(pres[i], port, 300);
			DevOut("  网段 %s.* 扫描完成: 发现 %d 个在线被控端", pres[i].c_str(), f);
			total += f;
		}
		DevOut("  扫描合计: %d 个", total);
		DevPrintList(false);
		return;
	}
	if (sub == "connect" || sub == "conn" || sub == "use" || sub == "open")
	{
		std::string ip;
		int port = 9999;
		if (!DevResolveTarget(a, 2, ip, port)) { DevOut("  用法: dev connect <序号|ip[:port]>"); return; }
		DevConnectTo(ip, port);
		return;
	}
	DevOut("  未知设备子命令: %s  (dev help 查看用法)", sub.c_str());
}

/* 无头模式: client.exe --dev "<子命令>" (自动去掉命令行引号) */
static int DevHeadlessMain(const std::string& argsIn)
{
	std::string args;
	for (size_t i = 0; i < argsIn.size(); ++i)
		if (argsIn[i] != '"') args += argsIn[i];
	std::vector<std::string> a;
	a.push_back("dev");
	std::vector<std::string> rest = ExtSplit(args);
	for (size_t i = 0; i < rest.size(); ++i) a.push_back(rest[i]);
	DevCommand(a);
	return 0;
}

static void ExtRunCommand(const std::string& raw)
{
	std::string line = ExtTrim(raw);
	if (line.empty()) return;

	if (g_cmd_hist.empty() || g_cmd_hist.back() != line) g_cmd_hist.push_back(line);
	while (g_cmd_hist.size() > 200) g_cmd_hist.pop_front();
	g_hist_pos = (int)g_cmd_hist.size();

	ConPrint("> %s", line.c_str());
	std::vector<std::string> a = ExtSplit(line);
	if (a.empty()) return;
	std::string cmd = ExtLower(a[0]);
	size_t n = a.size();

	if (cmd == "help" || cmd == "?" || cmd == "h")
	{
		for (size_t i = 0; i < sizeof(kHelp) / sizeof(kHelp[0]); ++i) ConPush(kHelp[i]);
		return;
	}
	if (cmd == "clear" || cmd == "cls") { ConClear(); ConPrint("> clear"); return; }
	if (cmd == "cam" || cmd == "camera")
	{
		std::string sub = (n > 1) ? ExtLower(a[1]) : std::string("on");
		if (sub == "off" || sub == "stop" || sub == "close" || sub == "hide")
		{
			ExtCamStop(true);
			ConPrint("  摄像头: 已停止");
		}
		else if (sub == "on" || sub == "start" || sub == "show" || sub == "win" || sub == "toggle")
		{
			ExtCamToggle(g_hwnd);
		}
		else if (sub == "stat" || sub == "status")
		{
			ConPrint("  摄像头: %s  %dx%d  %d fps", g_cam_status, g_cam_frm_w, g_cam_frm_h, g_cam_fps_val);
			ConPrint("  下次启动参数: %dx%d  %d fps  q%d  (cam res 640x360 | cam res native | cam fps 15 | cam q 65)",
				g_cam_reqW, g_cam_reqH, g_cam_reqFps, g_cam_reqQ);
		}
		else if (sub == "res" || sub == "size")
		{
			int w = 0, h = 0;
			if (n > 2 && sscanf_s(a[2].c_str(), "%dx%d", &w, &h) == 2 && w >= 160 && h >= 120)
			{
				g_cam_reqW = w; g_cam_reqH = h;
				ConPrint("  摄像头: 分辨率 %dx%d (重新 cam on 生效, 越小越流畅)", w, h);
			}
			else if (n > 2)
			{
				g_cam_reqW = 0; g_cam_reqH = 0;
				ConPrint("  摄像头: 使用摄像头原生分辨率 (重新 cam on 生效)");
			}
			else ConPrint("  用法: cam res <宽x高>|native   例: cam res 640x360");
		}
		else if (sub == "fps")
		{
			int v = (n > 2) ? atoi(a[2].c_str()) : 0;
			if (v >= 1 && v <= 60) { g_cam_reqFps = v; ConPrint("  摄像头: %d fps (重新 cam on 生效)", v); }
			else ConPrint("  用法: cam fps <1-60>");
		}
		else if (sub == "q" || sub == "quality")
		{
			int v = (n > 2) ? atoi(a[2].c_str()) : 0;
			if (v >= 10 && v <= 100) { g_cam_reqQ = v; ConPrint("  摄像头: 质量 q%d (重新 cam on 生效)", v); }
			else ConPrint("  用法: cam q <10-100>");
		}
		else
		{
			ConPrint("  用法: cam [on|off|stat|res <宽x高>|fps <n>|q <n>]    on = 弹出独立小窗口并开始采集");
		}
		return;
	}
	if (cmd == "ping")
	{
		if (!SendCmdPacket(CMD_TESTCONNECT, NULL, 0)) ConPrint("  发送失败: 未连接");
		else ConPrint("  ping 已发送 (回复会打印在下方)");
		return;
	}
	if (cmd == "dev" || cmd == "devices" || cmd == "host" || cmd == "hosts")
	{
		DevCommand(a);
		return;
	}
	if (cmd == "autostart" || cmd == "boot" || cmd == "startup")
	{
		std::string sub = (n > 1) ? a[1] : std::string("status");
		int op = 2;
		if (sub == "on" || sub == "1" || sub == "open" || sub == "enable")   op = 1;
		else if (sub == "off" || sub == "0" || sub == "close" || sub == "disable") op = 0;
		if (!SendCmdPacket(CMD_AUTOSTART, (const char*)&op, sizeof(op)))
			ConPrint("  发送失败: 未连接被控端");
		else
			ConPrint("  已请求%s开机自启, 结果见下方", op == 1 ? "开启" : (op == 0 ? "关闭" : "查询"));
		return;
	}
	if (cmd == "audio" || cmd == "sound" || cmd == "mic" || cmd == "microphone")
	{
		bool isMic = (cmd == "mic" || cmd == "microphone");
		SndOut* P = isMic ? SndMic() : SndSys();
		SndInit(P, isMic ? "mic" : "audio", isMic ? "麦克风(环境声)" : "系统声音",
			isMic ? CMD_MIC : CMD_AUDIO, isMic ? CMD_MIC_RES : CMD_AUDIO_RES);
		std::string sub = (n > 1) ? a[1] : std::string("start");
		if (sub == "start" || sub == "on") SndStart(P, (n > 2) ? atoi(a[2].c_str()) : 0);
		else if (sub == "stop" || sub == "off") SndStop(P);
		else if (sub == "stat" || sub == "status") SndStat(P);
		else if (sub == "list" || sub == "devs") SndList(P);
		else if (sub == "dev" || sub == "device")
		{
			if (n > 2) SndSetDev(P, atoi(a[2].c_str()));
			else ConPrint("  当前设备序号: %d (0=系统默认)", P->dev);
		}
		else if (sub == "gain")
		{
			if (n > 2) SndSetGain(P, atoi(a[2].c_str()));
			else ConPrint("  当前采集增益: %d%%", P->gain);
		}
		else if (sub == "vol" || sub == "volume")
		{
			if (n > 2) SndSetVolume(P, atoi(a[2].c_str()));
			else ConPrint("  当前播放音量: %d%%", P->vol);
		}
		else ConPrint("  用法: %s start [包长ms 10-500] | stop | stat | list | dev <n> | gain <10-800> | vol <0-100>", P->name);
		return;
	}
	if (cmd == "stat")
	{
		char buf[512];
		_snprintf_s(buf, _TRUNCATE, "  连接: %s   对端: %s:%d   延迟: %d ms   帧率: %d.%d fps   码率: %d KB/s",
			g_connected ? "已连接" : "未连接", g_ip, g_port, g_last_rtt,
			g_stat_fps_x10 / 10, g_stat_fps_x10 % 10, g_stat_kbps);
		ConPush(buf);
		_snprintf_s(buf, _TRUNCATE, "  被控端目录: %s   编码: %s   画质: %d   请求帧率: %d",
			g_cur_path[0] ? g_cur_path : "(未获取)", g_encoder == RC_ENC_JPEG ? "JPEG" : "PNG", g_quality, g_fps);
		ConPush(buf);
		if (g_locate_on)
		{
			_snprintf_s(buf, _TRUNCATE, "  最近定位: 被控端光标 (%d,%d)  屏幕 %dx%d", g_locate_x, g_locate_y, g_locate_sw, g_locate_sh);
			ConPush(buf);
		}
		return;
	}
	if (cmd == "apk" || cmd == "androidapk" || cmd == "android")
	{
		ExtGenerateAndroidApk(g_hwnd);
		return;
	}
	if (cmd == "shot" || cmd == "screenshot")
	{
		ExtSaveShot(n > 1 ? a[1] : std::string());
		return;
	}
	if (cmd == "locate" || cmd == "cur" || cmd == "cursor")
	{
		if (SendCmdPacket(CMD_CURSOR, NULL, 0)) ConPrint("  已请求被控端光标位置 ...");
		else ConPrint("  发送失败: 未连接");
		return;
	}
	if (cmd == "sysinfo" || cmd == "info")
	{
		if (SendCmdPacket(CMD_SYSINFO, NULL, 0)) ConPrint("  已请求被控端系统信息 ...");
		else ConPrint("  发送失败: 未连接");
		return;
	}
	if (cmd == "clip")
	{
		std::string sub = (n > 1) ? ExtLower(a[1]) : std::string("get");
		if (sub == "get")
		{
			ClipReq r; memset(&r, 0, sizeof(r));
			r.op = RC_CLIP_GET;
			SendCmdPacket(CMD_CLIPBOARD, (char*)&r, sizeof(r));
			ConPrint("  已请求被控端剪贴板 ...");
		}
		else if (sub == "set")
		{
			std::string text;
			for (size_t i = 2; i < n; ++i) { if (i > 2) text += " "; text += a[i]; }
			if (text.empty()) { ConPrint("  用法: clip set <文本>"); return; }
			if (!SendClipboard(1, text))
				ConPrint("  写入剪贴板失败: 未连接被控端");
			else
				ConPrint("  已写入被控端剪贴板 (%d 字节文本, 已转 UTF-8 发送)", (int)text.size());
		}
		else
		{
			ClipReq r; memset(&r, 0, sizeof(r));
			r.op = RC_CLIP_CLEAR;
			SendCmdPacket(CMD_CLIPBOARD, (char*)&r, sizeof(r));
			ConPrint("  已清空被控端剪贴板");
		}
		return;
	}
	if (cmd == "text")
	{
		std::string text;
		for (size_t i = 1; i < n; ++i) { if (i > 1) text += " "; text += a[i]; }
		if (text.empty()) { ConPrint("  用法: text <要输入的文本>"); return; }
		const std::string u = ExtGbk2Utf8(text);
		if (!SendCmdPacket(CMD_KEYTEXT, u.data(), (int)u.size())) { ConPrint("  失败: 未连接被控端"); return; }
		ConPrint("  已 Unicode 直投上屏 %d 个字符(中文/表情均可用)", (int)text.size());
		return;
	}
	if (cmd == "type")
	{
		/* 含中文的文本输入: 先把文本写进被控端剪贴板, 再在被控端按 Ctrl+V 粘贴。
		   同一个 TCP 连接上是顺序处理的, 所以不必 sleep 等待。 */
		std::string text;
		for (size_t i = 1; i < n; ++i) { if (i > 1) text += " "; text += a[i]; }
		if (text.empty()) { ConPrint("  用法: type <文本>   (含中文也可)"); return; }
		const std::string u = ExtGbk2Utf8(text);
		if (!SendCmdPacket(CMD_KEYTEXT, u.data(), (int)u.size())) { ConPrint("  失败: 未连接被控端"); return; }
		ConPrint("  已 Unicode 直投上屏 %d 个字符(不再占用被控端剪贴板)", (int)text.size());
		return;
	}
	if (cmd == "mouse" || cmd == "click")
	{
		if (n < 3) { ConPrint("  用法: %s <x> <y> [l|r|d]", cmd.c_str()); return; }
		int x = atoi(a[1].c_str()), y = atoi(a[2].c_str());
		Mouse m; memset(&m, 0, sizeof(m));
		m.action = MOUSE_MOVE; m.ptXY.x = x; m.ptXY.y = y;
		SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
		if (cmd == "click")
		{
			std::string bt = (n > 3) ? ExtLower(a[3]) : std::string("l");
			int down, up;
			if (bt == "r" || bt == "right") { down = MOUSE_RDOWN; up = MOUSE_RUP; }
			else if (bt == "m" || bt == "middle") { down = MOUSE_MDOWN; up = MOUSE_MUP; }
			else { down = MOUSE_LDOWN; up = MOUSE_LUP; }
			Sleep(60);
			memset(&m, 0, sizeof(m)); m.action = down; m.ptXY.x = x; m.ptXY.y = y;
			SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
			Sleep(40);
			memset(&m, 0, sizeof(m)); m.action = up; m.ptXY.x = x; m.ptXY.y = y;
			SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
			if (bt == "d" || bt == "double")
			{
				Sleep(60);
				memset(&m, 0, sizeof(m)); m.action = MOUSE_LDOWN; m.ptXY.x = x; m.ptXY.y = y;
				SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
				Sleep(40);
				memset(&m, 0, sizeof(m)); m.action = MOUSE_LUP; m.ptXY.x = x; m.ptXY.y = y;
				SendCmdPacket(CMD_MOUSE, (char*)&m, sizeof(m));
			}
		}
		ConPrint("  已发送 %s (%d, %d)", cmd.c_str(), x, y);
		return;
	}
	if (cmd == "key" || cmd == "keydown" || cmd == "keyup")
	{
		if (n < 2) { ConPrint("  用法: key <名称> [down|up]"); return; }
		int vk = ExtVkFromName(a[1]);
		if (vk == 0) { ConPrint("  未知按键: %s", a[1].c_str()); return; }
		bool up = (cmd == "keyup");
		if (n > 2) { std::string s = ExtLower(a[2]); up = (s == "up"); }
		ExtSendKey(vk, up);
		ConPrint("  已发送按键 VK=0x%02X (%s)", vk, up ? "抬起" : "按下");
		return;
	}
	if (cmd == "ls" || cmd == "dir")
	{
		ExtReqListPath(n > 1 ? a[1] : std::string(g_cur_path));
		ConPrint("  已请求目录列表");
		return;
	}
	if (cmd == "cd")
	{
		if (n < 2) { ConPrint("  用法: cd <路径>"); return; }
		ExtReqListPath(a[1]);
		return;
	}
	if (cmd == "get")
	{
		if (n < 2) { ConPrint("  用法: get <远端文件> [本机路径]"); return; }
		ExtStartDownload(a[1], n > 2 ? a[2] : std::string());
		return;
	}
	if (cmd == "put")
	{
		if (n < 2) { ConPrint("  用法: put <本机文件> [远端路径]"); return; }
		ExtStartUpload(a[1], n > 2 ? a[2] : std::string());
		return;
	}
	if (cmd == "mkdir" || cmd == "md")
	{
		if (n < 2) { ConPrint("  用法: mkdir <名称>"); return; }
		SendFileOp(RC_OP_MKDIR, JoinPath(std::string(g_cur_path), a[1]), "");
		ConPrint("  已请求新建目录: %s", a[1].c_str());
		return;
	}
	if (cmd == "rm" || cmd == "del")
	{
		if (n < 2) { ConPrint("  用法: rm <路径>"); return; }
		SendFileOp(RC_OP_DELETE, a[1], "");
		ConPrint("  已请求删除: %s", a[1].c_str());
		return;
	}
	if (cmd == "rename" || cmd == "mv")
	{
		if (n < 3) { ConPrint("  用法: rename <旧> <新>"); return; }
		SendFileOp(RC_OP_RENAME, a[1], a[2]);
		ConPrint("  已请求重命名: %s -> %s", a[1].c_str(), a[2].c_str());
		return;
	}
	if (cmd == "screen")
	{
		if (n < 3) { ConPrint("  用法: screen fps <n> | screen q <n> | screen enc <jpeg|png>"); return; }
		std::string sub = ExtLower(a[1]);
		if (sub == "fps")
		{
			g_fps = atoi(a[2].c_str());
			if (g_fps < 1) g_fps = 1;
			if (g_fps > 60) g_fps = 60;
		}
		else if (sub == "q" || sub == "quality")
		{
			g_quality = atoi(a[2].c_str());
			if (g_quality < 1) g_quality = 1;
			if (g_quality > 100) g_quality = 100;
		}
		else if (sub == "enc" || sub == "encode")
		{
			std::string e = ExtLower(a[2]);
			g_encoder = (e == "jpeg" || e == "jpg") ? RC_ENC_JPEG : RC_ENC_PNG;
		}
		else { ConPrint("  未知子命令: %s", sub.c_str()); return; }
		ApplyScreenCtrl(true);
		ConPrint("  画面参数: fps=%d quality=%d enc=%s", g_fps, g_quality, g_encoder == RC_ENC_JPEG ? "JPEG" : "PNG");
		return;
	}
	if (cmd == "rec")
	{
		int sec = (n > 1) ? atoi(a[1].c_str()) : 10;
		int flags = 0;
		if (n <= 2) flags = RC_REC_MOUSE | RC_REC_KEY;
		else
		{
			for (size_t i = 2; i < n; ++i)
			{
				std::string f = ExtLower(a[i]);
				if (f == "m" || f == "mouse") flags |= RC_REC_MOUSE;
				if (f == "k" || f == "key" || f == "keyboard") flags |= RC_REC_KEY;
			}
		}
		if (flags == 0) flags = RC_REC_MOUSE | RC_REC_KEY;
		ExtStartRecord(sec, flags);
		return;
	}
	if (cmd == "recstop") { ExtStopRecord(); return; }
	if (cmd == "recsave") { ExtRecSaveToFile(n > 1 ? a[1] : std::string()); return; }
	if (cmd == "recplay") { ExtReplayStart(); return; }
	if (cmd == "recread")
	{
		ConPrint("  最近录制: %d 字节", (int)g_rec_text.size());
		HWND ed = (g_hwnd != NULL) ? GetDlgItem(g_hwnd, IDC_EDIT_RECOUT) : NULL;
		if (ed != NULL) SetWindowTextA(ed, g_rec_text.c_str());
		return;
	}
	ConPrint("  未知命令: %s   (输入 help 查看全部命令)", cmd.c_str());
}

/* ---------------- 补充工具 ---------------- */
static bool ExtWriteFile(const std::string& path, const std::string& data)
{
	FILE* fp = NULL;
	if (fopen_s(&fp, path.c_str(), "wb") != 0 || fp == NULL)
		return false;
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
	return true;
}

/* ---- 文本编码转换 (本地GBK <-> 协议UTF-8) ---- */
static std::string ExtGbk2Utf8(const std::string& g)
{
	if (g.empty()) return std::string();
	int wn = MultiByteToWideChar(CP_ACP, 0, g.c_str(), (int)g.size(), NULL, 0);
	if (wn <= 0) return g;
	std::wstring w((size_t)wn, L'\0');
	MultiByteToWideChar(CP_ACP, 0, g.c_str(), (int)g.size(), &w[0], wn);
	int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, NULL, 0, NULL, NULL);
	if (un <= 0) return g;
	std::string u((size_t)un, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &u[0], un, NULL, NULL);
	return u;
}
static std::string ExtUtf8ToGbk(const std::string& u)
{
	if (u.empty()) return std::string();
	/* 先按严格 UTF-8 解; 失败说明调用方给的是本机 ANSI(被控端旧版/纯 ASCII 状态串), 原样返回 */
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u.c_str(), (int)u.size(), NULL, 0);
	if (wn <= 0) return u;
	std::wstring w((size_t)wn, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], wn);
	int an = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, NULL, 0, NULL, NULL);
	if (an <= 0) return u;
	std::string a((size_t)an, '\0');
	WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, &a[0], an, NULL, NULL);
	return a;
}

static bool SendClipboard(int op, const std::string& text)
{
	if (!g_connected)
		return false;
	ClipReq r;
	memset(&r, 0, sizeof(r));
	r.op = op;
	std::string body((const char*)&r, sizeof(r));
	body += ExtGbk2Utf8(text);   /* 协议统一 UTF-8 */
	return SendCmdPacket(CMD_CLIPBOARD, body.data(), (int)body.size());
}

/* ---------------- 工具页专用 ID ---------------- */
#define IDC_BTN_TOOLLOCATE   1050
#define IDC_BTN_TOOLSYSINFO  1051
#define IDC_BTN_TOOLCLIPGET  1052
#define IDC_REC_LBL          1053
#define IDC_CLIP_LBL         1054
#define IDC_BTN_GENAPK       1055   /* 生成安卓被控端 APK */

/* ============================================================
 *  v9: 剪贴板同步虚拟通道 — 全局状态(定义在此以便 UI 回调使用)
 * ============================================================ */
static CRITICAL_SECTION g_cclip_lock;
static LONG   g_cclip_auto = 1;        /* 自动双向同步开关(UI) */
static LONG   g_cclip_hold = 0;        /* 正在写本机剪贴板(抑制上报, 防回环) */
static LONG   g_cclip_ready = 0;       /* 双方 HELLO 完成 */
static DWORD  g_cclip_last_seq = 0;
static int    g_cclip_peer_fmt = 0;
static std::vector<ClipFileDesc> g_cclip_peer_files;
static ClipMsg g_cclip_peer_offer = { 0 };
static std::vector<std::string>  g_cclip_local_files;
#define RC_CCLIP_CHUNK  (64 * 1024)
#define RC_CLIP_MSG_MAX (4 * 1024 * 1024)

static void CClipOfferSelf(void);      /* 前置声明(实现在回包处理区) */

static const int kIdsFiles[] = {
	IDC_EDIT_PATH, IDC_LBL_PATH, IDC_BTN_REFRESH, IDC_BTN_UP, IDC_BTN_DOWNLOAD, IDC_BTN_UPLOAD,
	IDC_BTN_MKDIR, IDC_BTN_RENAME, IDC_BTN_DELETE, IDC_EDIT_NAME, IDC_LBL_NAME, IDC_LIST_FILES, IDC_EDIT_LOG };
static const int kIdsCmd[] = { IDC_CMD_OUT, IDC_CMD_IN, IDC_BTN_RUN, IDC_BTN_CLEAR, IDC_BTN_HELP, IDC_CMD_TIP };
static const int kIdsRec[] = {
	IDC_REC_LBL, IDC_EDIT_RECSEC, IDC_CHK_MOUSE, IDC_CHK_KEY, IDC_BTN_RECSTART, IDC_BTN_RECSTOP,
	IDC_BTN_RECPLAY, IDC_BTN_RECSAVE, IDC_BTN_RECCLEAR, IDC_REC_STATUS, IDC_EDIT_RECOUT, IDC_REC_TIP };
static const int kIdsAud[] = { IDC_BTN_AUDSYS, IDC_SLD_AUDSYS, IDC_BTN_AUDMIC, IDC_SLD_AUDMIC, IDC_AUD_STATUS };
static const int kIdsTool[] = {
	IDC_BTN_SNAPSAVE, IDC_BTN_TOOLLOCATE, IDC_BTN_TOOLSYSINFO, IDC_BTN_TOOLCLIPGET, IDC_BTN_CLIPSET,
	IDC_BTN_CLIPCLR, IDC_CLIP_LBL, IDC_EDIT_CLIPTXT, IDC_TOOL_OUT, IDC_TOOL_TIP,
	IDC_BTN_GENAPK, IDC_CHK_CLIPSYNC,
	IDC_BTN_FITFILL, IDC_BTN_FITADAPT, IDC_BTN_FULLSCREEN, IDC_BTN_ZOOMIN, IDC_BTN_ZOOMOUT,
	IDC_BTN_ZOOM100, IDC_LBL_VIEWMODE };

static void ExtShowIds(HWND hwnd, const int* ids, int cnt, bool show)
{
	for (int i = 0; i < cnt; ++i)
	{
		HWND c = GetDlgItem(hwnd, ids[i]);
		if (c != NULL)
		{
			ShowWindow(c, show ? SW_SHOW : SW_HIDE);
			/* 页内控件与 Tab 控件是同父兄弟: 必须把当前页控件提到 Z 序最上层,
			   否则会被 SysTabControl32 盖住(矩形不重叠也会“看不见按钮”) */
			if (show)
				SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}
}

static void ExtCreateControls(HWND hwnd)
{
	INITCOMMONCONTROLSEX icc;
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_TAB_CLASSES;
	InitCommonControlsEx(&icc);

	/* 页签改用自控按钮(不再用 SysTabControl32), 文字/位置/字体完全可控 */
	g_ext_tab = NULL;
	{
		const char* pgnames[4] = { "文件操作", "命令行", "键鼠录制", "工具" };
		for (int i = 0; i < 4; ++i)
		{
			DWORD st = BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP;
			if (i == 0)
				st |= WS_GROUP;
			g_ext_pagebtn[i] = Mk(hwnd, "BUTTON", pgnames[i], st, 0, 0, 80, 24, IDC_PAGEBTN0 + i);
		}
	}

	/* 命令行页 */
	Mk(hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		0, 0, 100, 100, IDC_CMD_OUT);
	Mk(hwnd, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 100, 22, IDC_CMD_IN);
	Mk(hwnd, "BUTTON", "执行", BS_DEFPUSHBUTTON, 0, 0, 70, 24, IDC_BTN_RUN);
	Mk(hwnd, "BUTTON", "清空", BS_PUSHBUTTON, 0, 0, 70, 24, IDC_BTN_CLEAR);
	Mk(hwnd, "BUTTON", "帮助", BS_PUSHBUTTON, 0, 0, 70, 22, IDC_BTN_HELP);
	Mk(hwnd, "STATIC", "示例: help / locate / shot / sysinfo / clip get / ls / get 远端文件 / rec 10",
		0, 0, 0, 100, 20, IDC_CMD_TIP);

	/* 键鼠录制页 */
	Mk(hwnd, "STATIC", "录制时长(秒):", 0, 0, 0, 90, 18, IDC_REC_LBL);
	Mk(hwnd, "EDIT", "10", WS_BORDER | ES_NUMBER, 0, 0, 50, 22, IDC_EDIT_RECSEC);
	HWND ck1 = Mk(hwnd, "BUTTON", "鼠标", BS_AUTOCHECKBOX, 0, 0, 60, 20, IDC_CHK_MOUSE);
	HWND ck2 = Mk(hwnd, "BUTTON", "键盘", BS_AUTOCHECKBOX, 0, 0, 60, 20, IDC_CHK_KEY);
	SendMessageA(ck1, BM_SETCHECK, BST_CHECKED, 0);
	SendMessageA(ck2, BM_SETCHECK, BST_CHECKED, 0);
	Mk(hwnd, "BUTTON", "开始录制", BS_PUSHBUTTON, 0, 0, 96, 26, IDC_BTN_RECSTART);
	Mk(hwnd, "BUTTON", "停止", BS_PUSHBUTTON, 0, 0, 62, 26, IDC_BTN_RECSTOP);
	Mk(hwnd, "BUTTON", "回放", BS_PUSHBUTTON, 0, 0, 62, 26, IDC_BTN_RECPLAY);
	Mk(hwnd, "BUTTON", "保存文件", BS_PUSHBUTTON, 0, 0, 62, 26, IDC_BTN_RECSAVE);
	Mk(hwnd, "BUTTON", "清空", BS_PUSHBUTTON, 0, 0, 62, 24, IDC_BTN_RECCLEAR);
	Mk(hwnd, "STATIC", "状态: 空闲", 0, 0, 0, 100, 18, IDC_REC_STATUS);
	Mk(hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		0, 0, 100, 100, IDC_EDIT_RECOUT);
	Mk(hwnd, "STATIC", "录制期间被控端采样鼠标/键盘，结束后数据一次性回传。",
		0, 0, 0, 100, 18, IDC_REC_TIP);

	/* 工具页 */
	Mk(hwnd, "BUTTON", "截图并保存", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_SNAPSAVE);
	Mk(hwnd, "BUTTON", "定位光标", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_TOOLLOCATE);
	Mk(hwnd, "BUTTON", "系统信息", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_TOOLSYSINFO);
	Mk(hwnd, "BUTTON", "读剪贴板", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_TOOLCLIPGET);
	Mk(hwnd, "BUTTON", "清空剪贴板", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_CLIPCLR);
	Mk(hwnd, "BUTTON", "生成安卓被控端", BS_PUSHBUTTON, 0, 0, 90, 26, IDC_BTN_GENAPK);
	Mk(hwnd, "STATIC", "剪贴板内容(可编辑后写入被控端):", 0, 0, 0, 100, 18, IDC_CLIP_LBL);
	Mk(hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 0, 0, 100, 56, IDC_EDIT_CLIPTXT);
	Mk(hwnd, "BUTTON", "写入被控端剪贴板", BS_PUSHBUTTON, 0, 0, 140, 26, IDC_BTN_CLIPSET);
	Mk(hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		0, 0, 100, 100, IDC_TOOL_OUT);
	HWND ckClip = Mk(hwnd, "BUTTON", "剪贴板自动同步(双向: 文本/图片/文件)", BS_AUTOCHECKBOX, 0, 0, 250, 20, IDC_CHK_CLIPSYNC);
	SendMessageA(ckClip, BM_SETCHECK, BST_CHECKED, 0);
	/* v10: 画面铺展比例 / 全屏 */
	Mk(hwnd, "BUTTON", "铺满窗口", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_FITFILL);
	Mk(hwnd, "BUTTON", "适应窗口", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_FITADAPT);
	Mk(hwnd, "BUTTON", "全屏 F11", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_FULLSCREEN);
	Mk(hwnd, "BUTTON", "放大 +", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_ZOOMIN);
	Mk(hwnd, "BUTTON", "缩小 -", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_ZOOMOUT);
	Mk(hwnd, "BUTTON", "1:1", BS_PUSHBUTTON, 0, 0, 90, 24, IDC_BTN_ZOOM100);
	Mk(hwnd, "STATIC", "画面: 适应窗口", SS_LEFT, 0, 0, 100, 18, IDC_LBL_VIEWMODE);
	Mk(hwnd, "STATIC", "提示: 定位会在画面上画红色十字; 截图默认存到程序目录 rc_shot_时间.jpg",
		0, 0, 0, 100, 18, IDC_TOOL_TIP);

	/* 顶部快捷工具条 (常驻) */

	Mk(hwnd, "BUTTON", "截图并保存", BS_PUSHBUTTON, 0, 0, 96, 24, IDC_BTN_SNAP);

	Mk(hwnd, "BUTTON", "定位光标", BS_PUSHBUTTON, 0, 0, 96, 24, IDC_BTN_LOCATE);

	Mk(hwnd, "BUTTON", "读取剪贴板", BS_PUSHBUTTON, 0, 0, 96, 24, IDC_BTN_CLIPGET);

	Mk(hwnd, "BUTTON", "系统信息", BS_PUSHBUTTON, 0, 0, 96, 24, IDC_BTN_SYSINFO);

	Mk(hwnd, "BUTTON", "摄像头", BS_PUSHBUTTON, 0, 0, 96, 24, IDC_BTN_CAM);



	/* 命令输入框子类化: 上/下键翻历史 */
	g_cmd_in_old = (WNDPROC)SetWindowLongPtrA(GetDlgItem(hwnd, IDC_CMD_IN), GWLP_WNDPROC, (LONG_PTR)ExtCmdInProc);

	ConPrint("控制端已启动。输入 help 查看全部命令。");
	ExtSetPage(hwnd, EXT_PAGE_FILES);
}

/* ---------------- 页面切换 ---------------- */
static void ExtSetPage(HWND hwnd, int page)
{
	if (page < 0 || page > 3)
		page = 0;
	g_ext_page = page;
	ExtShowIds(hwnd, kIdsFiles, _countof(kIdsFiles), page == EXT_PAGE_FILES);
	ExtShowIds(hwnd, kIdsCmd, _countof(kIdsCmd), page == EXT_PAGE_CMD);
	ExtShowIds(hwnd, kIdsRec, _countof(kIdsRec), page == EXT_PAGE_REC);
	ExtShowIds(hwnd, kIdsTool, _countof(kIdsTool), page == EXT_PAGE_TOOL);
	ExtShowIds(hwnd, kIdsAud, _countof(kIdsAud), page == EXT_PAGE_TOOL);
	for (int i = 0; i < 4; ++i)
		if (g_ext_pagebtn[i] != NULL)
			SendMessageA(g_ext_pagebtn[i], BM_SETCHECK,
				(WPARAM)((i == page) ? BST_CHECKED : BST_UNCHECKED), 0);
	ExtLayout(hwnd);
	if (page == EXT_PAGE_FILES)
		FillFileList();
	else if (page == EXT_PAGE_CMD)
		SetFocus(GetDlgItem(hwnd, IDC_CMD_IN));
	ExtConsoleRefresh(hwnd);
}

/* ============================================================
 *  v10: 画面铺展比例 / 全屏模式
 * ============================================================ */
static void Layout(HWND hwnd);       /* 前置声明: 定义在后面 */

static void ViewNotifyResize()
{
	InterlockedExchange(&g_need_full_render, 1);
	if (g_view != NULL)
	{
		InvalidateRect(g_view, NULL, TRUE);
		UpdateWindow(g_view);
	}
}

static void ExtUpdateViewModeText(HWND hwnd)
{
	const LONG fit = InterlockedCompareExchange(&g_view_fit, 0, 0);
	const LONG zoom = InterlockedCompareExchange(&g_view_zoom, 0, 0);
	char t[96];
	if (fit == 1)
		_snprintf_s(t, _TRUNCATE, "画面: 铺满窗口(拉伸到满, 无黑边)");
	else if (fit == 2)
		_snprintf_s(t, _TRUNCATE, "画面: 手动缩放 %d%%", (int)zoom);
	else
		_snprintf_s(t, _TRUNCATE, "画面: 适应窗口(等比, 有黑边)");
	SetWindowTextA(GetDlgItem(hwnd, IDC_LBL_VIEWMODE), t);
}

/* mode: -1=只调百分比  0=适应  1=铺满  2=手动百分比 */
static void ExtSetFitMode(HWND hwnd, LONG mode, int zoomDelta)
{
	char t[96];
	if (zoomDelta != 0)
	{
		LONG z = InterlockedCompareExchange(&g_view_zoom, 0, 0) + zoomDelta;
		if (z < 20) z = 20;
		if (z > 400) z = 400;
		InterlockedExchange(&g_view_zoom, z);
		InterlockedExchange(&g_view_fit, 2);
		_snprintf_s(t, _TRUNCATE, "  画面缩放: %d%%", (int)z);
	}
	else
	{
		if (mode >= 0)
			InterlockedExchange(&g_view_fit, mode);
		if (mode == 2)
			InterlockedExchange(&g_view_zoom, 100);
		const LONG fit = InterlockedCompareExchange(&g_view_fit, 0, 0);
		if (fit == 1)
			_snprintf_s(t, _TRUNCATE, "  画面铺展: 铺满窗口(拉伸, 无黑边)");
		else if (fit == 2)
			_snprintf_s(t, _TRUNCATE, "  画面铺展: 原始尺寸 1:1");
		else
			_snprintf_s(t, _TRUNCATE, "  画面铺展: 适应窗口(等比, 保留黑边)");
	}
	ExtUpdateViewModeText(hwnd);
	ViewNotifyResize();
	ConPrint("%s", t);
}

static void ExtSetFullScreen(HWND hwnd, bool on)
{
	if (hwnd == NULL)
		return;
	const bool cur = (InterlockedCompareExchange(&g_fullscreen, 0, 0) != 0);
	if (on == cur)
		return;
	if (on)
	{
		memset(&g_fs_wp, 0, sizeof(g_fs_wp));
		g_fs_wp.length = sizeof(WINDOWPLACEMENT);
		GetWindowPlacement(hwnd, &g_fs_wp);
		g_fs_count = 0;
		for (HWND c = GetWindow(hwnd, GW_CHILD); c != NULL && g_fs_count < RC_FS_MAXWND; c = GetWindow(c, GW_HWNDNEXT))
		{
			if (c == g_view)
				continue;
			g_fs_saved[g_fs_count++] = c;
			ShowWindow(c, SW_HIDE);
		}
		LONG_PTR st = GetWindowLongPtrA(hwnd, GWL_STYLE);
		SetWindowLongPtrA(hwnd, GWL_STYLE,
			(st & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi;
		memset(&mi, 0, sizeof(mi));
		mi.cbSize = sizeof(MONITORINFO);
		if (GetMonitorInfoA(mon, &mi))
		{
			SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
			             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
			             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		}
		InterlockedExchange(&g_fullscreen, 1);
		Layout(hwnd);
		SetFocus(g_view);
		ConPrint("  已进入全屏模式: 远程画面铺满整个屏幕 (按 F11 或 Esc 退出)");
	}
	else
	{
		InterlockedExchange(&g_fullscreen, 0);
		LONG_PTR st = GetWindowLongPtrA(hwnd, GWL_STYLE);
		SetWindowLongPtrA(hwnd, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		if (g_fs_wp.length == sizeof(WINDOWPLACEMENT))
		{
			SetWindowPlacement(hwnd, &g_fs_wp);
			g_fs_wp.length = 0;
		}
		for (int i = 0; i < g_fs_count; ++i)
			ShowWindow(g_fs_saved[i], SW_SHOW);
		g_fs_count = 0;
		ExtSetPage(hwnd, g_ext_page);   /* 恢复: 当前页可见, 其余页隐藏 */
		Layout(hwnd);
		ExtLayout(hwnd);
		ConPrint("  已退出全屏模式");
	}
	ViewNotifyResize();
}

/* ---------------- 布局 ---------------- */
/* ---------------- v6: 声音(在被控端采集, 播放到本机) 开关 UI ---------------- */
static void ExtAudioBtnText(char* out, int n, SndOut* P, const char* what)
{
	int on = InterlockedCompareExchange((volatile LONG*)&P->on, 1, 1);
	_snprintf_s(out, n, _TRUNCATE, "%s: %s", what, on ? "开" : "关");
}
static void ExtAudioUpdateUI(HWND hwnd)
{
	if (GetDlgItem(hwnd, IDC_BTN_AUDSYS) == NULL) return;
	char b1[64], b2[64];
	ExtAudioBtnText(b1, sizeof(b1), &g_sndSys, "系统声音(被控端→本机)");
	ExtAudioBtnText(b2, sizeof(b2), &g_sndMic, "麦克风(被控端→本机)");
	SetWindowTextA(GetDlgItem(hwnd, IDC_BTN_AUDSYS), b1);
	SetWindowTextA(GetDlgItem(hwnd, IDC_BTN_AUDMIC), b2);
	SendMessageA(GetDlgItem(hwnd, IDC_SLD_AUDSYS), TBM_SETPOS, TRUE, g_sndSys.vol);
	SendMessageA(GetDlgItem(hwnd, IDC_SLD_AUDMIC), TBM_SETPOS, TRUE, g_sndMic.vol);
	char st[160];
	_snprintf_s(st, _TRUNCATE, "音量: 系统声音 %d%% / 麦克风 %d%%   (被控端采集增益用 audio gain / mic gain)",
		g_sndSys.vol, g_sndMic.vol);
	SetWindowTextA(GetDlgItem(hwnd, IDC_AUD_STATUS), st);
}
static void ExtAudioToggle(SndOut* P, HWND hwnd)
{
	if (InterlockedCompareExchange((volatile LONG*)&P->on, 1, 1))
		SndStop(P);
	else
		SndStart(P, 0);
	ExtAudioUpdateUI(hwnd);
}
static void ExtLayout(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int cx = rc.right, cy = rc.bottom;
	const int SBH = 22;
	int px = cx - 340;
	int cyu = cy - SBH;
	if (px < 150)
		px = 150;
	int TW = cx - px - 10;
	if (TW < 140)
		TW = 140;

	/* --- 页签条: 4 个自控按钮, 位置/宽度全由这里算, 绝不重叠 --- */
	const int TBH = 26;
	int tbw = (TW - 10) / 4;
	if (tbw < 30)
		tbw = 30;
	for (int i = 0; i < 4; ++i)
		if (g_ext_pagebtn[i] != NULL)
			MoveWindow(g_ext_pagebtn[i], px + 4 + tbw * i, 44, tbw, TBH, TRUE);

	/* --- 页面客户区: 固定公式(不再依赖 Tab 控件估算矩形) --- */
	int x0 = px + 4;
	int y0 = 44 + TBH + 4;
	int w = TW - 8;
	int h = (cyu - y0) - 6;
	if (w < 60)
		w = 60;
	if (h < 60)
		h = 60;

	/* 命令行页 */
	MoveWindow(GetDlgItem(hwnd, IDC_CMD_OUT), x0, y0, w, h - 56, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_CMD_IN), x0, y0 + h - 50, w - 154, 24, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RUN), x0 + w - 146, y0 + h - 50, 70, 24, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR), x0 + w - 72, y0 + h - 50, 70, 24, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_HELP), x0, y0 + h - 22, 70, 20, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_CMD_TIP), x0 + 76, y0 + h - 22, w - 76, 20, TRUE);

	/* 录制页 */
	MoveWindow(GetDlgItem(hwnd, IDC_REC_LBL), x0, y0 + 2, 90, 18, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_EDIT_RECSEC), x0 + 92, y0, 50, 22, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_CHK_MOUSE), x0 + 150, y0 + 2, 60, 20, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_CHK_KEY), x0 + 214, y0 + 2, 60, 20, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RECSTART), x0, y0 + 30, 96, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RECSTOP), x0 + 100, y0 + 30, 62, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RECPLAY), x0 + 166, y0 + 30, 62, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RECSAVE), x0 + 232, y0 + 30, 62, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_RECCLEAR), x0, y0 + 62, 62, 24, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_REC_STATUS), x0 + 70, y0 + 65, w - 70, 18, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_EDIT_RECOUT), x0, y0 + 92, w, h - 116, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_REC_TIP), x0, y0 + h - 20, w, 18, TRUE);

	/* 工具页 */
	int bw = (w - 8) / 3;
	if (bw < 60) bw = 60;
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_SNAPSAVE), x0, y0, bw, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_TOOLLOCATE), x0 + bw + 4, y0, bw, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_TOOLSYSINFO), x0 + (bw + 4) * 2, y0, bw, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_TOOLCLIPGET), x0, y0 + 30, bw, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLIPCLR), x0 + bw + 4, y0 + 30, bw, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_GENAPK), x0 + (bw + 4) * 2, y0 + 30, bw, 26, TRUE);
	/* 声音开关(懒创建, 只创建一次) */
	if (GetDlgItem(hwnd, IDC_BTN_AUDSYS) == NULL)
	{
		Mk(hwnd, "BUTTON", "系统声音(被控端→本机): 关", BS_PUSHBUTTON, 0, 0, 20, 20, IDC_BTN_AUDSYS);
		Mk(hwnd, "msctls_trackbar32", "", TBS_HORZ | TBS_NOTICKS, 0, 0, 20, 20, IDC_SLD_AUDSYS);
		Mk(hwnd, "BUTTON", "麦克风(被控端→本机): 关", BS_PUSHBUTTON, 0, 0, 20, 20, IDC_BTN_AUDMIC);
		Mk(hwnd, "msctls_trackbar32", "", TBS_HORZ | TBS_NOTICKS, 0, 0, 20, 20, IDC_SLD_AUDMIC);
		Mk(hwnd, "STATIC", "音量: 100%", SS_LEFT, 0, 0, 20, 20, IDC_AUD_STATUS);
		SendMessageA(GetDlgItem(hwnd, IDC_SLD_AUDSYS), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
		SendMessageA(GetDlgItem(hwnd, IDC_SLD_AUDMIC), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	}
	{
		int abw = w - (w / 3) - 8;
		if (abw > 190) abw = 190;
		if (abw < 110) abw = 110;
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_AUDSYS), x0, y0 + 60, abw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SLD_AUDSYS), x0 + abw + 6, y0 + 60, w - abw - 6, 26, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_AUDMIC), x0, y0 + 88, abw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SLD_AUDMIC), x0 + abw + 6, y0 + 88, w - abw - 6, 26, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_AUD_STATUS), x0, y0 + 116, w, 16, TRUE);
		ExtAudioUpdateUI(hwnd);
		ExtShowIds(hwnd, kIdsAud, _countof(kIdsAud), g_ext_page == EXT_PAGE_TOOL);
	}
	MoveWindow(GetDlgItem(hwnd, IDC_CLIP_LBL), x0, y0 + 136, w, 18, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_EDIT_CLIPTXT), x0, y0 + 156, w, 52, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLIPSET), x0, y0 + 214, 140, 26, TRUE);
	MoveWindow(GetDlgItem(hwnd, IDC_CHK_CLIPSYNC), x0 + 148, y0 + 220, w - 148 > 120 ? w - 148 : 120, 20, TRUE);
	/* v10: 画面铺展 / 全屏 控制行 */
	{
		int vbw = (w - 8) / 3;
		if (vbw < 58) vbw = 58;
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_FITFILL),    x0,             y0 + 246, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_FITADAPT),   x0 + vbw + 4,   y0 + 246, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_FULLSCREEN), x0 + (vbw+4)*2, y0 + 246, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_ZOOMOUT),    x0,             y0 + 274, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_ZOOM100),    x0 + vbw + 4,   y0 + 274, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_ZOOMIN),     x0 + (vbw+4)*2, y0 + 274, vbw, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_LBL_VIEWMODE),   x0,             y0 + 302, w, 18, TRUE);
		int vh = h - 330 - 22;
		if (vh < 60) vh = 60;
		MoveWindow(GetDlgItem(hwnd, IDC_TOOL_OUT), x0, y0 + 328, w, vh, TRUE);
	}
	MoveWindow(GetDlgItem(hwnd, IDC_TOOL_TIP), x0, y0 + h - 20, w, 18, TRUE);

	/* 文件页: 行行分开; 操作按钮按面板宽度自动换行, 文字绝不裁剪 */
	{
		int logH = 132;
		if (logH > h / 3) logH = h / 3;
		int r1 = y0;
		int lblW = 44;
		int refW = 64;
		if (refW > w / 4) refW = w / 4;
		MoveWindow(GetDlgItem(hwnd, IDC_LBL_PATH),  x0, r1 + 3, lblW, 18, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDIT_PATH), x0 + lblW, r1, w - lblW - refW - 4, 22, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH), x0 + w - refW, r1, refW, 24, TRUE);

		int ids[6] = { IDC_BTN_UP, IDC_BTN_DOWNLOAD, IDC_BTN_UPLOAD,
		               IDC_BTN_MKDIR, IDC_BTN_RENAME, IDC_BTN_DELETE };
		int gap = 4;
		int per = 6;
		int bw = (w - gap * 5) / 6;
		if (bw < 96)
		{
			per = 3;
			bw = (w - gap * 2) / 3;
		}
		if (bw < 40)
			bw = 40;
		int rows = (6 + per - 1) / per;
		for (int i = 0; i < 6; ++i)
		{
			int bx = x0 + (i % per) * (bw + gap);
			int by = y0 + 30 + (i / per) * 30;
			if (bx + bw > x0 + w)
				bx = x0 + w - bw;
			MoveWindow(GetDlgItem(hwnd, ids[i]), bx, by, bw, 24, TRUE);
		}
		int r3 = y0 + 30 + rows * 30;
		MoveWindow(GetDlgItem(hwnd, IDC_LBL_NAME),  x0, r3 + 3, lblW, 18, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDIT_NAME), x0 + lblW, r3, w - lblW, 22, TRUE);
		int r4 = r3 + 28;
		int listH = h - (r4 - y0) - logH - 6;
		if (listH < 40)
			listH = 40;
		MoveWindow(GetDlgItem(hwnd, IDC_LIST_FILES), x0, r4, w, listH, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDIT_LOG),   x0, r4 + listH + 4, w, logH, TRUE);
	}


	/* 顶部快捷工具条: 独占第二行(视频区上方), 与连接行所有控件彻底分开 */
	{
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_SNAP),    10,  46, 96, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_LOCATE),  110, 46, 96, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLIPGET), 210, 46, 96, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_SYSINFO), 310, 46, 96, 24, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_BTN_CAM),     410, 46, 96, 24, TRUE);
	}
}


/* ---------------- 通知 ---------------- */
static bool ExtOnNotify(HWND hwnd, LPARAM lp)
{
	NMHDR* nh = (NMHDR*)lp;
	if (nh != NULL && nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE)
	{
		int sel = (int)SendMessageA(g_ext_tab, TCM_GETCURSEL, 0, 0);
		ExtSetPage(hwnd, sel);
		return true;
	}
	return false;
}

/* ---------------- 安卓被控端 APK 一键生成 ---------------- */
/* APK 数据内嵌在 android_apk_data.h(由 temp\android\build.py + embed_apk.py 生成)。
   若 exe 同目录放有 RCAndroidAgent.apk(更新的构建), 会优先用外部文件, 便于热替换。 */
static bool ExtWriteAndroidApk(const char* path, char* errOut, size_t errSz)
{
	const char* src = NULL;
	unsigned int len = 0;

	/* exe 同目录若放有 RCAndroidAgent.apk, 优先用它(便于热替换) */
	char side[MAX_PATH] = { 0 };
	{
		char self[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, self, MAX_PATH);
		char* p = strrchr(self, '\\');
		if (p != NULL)
		{
			*(p + 1) = 0;
			strncpy_s(side, self, _TRUNCATE);
			strncat_s(side, "RCAndroidAgent.apk", _TRUNCATE);
		}
	}

	FILE* fp = NULL;
	if (fopen_s(&fp, path, "wb") != 0 || fp == NULL)
	{
		_snprintf_s(errOut, errSz, _TRUNCATE, "无法创建输出文件(路径只读或被占用?)");
		return false;
	}
	if (side[0] != 0)
	{
		FILE* fs = NULL;
		if (fopen_s(&fs, side, "rb") == 0 && fs != NULL)
		{
			char buf[65536];
			size_t n = 0;
			while ((n = fread(buf, 1, sizeof(buf), fs)) > 0)
			{
				if (fwrite(buf, 1, n, fp) != n)
				{
					fclose(fs); fclose(fp);
					_snprintf_s(errOut, errSz, _TRUNCATE, "写入失败(磁盘空间不足?)");
					return false;
				}
				len += (unsigned int)n;
			}
			fclose(fs);
			fclose(fp);
			if (len > 0)
				return true;
			fp = NULL;
			if (fopen_s(&fp, path, "wb") != 0 || fp == NULL)
			{
				_snprintf_s(errOut, errSz, _TRUNCATE, "无法创建输出文件");
				return false;
			}
		}
	}

	src = (const char*)kAndroidApk;
	unsigned int written = (unsigned int)fwrite(src, 1, kAndroidApkSize, fp);
	fclose(fp);
	if (written != kAndroidApkSize)
	{
		_snprintf_s(errOut, errSz, _TRUNCATE, "写入不完整: %u/%u 字节", written, kAndroidApkSize);
		return false;
	}
	return true;
}

static void ExtGenerateAndroidApk(HWND hwnd)
{
	char file[MAX_PATH] = { 0 };
	strncpy_s(file, "RCAndroidAgent.apk", _TRUNCATE);

	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = (hwnd != NULL) ? hwnd : g_hwnd;
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrFilter = "安卓安装包 (*.apk)\0*.apk\0所有文件\0*.*\0";
	ofn.lpstrTitle = "生成安卓被控端 APK — 选择保存位置";
	ofn.lpstrDefExt = "apk";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameA(&ofn))
		return;

	char err[256] = { 0 };
	if (!ExtWriteAndroidApk(file, err, sizeof(err)))
	{
		ConPrint("  生成安卓被控端失败: %s", err);
		MessageBoxA(ofn.hwndOwner, err, "生成失败", MB_OK | MB_ICONERROR);
		return;
	}

	char msg[1024];
	_snprintf_s(msg, _TRUNCATE,
		"已生成安卓被控端 APK:\n%s\n\n"
		"手机端使用步骤:\n"
		"  1) 把 APK 装到安卓手机/平板(允许“未知来源安装”)\n"
		"  2) 手机与本机连同一网络; 打开 App, 服务器地址填 %s:%d\n"
		"  3) 点“授权屏幕采集并启动服务”, 在系统弹窗中允许录屏\n"
		"  4) 要远程触摸/按键: 打开系统设置里本 App 的无障碍服务\n"
		"  5) 回到本控制端点“连接”, 即可看到手机画面并操作\n",
		file, g_ip, g_port);
	ConPrint("  已生成安卓被控端 APK: %s", file);
	MessageBoxA(ofn.hwndOwner, msg, "生成成功", MB_OK | MB_ICONINFORMATION);
}

/* ---------------- 按钮/命令 ---------------- */
static bool ExtOnCommand(HWND hwnd, int id, int code)
{
	switch (id)

	{

	case IDC_BTN_AUDSYS:  ExtAudioToggle(&g_sndSys, hwnd); return true;

	case IDC_BTN_AUDMIC:  ExtAudioToggle(&g_sndMic, hwnd); return true;

	case IDC_BTN_SNAP:    ExtRunCommand("shot");     return true;

	case IDC_BTN_LOCATE:  ExtRunCommand("locate");   return true;

	case IDC_BTN_CLIPGET: ExtRunCommand("clip get"); return true;

	case IDC_BTN_SYSINFO: ExtRunCommand("sysinfo");  return true;

	case IDC_BTN_CAM:     ExtCamToggle(hwnd);        return true;

	case IDC_BTN_GENAPK:  ExtGenerateAndroidApk(hwnd); return true;

	default: break;

	}



	if (code != BN_CLICKED && code != 0)
		return false;
	switch (id)
	{
	case IDC_BTN_RUN:
	{
		HWND ed = GetDlgItem(hwnd, IDC_CMD_IN);
		char buf[2048] = { 0 };
		if (ed != NULL)
			GetWindowTextA(ed, buf, sizeof(buf) - 1);
		SetWindowTextA(ed, "");
		std::string line = ExtTrim(std::string(buf));
		if (!line.empty())
			ExtRunCommand(line);
		SetFocus(ed);
		return true;
	}
	case IDC_BTN_HELP:
		ExtRunCommand("help");
		return true;
	case IDC_BTN_CLEAR:
		ConClear();
		return true;

	case IDC_BTN_SNAPSAVE:
		ExtSaveShot("");
		return true;
	case IDC_BTN_TOOLLOCATE:
		ExtRunCommand("locate");
		return true;
	case IDC_BTN_TOOLSYSINFO:
		ExtRunCommand("sysinfo");
		return true;
	case IDC_BTN_TOOLCLIPGET:
		ExtRunCommand("clip get");
		return true;
	case IDC_BTN_CLIPSET:
	{
		HWND ed = GetDlgItem(hwnd, IDC_EDIT_CLIPTXT);
		if (ed == NULL)
			return true;
		int len = GetWindowTextLengthA(ed);
		if (len < 0) len = 0;
		std::vector<char> v((size_t)len + 1, 0);
		GetWindowTextA(ed, &v[0], len + 1);
		if (!SendClipboard(1, std::string(&v[0])))
			ConPrint("  写入剪贴板失败: 未连接被控端");
		else
			ConPrint("  已写入被控端剪贴板: %d 字符", len);
		return true;
	}
	case IDC_BTN_FITFILL:      /* v10: 画面铺满窗口(拉伸) */
		ExtSetFitMode(hwnd, 1, 0);
		return true;
	case IDC_BTN_FITADAPT:     /* v10: 画面适应窗口(等比) */
		ExtSetFitMode(hwnd, 0, 0);
		return true;
	case IDC_BTN_ZOOMIN:       /* v10: 画面放大 +10% */
		ExtSetFitMode(hwnd, -1, +10);
		return true;
	case IDC_BTN_ZOOMOUT:      /* v10: 画面缩小 -10% */
		ExtSetFitMode(hwnd, -1, -10);
		return true;
	case IDC_BTN_ZOOM100:      /* v10: 原始尺寸 1:1 */
		ExtSetFitMode(hwnd, 2, 0);
		return true;
	case IDC_BTN_FULLSCREEN:   /* v10: 全屏切换 */
		ExtSetFullScreen(hwnd, InterlockedCompareExchange(&g_fullscreen, 0, 0) == 0);
		return true;
	case IDC_CHK_CLIPSYNC:   /* v9: 剪贴板自动同步开关 */
	{
		const BOOL on = (SendMessageA(GetDlgItem(hwnd, IDC_CHK_CLIPSYNC), BM_GETCHECK, 0, 0) == BST_CHECKED);
		InterlockedExchange(&g_cclip_auto, on ? 1 : 0);
		if (on)
			g_cclip_last_seq = GetClipboardSequenceNumber();
		ConPrint("  剪贴板自动同步: %s", on ? "开 (双向: 文本/图片/文件)" : "关");
		if (g_connected)
		{
			ClipMsg hm;
			memset(&hm, 0, sizeof(hm));
			hm.msg = RC_CM_HELLO;
			hm.fmt = RC_CF_TEXT | RC_CF_UNICODE | RC_CF_PNG | RC_CF_FILES | RC_CF_DIB;
			hm.flags = on ? 1 : 0;
			SendCmdPacket(CMD_CLIP_CHANNEL, (char*)&hm, sizeof(hm));
			if (on)
				CClipOfferSelf();
		}
		return true;
	}
	case IDC_BTN_CLIPCLR:
		SendClipboard(2, "");
		ConPrint("  已请求清空被控端剪贴板");
		return true;

	case IDC_BTN_RECSTART:
	{
		char sb[32] = { 0 };
		GetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_RECSEC), sb, sizeof(sb) - 1);
		int sec = atoi(sb);
		int flags = 0;
		if (SendMessageA(GetDlgItem(hwnd, IDC_CHK_MOUSE), BM_GETCHECK, 0, 0) == BST_CHECKED)
			flags |= RC_REC_MOUSE;
		if (SendMessageA(GetDlgItem(hwnd, IDC_CHK_KEY), BM_GETCHECK, 0, 0) == BST_CHECKED)
			flags |= RC_REC_KEY;
		if (flags == 0)
		{
			ConPrint("  请至少勾选“鼠标”或“键盘”");
			return true;
		}
		ExtStartRecord(sec, flags);
		return true;
	}
	case IDC_BTN_RECSTOP:
		ExtStopRecord();
		return true;
	case IDC_BTN_RECPLAY:
		ExtReplayStart();
		return true;
	case IDC_BTN_RECSAVE:
	{
		if (g_rec_text.empty())
		{
			ConPrint("  没有可保存的录制数据");
			return true;
		}
		char dir[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, dir, MAX_PATH - 1);
		std::string path(dir);
		size_t k = path.find_last_of("\\/");
		if (k != std::string::npos)
			path = path.substr(0, k + 1);
		SYSTEMTIME st;
		GetLocalTime(&st);
		char nm[128];
		_snprintf_s(nm, _TRUNCATE, "rc_rec_%04d%02d%02d_%02d%02d%02d.txt",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		path += nm;
		if (ExtWriteFile(path, g_rec_text))
			ConPrint("  录制已保存: %s", path.c_str());
		else
			ConPrint("  保存失败: %s", path.c_str());
		return true;
	}
	case IDC_BTN_RECCLEAR:
		g_rec_text.clear();
		SetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_RECOUT), "");
		SetWindowTextA(GetDlgItem(hwnd, IDC_REC_STATUS), "状态: 空闲");
		ConPrint("  录制数据已清空");
		return true;
	}
	return false;
}

/* ---------------- 回包文本解析: 跳过固定头部结构 ---------------- */
static std::string ExtTextAfterHead(const char* body, int len, int headSize)
{
	if (len <= headSize)
		return std::string();
	return std::string(body + headSize, (size_t)(len - headSize));
}

/* ---------------- 控制台文本刷新到控件 ---------------- */
static void ExtConsoleRefresh(HWND hwnd)
{
	std::string all;
	ConAllToString(all);
	HWND ed = GetDlgItem(hwnd, IDC_CMD_OUT);
	if (ed != NULL)
	{
		SetWindowTextA(ed, all.c_str());
		SendMessageA(ed, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
		SendMessageA(ed, EM_SCROLLCARET, 0, 0);
	}
	HWND ed2 = GetDlgItem(hwnd, IDC_TOOL_OUT);
	if (ed2 != NULL)
	{
		std::string tail = all;
		if (tail.size() > 2000)
			tail = tail.substr(tail.size() - 2000);
		SetWindowTextA(ed2, tail.c_str());
		SendMessageA(ed2, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
		SendMessageA(ed2, EM_SCROLLCARET, 0, 0);
	}
}

/* ---------------- 键盘鼠标录制回包 ---------------- */
static bool ExtOnInputLog(const char* body, int len)
{
	if (len < (int)sizeof(InputLog))
		return true;
	InputLog h;
	memcpy(&h, body, sizeof(h));
	std::string text = ExtTextAfterHead(body, len, (int)sizeof(InputLog));
	g_rec_text += text;
	if (h.last != 0)
	{
		g_rec_expect = false;
		HWND h2 = g_hwnd;
		if (h2 != NULL)
		{
			SetWindowTextA(GetDlgItem(h2, IDC_EDIT_RECOUT), g_rec_text.c_str());
			char st[128];
			_snprintf_s(st, _TRUNCATE, "状态: 已完成 (%d 字节)", (int)g_rec_text.size());
			SetWindowTextA(GetDlgItem(h2, IDC_REC_STATUS), st);
		}
		ConPrint("  录制完成，共接收 %d 字节", (int)g_rec_text.size());
		int n = 0;
		for (size_t i = 0; i < g_rec_text.size(); ++i)
			if (g_rec_text[i] == '\n')
				++n;
		ConPrint("  数据已显示在“键鼠录制”页，可用“回放”或“保存文件”");
		(void)n;
	}
	else
	{
		if (g_hwnd != NULL)
		{
			char st[128];
			_snprintf_s(st, _TRUNCATE, "状态: 接收中 (%d 字节)", (int)g_rec_text.size());
			SetWindowTextA(GetDlgItem(g_hwnd, IDC_REC_STATUS), st);
		}
	}
	return true;
}

/* ---------------- 统一回包处理(返回 true 表示已处理) ---------------- */
/* ============================================================
 *  v9: 剪贴板同步虚拟通道(控制端, 与被控端对称)
 *  延迟渲染: 本端剪贴板变化 → OFFER(仅描述) → 对端 REQUEST → DATA 分块
 * ============================================================ */
/* 注意: 状态变量/开关定义见文件前部(kIdsFiles 之前), 以便 UI 命令回调可访问 */

static std::wstring CClipUtf8ToWide(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
	std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string CClipWideToUtf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
	std::string s((size_t)(n > 0 ? n : 0), '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
	return s;
}

static void CClipTouchSeq()
{
	g_cclip_last_seq = GetClipboardSequenceNumber();
}

static std::string CClipBaseName(const std::string& p)
{
	size_t k = p.find_last_of('\\');
	size_t k2 = p.find_last_of('/');
	if (k2 != std::string::npos && (k == std::string::npos || k2 > k)) k = k2;
	return (k == std::string::npos) ? p : p.substr(k + 1);
}

static void CClipEnsureDir(const std::string& d)
{
	std::wstring w = CClipUtf8ToWide(d);
	for (size_t i = 0; i < w.size(); i++)
	{
		if (w[i] == (wchar_t)92 || w[i] == (wchar_t)47)
		{
			if (i > 2) CreateDirectoryW(w.substr(0, i).c_str(), NULL);
		}
	}
	CreateDirectoryW(w.c_str(), NULL);
}

/* ---- 读本机剪贴板(延迟渲染: 只取描述/清单, 不预传内容) ---- */
static bool CClipLocalText(std::string& utf8)
{
	utf8.clear();
	if (!OpenClipboard(NULL)) return false;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h != NULL)
	{
		const wchar_t* w = (const wchar_t*)GlobalLock(h);
		if (w != NULL) { utf8 = CClipWideToUtf8(w); GlobalUnlock(h); }
	}
	CloseClipboard();
	return !utf8.empty();
}

static bool CClipLocalFiles(std::vector<std::string>& paths, std::vector<ClipFileDesc>& desc)
{
	paths.clear();
	desc.clear();
	if (!OpenClipboard(NULL)) return false;
	HDROP dp = (HDROP)GetClipboardData(CF_HDROP);
	if (dp == NULL) { CloseClipboard(); return false; }
	UINT n = DragQueryFileW(dp, 0xFFFFFFFF, NULL, 0);
	for (UINT i = 0; i < n && i < 256; i++)
	{
		wchar_t w[MAX_PATH + 1] = { 0 };
		if (DragQueryFileW(dp, i, w, MAX_PATH) == 0) continue;
		std::string u8 = CClipWideToUtf8(w);
		if (u8.empty() || u8.size() >= sizeof(ClipFileDesc::name)) continue;
		ClipFileDesc d;
		memset(&d, 0, sizeof(d));
		WIN32_FILE_ATTRIBUTE_DATA fa;
		memset(&fa, 0, sizeof(fa));
		if (GetFileAttributesExW(w, GetFileExInfoStandard, &fa))
		{
			d.size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
			d.is_dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
			ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
			d.mtime = (unsigned int)(t / 10000000ULL - 11644473600ULL);
		}
		strncpy_s(d.name, u8.c_str(), _TRUNCATE);
		paths.push_back(u8);
		desc.push_back(d);
	}
	CloseClipboard();
	return !paths.empty();
}

static bool CClipLocalPng(std::string& png)
{
	png.clear();
	UINT cfPng = RegisterClipboardFormatA("PNG");
	if (!OpenClipboard(NULL)) return false;
	bool ok = false;
	if (cfPng != 0)
	{
		HANDLE h = GetClipboardData(cfPng);
		if (h != NULL)
		{
			const void* p = GlobalLock(h);
			if (p != NULL)
			{
				SIZE_T sz = GlobalSize(h);
				if (sz > 8 && sz <= RC_CLIP_MSG_MAX) { png.assign((const char*)p, sz); ok = true; }
				GlobalUnlock(h);
			}
		}
	}
	if (!ok)
	{
		HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP);
		if (hb != NULL)
		{
			HBITMAP hc = (HBITMAP)CopyImage(hb, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
			if (hc != NULL)
			{
				CImage img;
				img.Attach(hc);
				IStream* st = NULL;
				if (CreateStreamOnHGlobal(NULL, TRUE, &st) == S_OK)
				{
					if (SUCCEEDED(img.Save(st, Gdiplus::ImageFormatPNG)))
					{
						STATSTG ss;
						memset(&ss, 0, sizeof(ss));
						if (SUCCEEDED(st->Stat(&ss, STATFLAG_NONAME)) && ss.cbSize.QuadPart > 8 &&
							ss.cbSize.QuadPart <= RC_CLIP_MSG_MAX)
						{
							LARGE_INTEGER z;
							z.QuadPart = 0;
							st->Seek(z, STREAM_SEEK_SET, NULL);
							png.resize((size_t)ss.cbSize.QuadPart);
							ULONG rd = 0;
							st->Read(&png[0], (ULONG)png.size(), &rd);
							png.resize(rd);
							ok = !png.empty();
						}
					}
					st->Release();
				}
				img.Destroy();
			}
		}
	}
	CloseClipboard();
	return ok;
}

/* ---- 通道发送 / 主动上报 ---- */
static bool CClipSendRaw(int msg, int fmt, unsigned int seq, unsigned int offset, unsigned int size,
	int last, int count, const void* extra, int extraLen)
{
	ClipMsg m;
	memset(&m, 0, sizeof(m));
	m.msg = msg;
	m.fmt = fmt;
	m.seq = seq;
	m.offset = offset;
	m.size = size;
	m.last = last;
	m.count = count;
	if (extraLen < 0) extraLen = 0;
	std::string body((const char*)&m, sizeof(m));
	if (extra != NULL && extraLen > 0)
		body.append((const char*)extra, (size_t)extraLen);
	return SendCmdPacket(CMD_CLIP_CHANNEL, body.data(), (int)body.size());
}

static void CClipOfferSelf()
{
	std::vector<std::string> files;
	std::vector<ClipFileDesc> desc;
	int fmt = 0;
	unsigned int size = 0;
	int count = 0;
	if (CClipLocalFiles(files, desc))
	{
		fmt = RC_CF_FILES;
		count = (int)desc.size();
	}
	else
	{
		std::string png, text;
		if (CClipLocalPng(png)) { fmt = RC_CF_PNG; size = (unsigned int)png.size(); }
		else if (CClipLocalText(text)) { fmt = RC_CF_TEXT | RC_CF_UNICODE; size = (unsigned int)text.size(); }
	}
	if (fmt == 0) return;
	EnterCriticalSection(&g_cclip_lock);
	g_cclip_local_files = files;
	LeaveCriticalSection(&g_cclip_lock);
	CClipSendRaw(RC_CM_OFFER, fmt, 0, 0, size, 0, count,
		desc.empty() ? NULL : (const void*)desc.data(), (int)(desc.size() * sizeof(ClipFileDesc)));
	LogLine("clip: 本机剪贴板已变化 -> 已通知被控端 (fmt=0x%x, %d 项)", fmt, count > 0 ? count : 1);
}

/* ---- 写本机剪贴板(对端 -> 本机) ---- */
#pragma pack(push,1)
struct CClipDropFiles
{
	DWORD pFiles;
	POINT pt;
	BOOL  fNC;
	BOOL  fWide;
};
#pragma pack(pop)

static void CClipApplyText(const std::string& utf8)
{
	if (utf8.empty()) return;
	std::wstring w = CClipUtf8ToWide(utf8);
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
	CClipTouchSeq();
	LogLine("clip: 本机剪贴板 <- 对端文本 (%d 字节 UTF-8)", (int)utf8.size());
}

static void CClipApplyFiles(const std::vector<std::string>& paths)
{
	if (paths.empty()) return;
	std::wstring list;
	for (size_t i = 0; i < paths.size(); i++)
	{
		list += CClipUtf8ToWide(paths[i]);
		list.push_back(L'\0');
	}
	size_t bytes = sizeof(CClipDropFiles) + (list.size() + 1) * sizeof(wchar_t);
	HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
	if (hg == NULL) return;
	void* p = GlobalLock(hg);
	if (p == NULL) { GlobalFree(hg); return; }
	CClipDropFiles* df = (CClipDropFiles*)p;
	df->pFiles = sizeof(CClipDropFiles);
	df->fWide = TRUE;
	memcpy((char*)p + sizeof(CClipDropFiles), list.c_str(), (list.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(hg);
	if (!OpenClipboard(NULL)) { GlobalFree(hg); return; }
	EmptyClipboard();
	if (SetClipboardData(CF_HDROP, hg) == NULL) GlobalFree(hg);
	CloseClipboard();
	CClipTouchSeq();
	LogLine("clip: 本机剪贴板 <- 对端文件清单 (%d 项, 本地实体文件)", (int)paths.size());
}

static void CClipApplyPng(const std::string& png)
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
		LARGE_INTEGER li;
		li.QuadPart = 0;
		st->Seek(li, STREAM_SEEK_SET, NULL);
		CImage img;
		if (SUCCEEDED(img.Load(st)))
		{
			hbmp = img.Detach();
			BITMAP bm;
			memset(&bm, 0, sizeof(bm));
			if (hbmp != NULL && GetObject(hbmp, sizeof(bm), &bm) == sizeof(bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
			{
				BITMAPINFOHEADER bh;
				memset(&bh, 0, sizeof(bh));
				bh.biSize = sizeof(bh);
				bh.biWidth = bm.bmWidth;
				bh.biHeight = -bm.bmHeight;
				bh.biPlanes = 1;
				bh.biBitCount = 32;
				bh.biCompression = BI_RGB;
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
	CClipTouchSeq();
	LogLine("clip: 本机剪贴板 <- 对端位图 (%d 字节 PNG)", (int)png.size());
}

/* ---- 接收(对端 -> 本机)状态 ---- */
static bool g_cclip_in_active = false;
static int  g_cclip_in_idx = 0;
static int  g_cclip_in_count = 0;
static FILE* g_cclip_in_fp = NULL;
static unsigned long long g_cclip_in_got = 0;
static std::string g_cclip_in_dir;
static std::vector<std::string> g_cclip_in_paths;
static std::vector<ClipFileDesc> g_cclip_in_desc;
static std::string g_cclip_send_text;
static std::string g_cclip_send_png;

static void CClipInReset()
{
	if (g_cclip_in_fp != NULL) { fclose(g_cclip_in_fp); g_cclip_in_fp = NULL; }
	g_cclip_in_active = false;
	g_cclip_in_idx = 0;
	g_cclip_in_count = 0;
	g_cclip_in_got = 0;
	g_cclip_in_paths.clear();
	g_cclip_in_desc.clear();
}

static void CClipBeginRecvFiles()
{
	std::vector<ClipFileDesc> desc;
	EnterCriticalSection(&g_cclip_lock);
	desc = g_cclip_peer_files;
	LeaveCriticalSection(&g_cclip_lock);
	if (desc.empty()) return;
	char tmp[MAX_PATH + 1] = { 0 };
	if (GetTempPathA(MAX_PATH, tmp) == 0) return;
	char sub[64] = { 0 };
	_snprintf_s(sub, sizeof(sub), _TRUNCATE, "rc_clip_sync\\%u", (unsigned int)GetTickCount());
	std::string dir = std::string(tmp) + sub;
	CClipEnsureDir(dir);
	CClipInReset();
	g_cclip_in_desc = desc;
	g_cclip_in_dir = dir;
	g_cclip_in_count = (int)desc.size();
	g_cclip_in_active = true;
	InterlockedExchange(&g_cclip_hold, 1);
	CClipSendRaw(RC_CM_REQUEST, RC_CF_FILES, 0, 0, 0, 0, 0, NULL, 0);
	LogLine("clip: 接收对端 %d 个文件 -> %s", g_cclip_in_count, dir.c_str());
}

static void CClipHandleRecvData(ClipMsg& m, const char* extra, int extraLen)
{
	if (g_cclip_in_active)
	{
		if ((int)m.seq != g_cclip_in_idx)
		{
			LogLine("clip: 数据序号不符 (seq=%u, 期望 %d)", m.seq, g_cclip_in_idx);
			return;
		}
		if (m.offset == 0)
		{
			if (g_cclip_in_fp != NULL) { fclose(g_cclip_in_fp); g_cclip_in_fp = NULL; }
			if (g_cclip_in_idx < (int)g_cclip_in_desc.size())
			{
				std::string full = g_cclip_in_dir + "\\" + CClipBaseName(g_cclip_in_desc[(size_t)g_cclip_in_idx].name);
				if (g_cclip_in_desc[(size_t)g_cclip_in_idx].is_dir)
				{
					std::wstring w = CClipUtf8ToWide(full);
					CreateDirectoryW(w.c_str(), NULL);
				}
				else
				{
					fopen_s(&g_cclip_in_fp, full.c_str(), "wb");
					if (g_cclip_in_fp != NULL) g_cclip_in_paths.push_back(full);
				}
			}
			g_cclip_in_got = 0;
		}
		if (extraLen > 0 && g_cclip_in_fp != NULL)
		{
			fwrite(extra, 1, (size_t)extraLen, g_cclip_in_fp);
			g_cclip_in_got += (unsigned long long)extraLen;
		}
		if (m.last)
		{
			if (g_cclip_in_fp != NULL) { fclose(g_cclip_in_fp); g_cclip_in_fp = NULL; }
			g_cclip_in_idx++;
			if (g_cclip_in_idx >= g_cclip_in_count)
			{
				std::vector<std::string> paths = g_cclip_in_paths;
				const int got = g_cclip_in_idx;
				CClipInReset();
				CClipApplyFiles(paths);
				InterlockedExchange(&g_cclip_hold, 0);
				CClipSendRaw(RC_CM_UNLOCK, RC_CF_FILES, 0, 0, 0, 0, 0, NULL, 0);
				LogLine("clip: 已接收 %d 个文件, 本地剪贴板就绪(资源管理器 Ctrl+V 即可粘贴)", got);
				PostStatus("远程文件已同步到本地剪贴板(可直接粘贴)");
			}
			else
			{
				CClipSendRaw(RC_CM_REQUEST, RC_CF_FILES, (unsigned int)g_cclip_in_idx, 0, 0, 0, 0, NULL, 0);
			}
		}
		return;
	}
	if (extraLen <= 0) return;
	std::string data(extra, (size_t)extraLen);
	if (m.fmt == RC_CF_PNG) CClipApplyPng(data);
	else CClipApplyText(data);
}

/* ---- 按需发送本端内容(响应对端 REQUEST) ---- */
static void CClipSendBuf(int fmt, unsigned int seq, const char* data, size_t size)
{
	if (size == 0)
	{
		CClipSendRaw(RC_CM_DATA, fmt, seq, 0, 0, 1, 0, NULL, 0);
		return;
	}
	size_t off = 0;
	while (off < size)
	{
		size_t n = size - off;
		if (n > RC_CCLIP_CHUNK) n = RC_CCLIP_CHUNK;
		const bool last = (off + n >= size);
		CClipSendRaw(RC_CM_DATA, fmt, seq, (unsigned int)off, (unsigned int)n, last ? 1 : 0, 0, data + off, (int)n);
		off += n;
	}
}

static void CClipSendLocalContent(int fmt, unsigned int seq)
{
	if (fmt & RC_CF_FILES)
	{
		std::vector<std::string> files;
		EnterCriticalSection(&g_cclip_lock);
		files = g_cclip_local_files;
		LeaveCriticalSection(&g_cclip_lock);
		if (seq >= files.size())
		{
			CClipSendRaw(RC_CM_ABORT, RC_CF_FILES, seq, 0, 0, 0, 0, NULL, 0);
			return;
		}
		std::wstring w = CClipUtf8ToWide(files[seq]);
		FILE* fp = NULL;
		_wfopen_s(&fp, w.c_str(), L"rb");
		if (fp == NULL)
		{
			CClipSendRaw(RC_CM_DATA, RC_CF_FILES, seq, 0, 0, 1, 0, NULL, 0);   /* 目录/无法打开: 空文件占位 */
			return;
		}
		std::vector<char> buf(RC_CCLIP_CHUNK);
		unsigned int off = 0;
		for (;;)
		{
			size_t rd = fread(buf.data(), 1, buf.size(), fp);
			if (rd == 0)
			{
				CClipSendRaw(RC_CM_DATA, RC_CF_FILES, seq, off, 0, 1, 0, NULL, 0);
				break;
			}
			CClipSendRaw(RC_CM_DATA, RC_CF_FILES, seq, off, (unsigned int)rd, 0, 0, buf.data(), (int)rd);
			off += (unsigned int)rd;
			if (rd < buf.size())
			{
				CClipSendRaw(RC_CM_DATA, RC_CF_FILES, seq, off, 0, 1, 0, NULL, 0);
				break;
			}
		}
		fclose(fp);
		return;
	}
	if (fmt & RC_CF_PNG)
	{
		std::string png;
		if (CClipLocalPng(png) && !png.empty())
			CClipSendBuf(RC_CF_PNG, 0, png.data(), png.size());
		else
			CClipSendRaw(RC_CM_ABORT, RC_CF_PNG, 0, 0, 0, 0, 0, NULL, 0);
		return;
	}
	std::string text;
	if (CClipLocalText(text) && !text.empty())
		CClipSendBuf(RC_CF_TEXT, 0, text.data(), text.size());
	else
		CClipSendRaw(RC_CM_ABORT, RC_CF_TEXT, 0, 0, 0, 0, 0, NULL, 0);
}

static void CClipHandleChannel(Packet* p)
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
		g_cclip_peer_fmt = m.fmt;
		InterlockedExchange(&g_cclip_ready, 1);
		LogLine("clip: 剪贴板通道就绪(对端支持 0x%x)", m.fmt);
		break;

	case RC_CM_OFFER:
	{
		EnterCriticalSection(&g_cclip_lock);
		g_cclip_peer_offer = m;
		g_cclip_peer_files.clear();
		if ((m.fmt & RC_CF_FILES) && m.count > 0 && extraLen >= (int)sizeof(ClipFileDesc))
		{
			int n = m.count;
			int mx = (int)((size_t)extraLen / sizeof(ClipFileDesc));
			if (n > mx) n = mx;
			const ClipFileDesc* d = (const ClipFileDesc*)extra;
			for (int i = 0; i < n; i++)
				g_cclip_peer_files.push_back(d[i]);
		}
		const bool hasFiles = !g_cclip_peer_files.empty();
		const int  fileCount = (int)g_cclip_peer_files.size();
		LeaveCriticalSection(&g_cclip_lock);
		if (hasFiles)
		{
			LogLine("clip: 对端剪贴板含 %d 个文件 -> 延迟渲染接收中", fileCount);
			CClipBeginRecvFiles();
		}
		else if (m.fmt & (RC_CF_PNG | RC_CF_TEXT))
		{
			CClipSendRaw(RC_CM_REQUEST, m.fmt, 0, 0, 0, 0, 0, NULL, 0);
		}
		else
		{
			LogLine("clip: 对端剪贴板格式不支持 (0x%x)", m.fmt);
		}
		break;
	}

	case RC_CM_REQUEST:
		LogLine("clip: 对端请求本端内容 (fmt=0x%x, seq=%u)", m.fmt, m.seq);
		CClipSendLocalContent(m.fmt, m.seq);
		break;

	case RC_CM_DATA:
		CClipHandleRecvData(m, extra, extraLen);
		break;

	case RC_CM_LOCK:
		InterlockedExchange(&g_cclip_hold, 1);
		break;

	case RC_CM_UNLOCK:
		InterlockedExchange(&g_cclip_hold, 0);
		break;

	case RC_CM_ABORT:
		if (g_cclip_in_active)
		{
			CClipInReset();
			InterlockedExchange(&g_cclip_hold, 0);
			LogLine("clip: 对端中止了文件传输");
		}
		break;

	case RC_CM_PASTE:
	{
		/* 对端要求在本机执行粘贴(远程 Ctrl+V) */
		HWND fg = GetForegroundWindow();
		if (fg != NULL)
		{
			PostMessageA(fg, WM_KEYDOWN, VK_CONTROL, 0);
			PostMessageA(fg, WM_KEYDOWN, 'V', 0);
			PostMessageA(fg, WM_KEYUP, 'V', 0);
			PostMessageA(fg, WM_KEYUP, VK_CONTROL, 0);
		}
		break;
	}

	default:
		LogLine("clip: 未知通道消息 %d", m.msg);
		break;
	}
}

static bool ExtOnPacket(Packet* p)
{
	if (p == NULL)
		return false;
	const char* body = p->body;
	int len = p->header.body_len;

	switch (p->header.cmd)
	{
	case CMD_CLIP_CHANNEL:   /* v9: 剪贴板同步虚拟通道(文本/位图/文件延迟渲染) */
		CClipHandleChannel(p);
		return true;

	case CMD_CAMERA_FRAME:   /* 被控端摄像头 JPEG 帧 -> 交给独立小窗口解码显示 */
	{
		if (len < (int)sizeof(CameraFrameHead)) return true;
		CameraFrameHead ch;
		memcpy(&ch, body, sizeof(ch));
		int jl = len - (int)sizeof(CameraFrameHead);
		if (ch.bytes > 0 && ch.bytes < jl) jl = ch.bytes;
		if (jl > 0)
		{
			ExtCamLock();
			EnterCriticalSection(&g_cam_lock);
			g_cam_jpg.assign(body + sizeof(CameraFrameHead), body + sizeof(CameraFrameHead) + jl);
			g_cam_frm_w = ch.w;
			g_cam_frm_h = ch.h;
			LeaveCriticalSection(&g_cam_lock);
			if (g_cam_wnd != NULL) { InterlockedIncrement(&g_cam_pending); PostMessageA(g_cam_wnd, WM_APP_CAMFRAME, 0, 0); }
		}
		return true;
	}
	case CMD_CAMERA_RES:     /* 摄像头状态/错误文本 */
	{
		if (len <= 0) return true;
		std::string txt(body, (size_t)len);
		while (!txt.empty())
		{
			const char c = txt[txt.size() - 1];
			if (c == 13 || c == 10 || c == 0 || c == ' ') txt.erase(txt.size() - 1);
			else break;
		}
		if (!txt.empty())
		{
			_snprintf_s(g_cam_status, _TRUNCATE, "%s", txt.c_str());
			/* 被控端不接受请求分辨率时, 自动退回摄像头原生分辨率重试一次 */
			if (g_cam_reqW > 0 && txt.find("协商失败") != std::string::npos && !g_cam_fallback_tried)
			{
				g_cam_fallback_tried = true;
				g_cam_reqW = 0; g_cam_reqH = 0;
				ExtCamSend(RC_CAM_START, 0, 0, 0, g_cam_reqFps, g_cam_reqQ);
				ConPrint("  摄像头: 请求分辨率不被支持, 已自动改用原生分辨率");
			}
			ConPrint("  摄像头: %s", txt.c_str());
			ExtCamSetTitle();
			if (txt.find("fail") != std::string::npos || txt.find("error") != std::string::npos ||
				txt.find("失败") != std::string::npos || txt.find("没有") != std::string::npos)
				InterlockedExchange((volatile LONG*)&g_cam_on, 0);
		}
		return true;
	}
	case CMD_AUDIO_FRAME:    /* 系统声音 PCM */
	case CMD_MIC_FRAME:      /* 麦克风 PCM */
	{
		bool isMic = (p->header.cmd == CMD_MIC_FRAME);
		SndOut* P = isMic ? SndMic() : SndSys();
		SndInit(P, isMic ? "mic" : "audio", isMic ? "麦克风(环境声)" : "系统声音",
			isMic ? CMD_MIC : CMD_AUDIO, isMic ? CMD_MIC_RES : CMD_AUDIO_RES);
		if (len < (int)sizeof(AudioFrameHead)) return true;
		AudioFrameHead ah;
		memcpy(&ah, body, sizeof(ah));
		int pcm = len - (int)sizeof(ah);
		if (ah.bytes > 0 && ah.bytes < pcm) pcm = ah.bytes;
		if ((ah.flags & RC_AF_FIRST) && P->on)
			ConPrint("  %s回传开始: %d Hz / %d ch / %d bit, 每包 %d 字节", P->label, ah.rate, ah.ch, ah.bits, ah.bytes);
		SndPlay(P, body + sizeof(ah), pcm, ah.rate, ah.ch);
		return true;
	}

	case CMD_AUDIO_RES:      /* 声音状态文本 */
	case CMD_MIC_RES:        /* 麦克风状态文本 */
	{
		const bool isMicRes = (p->header.cmd == CMD_MIC_RES);
		std::string txt;
		if (len > 0) txt.assign(body, (size_t)len);
		while (!txt.empty() && (txt[txt.size() - 1] == '\r' || txt[txt.size() - 1] == '\n' || txt[txt.size() - 1] == 0)) txt.erase(txt.size() - 1);
		if (!txt.empty()) ConPrint("  %s: %s", isMicRes ? "麦克风" : "声音", txt.c_str());
		return true;
	}

	case CMD_HELLO_RES:      /* 被控端版本/能力信息 */
	{
		HelloInfo hi;
		memset(&hi, 0, sizeof(hi));
		const int n = (len < (int)sizeof(hi)) ? len : (int)sizeof(hi);
		if (n > 0)
			memcpy(&hi, body, (size_t)n);
		if (hi.build[0] == 0)
			strcpy_s(hi.build, sizeof(hi.build), "(未知版本)");

		InterlockedExchange(&g_server_hello, 1);
		g_server_proto = hi.proto_version;
		LogLine("被控端版本: %s (协议 v%d, 能力 0x%X)", hi.build, hi.proto_version, hi.caps);
		ConPrint("被控端版本: %s", hi.build);
		ConPrint("  协议 v%d, 能力: %s%s%s%s%s%s",
			hi.proto_version,
			(hi.caps & RC_CAP_SCREEN)    ? "屏幕 " : "",
			(hi.caps & RC_CAP_FILE)      ? "文件 " : "",
			(hi.caps & RC_CAP_SYSINFO)   ? "系统信息 " : "",
			(hi.caps & RC_CAP_CURSOR)    ? "定位 " : "",
			(hi.caps & RC_CAP_INPUTREC)  ? "键鼠录制 " : "",
			(hi.caps & RC_CAP_CLIPBOARD) ? "剪贴板 " : "");
		{
			char cap2[192] = { 0 };
			if (hi.caps & RC_CAP_AUDIO)  strcat_s(cap2, "系统声音 ");
			if (hi.caps & RC_CAP_CAMERA) strcat_s(cap2, "摄像头 ");
			if (hi.caps & RC_CAP_MIC)    strcat_s(cap2, "麦克风 ");
			if (cap2[0]) ConPrint("  高级能力: %s", cap2);
			else ConPrint("  高级能力: 无 (被控端 exe 较旧, 不支持声音/摄像头/麦克风)");
			ConPrint("  提示: 听被控端声音 -> 左侧[工具]页的 [系统声音(被控端->本机)] 按钮, 或输入 audio start");
			ConPrint("        被控端摄像头 -> [摄像头] 按钮 或 cam on (默认 640x360 流畅模式, 可用 cam res 调整)");
		}
		/* 远程(非本机)连接: 自动开启被控端系统声音回传; 本机自测不自动开, 避免啸叫 */
		if ((hi.caps & RC_CAP_AUDIO) && !ExtIsLoopbackTarget())
		{
			ConPrint("  已自动开启被控端系统声音回传(音量 100%%, 'audio vol <0-100>' 调整, 'audio stop' 关闭)");
			SndStart(SndSys(), 0);
		}
		if (hi.proto_version < RC_PROTO_VERSION)
			ConPrint("  注意: 被控端协议较旧(v%d < v%d)，部分新功能可能无响应，建议更新被控端 exe。",
				hi.proto_version, RC_PROTO_VERSION);
		ExtConsoleRefresh(g_hwnd);
		return true;
	}

	case CMD_CURSOR_RES:
	{
		if (len < (int)sizeof(CursorInfo))
			return true;
		CursorInfo ci;
		memcpy(&ci, body, sizeof(ci));
		g_locate_x = ci.x;
		g_locate_y = ci.y;
		g_locate_sw = ci.screen_w;
		g_locate_sh = ci.screen_h;
		g_locate_on = true;
		ConPrint("  被控端光标: (%d, %d)  被控端屏幕: %dx%d  主显示器: %dx%d",
			ci.x, ci.y, ci.screen_w, ci.screen_h, ci.monitor_w, ci.monitor_h);
		ConPrint("  已在远程画面上绘制红色十字定位");
		if (g_view != NULL)
			InvalidateRect(g_view, NULL, FALSE);
		char st[200];
		_snprintf_s(st, _TRUNCATE, "定位成功: 被控端光标 (%d, %d) / 屏幕 %dx%d", ci.x, ci.y, ci.screen_w, ci.screen_h);
		PostStatus(st);
		return true;
	}

	case CMD_AUTOSTART_RES:
	{
		std::string t = ExtTextAfterHead(body, len, 0);
		ConPrint("----------------- 被控端开机自启 -----------------");
		std::string line;
		for (size_t i = 0; i <= t.size(); ++i)
		{
			char ch = (i < t.size()) ? t[i] : '\n';
			if (ch == '\r')
				continue;
			if (ch == '\n')
			{
				if (!line.empty())
				{
					ConPrint("  %s", line.c_str());
					line.clear();
				}
			}
			else
			{
				line.push_back(ch);
			}
		}
		ConPrint("--------------------------------------------------");
		return true;
	}

	case CMD_SYSINFO_RES:
	{
		std::string t = ExtTextAfterHead(body, len, 0);
		ConPrint("---------------- 被控端系统信息 ----------------");
		std::string line;
		for (size_t i = 0; i <= t.size(); ++i)
		{
			char ch = (i < t.size()) ? t[i] : '\n';
			if (ch == '\r')
				continue;
			if (ch == '\n')
			{
				if (!line.empty())
				{
					ConPrint("  %s", line.c_str());
					line.clear();
				}
			}
			else
			{
				line.push_back(ch);
			}
		}
		ConPrint("------------------------------------------------");
		return true;
	}

	case CMD_CLIPBOARD_RES:
	{
		/* 回包体: ClipReq 头 + UTF-8 文本 */
		int op = -1;
		if (len >= (int)sizeof(ClipReq))
		{
			ClipReq r0;
			memcpy(&r0, body, sizeof(r0));
			if (r0.op >= 0 && r0.op <= 2) op = r0.op;
		}
		int off = (op >= 0) ? (int)sizeof(ClipReq) : 0;   /* 兼容旧被控端(无头) */
		std::string u8 = ExtTextAfterHead(body, len, off);
		std::string g = ExtUtf8ToGbk(u8);
		if (g.compare(0, 3, "ok:") == 0 || g.compare(0, 4, "err:") == 0)
		{
			ConPrint("  剪贴板操作完成: %s", g.c_str());
		}
		else
		{
			ConPrint("  被控端剪贴板(%d 字节):", (int)g.size());
			if (!g.empty())
				ConPrint("  %s", g.c_str());
			if (g_hwnd != NULL)
				SetWindowTextA(GetDlgItem(g_hwnd, IDC_EDIT_CLIPTXT), g.c_str());
		}
		return true;
	}

	case CMD_FILE_LIST_RES:
	{
		/* 目录列表回包: "@path\t<dir>\n" + 每行 "<kind>\t<name>\t<size>\t<time>" */
		std::string t = ExtTextAfterHead(body, len, 0);
		if (t.compare(0, 6, "@path\t") == 0)
		{
			size_t nl = t.find('\n');
			std::string dir = t.substr(6, (nl == std::string::npos) ? std::string::npos : nl - 6);
			ConPrint("  被控端目录: %s", dir.c_str());
			t.erase(0, (nl == std::string::npos) ? t.size() : nl + 1);
		}
		if (t.compare(0, 4, "err:") == 0)
		{
			ConPrint("  目录读取失败: %s", t.c_str() + 4);
			return true;
		}
		int shown = 0;
		size_t pos = 0;
		while (pos < t.size())
		{
			size_t nl = t.find('\n', pos);
			std::string line = t.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
			pos = (nl == std::string::npos) ? t.size() : nl + 1;
			if (!line.empty() && line[line.size() - 1] == '\r')
				line.erase(line.size() - 1);
			size_t t1 = line.find('\t');
			if (t1 == std::string::npos) continue;
			size_t t2 = line.find('\t', t1 + 1);
			if (t2 == std::string::npos) continue;
			size_t t3 = line.find('\t', t2 + 1);
			if (t3 == std::string::npos) continue;
			char kind = line[0];
			std::string name = line.substr(t1 + 1, t2 - t1 - 1);
			std::string size = line.substr(t2 + 1, t3 - t2 - 1);
			ConPrint("  %s %10s  %s", (kind == 'D' || kind == 'd') ? "[DIR] " : "      ", size.c_str(), name.c_str());
			shown++;
		}
		ConPrint("  -------- 共 %d 项 --------", shown);
		return false;   /* 继续交给 HandleFileListPacket 刷新文件页 */
	}

	case CMD_INPUT_LOG:
		return ExtOnInputLog(body, len);

	default:
		break;
	}
	return false;
}

/* ---------------- 视图上的定位十字 ---------------- */
static void ExtDrawMarker(HWND hwnd, HDC hdc)
{
	if (!g_locate_on || g_locate_sw <= 0 || g_locate_sh <= 0 || g_image.IsNull())
		return;
	RECT rc;
	GetClientRect(hwnd, &rc);
	int dw = rc.right - rc.left;
	int dh = rc.bottom - rc.top;
	if (dw <= 0 || dh <= 0)
		return;

	/* 与视图一致的比例映射(不依赖是否存在黑边计算函数) */
	int iw = g_image.GetWidth();
	int ih = g_image.GetHeight();
	double s1 = (double)dw / (double)iw;
	double s2 = (double)dh / (double)ih;
	double s = (s1 < s2) ? s1 : s2;
	int ow = (int)(iw * s + 0.5);
	int oh = (int)(ih * s + 0.5);
	int ox = (dw - ow) / 2;
	int oy = (dh - oh) / 2;
	int x = ox + (int)((double)g_locate_x * s + 0.5);
	int y = oy + (int)((double)g_locate_y * s + 0.5);

	HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
	HPEN old = (HPEN)SelectObject(hdc, pen);
	MoveToEx(hdc, x - 30, y, NULL);
	LineTo(hdc, x + 30, y);
	MoveToEx(hdc, x, y - 30, NULL);
	LineTo(hdc, x, y + 30);
	SelectObject(hdc, old);
	DeleteObject(pen);

	Ellipse(hdc, x - 6, y - 6, x + 6, y + 6);
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, RGB(255, 0, 0));
	char lab[96];
	_snprintf_s(lab, _TRUNCATE, "(%d, %d)", g_locate_x, g_locate_y);
	TextOutA(hdc, x + 10, y + 8, lab, (int)strlen(lab));
}

/* ---------------- 连接成功后的欢迎信息 ---------------- */
static bool g_ext_greeted = false;
static void ExtOnConnected(void)
{
	if (!g_connected)
	{
		g_ext_greeted = false;
		return;
	}
	if (g_ext_greeted)
		return;
	g_ext_greeted = true;
	/* v9: 剪贴板虚拟通道握手(告诉被控端本端能力与自动同步开关), 并上报本端当前剪贴板 */
	InterlockedExchange(&g_cclip_ready, 0);
	g_cclip_last_seq = GetClipboardSequenceNumber();
	{
		ClipMsg hm;
		memset(&hm, 0, sizeof(hm));
		hm.msg = RC_CM_HELLO;
		hm.fmt = RC_CF_TEXT | RC_CF_UNICODE | RC_CF_PNG | RC_CF_FILES | RC_CF_DIB;
		hm.flags = InterlockedCompareExchange(&g_cclip_auto, 0, 0) ? 1 : 0;
		SendCmdPacket(CMD_CLIP_CHANNEL, (char*)&hm, sizeof(hm));
		LogLine("clip: 剪贴板通道 HELLO 已发送(自动同步=%s)", hm.flags ? "开" : "关");
	}
	DevTouch(g_ip, g_port);          /* 记录到设备列表(历史设备 + 最后连接时间) */
	ExtConsoleRefresh(g_hwnd);
	ConPrint("================ 已连接到被控端 ================");
	ConPrint("  已记录设备 %s:%d  (输入 dev 查看设备列表/在线状态)", g_ip, g_port);
	ConPrint("右侧“命令行”页可直接输入命令，回车或点“执行”运行；");
	ConPrint("输入 help 查看全部命令（截图/定位/剪贴板/系统信息/录制/文件传输）。");
}

/* ---------------- 命令输入框: 上/下键翻历史 ---------------- */
static LRESULT CALLBACK ExtCmdInProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_KEYDOWN)
	{
		if (wp == VK_UP || wp == VK_DOWN)
		{
			int n = (int)g_cmd_hist.size();
			if (n > 0)
			{
				if (wp == VK_UP)
				{
					if (g_hist_pos > 0)
						g_hist_pos--;
				}
				else if (g_hist_pos < n)
				{
					g_hist_pos++;
				}
				const char* t = (g_hist_pos >= 0 && g_hist_pos < n) ? g_cmd_hist[(size_t)g_hist_pos].c_str() : "";
				SetWindowTextA(hwnd, t);
				SendMessageA(hwnd, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
			}
			return 0;
		}
		if (wp == VK_RETURN)
		{
			char buf[2048] = { 0 };
			GetWindowTextA(hwnd, buf, sizeof(buf) - 1);
			SetWindowTextA(hwnd, "");
			std::string line = ExtTrim(std::string(buf));
			if (!line.empty())
				ExtRunCommand(line);
			return 0;
		}
	}
	if (g_cmd_in_old != NULL)
		return CallWindowProcA(g_cmd_in_old, hwnd, msg, wp, lp);
	return DefWindowProcA(hwnd, msg, wp, lp);
}

static void Layout(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int cx = rc.right, cy = rc.bottom;
	int px = cx - 340;          /* 右侧面板起点 */

	/* v10: 全屏模式 —— 画面窗口铺满整个客户区, 其余控件已隐藏 */
	if (InterlockedCompareExchange(&g_fullscreen, 0, 0) != 0)
	{
		MoveWindow(g_view, 0, 0, cx, cy, TRUE);
		return;
	}
	int pw = cx - px - 10;      /* 面板可用宽度 */
	const int SBH = 22;         /* 底部状态栏高度 */
	const int cyu = cy - SBH;   /* 状态栏以上可用高度 */

	/* 状态栏：整宽，永远可见 */
	MoveWindow(GetDlgItem(hwnd, IDC_STATIC_STATUS), 8, cy - SBH + 2, cx - 16, 18, TRUE);

	MoveWindow(g_view, 8, 76, px - 18, cyu - 84, TRUE);

	/* 说明: 文件页各控件由 ExtLayout() 统一在 Tab 客户区内排版, 此处不再定位,
	         以免与 Tab 页签/其他控件重叠 */
}

/* 状态栏/标题栏：连接状态 + 延迟 + 帧率 + 码率 + 远端分辨率 */
static void UpdateStatusBar(HWND hwnd)
{
	char text[360];
	int iw = 0, ih = 0;
	EnterCriticalSection(&g_state_lock);
	if (!g_image.IsNull())
	{
		iw = g_image.GetWidth();
		ih = g_image.GetHeight();
	}
	LeaveCriticalSection(&g_state_lock);

	if (g_connected)
	{
		if (iw > 0 && ih > 0)
			_snprintf_s(text, _TRUNCATE,
				"● 已连接  %s:%d    延迟 %d ms    %d.%d fps    %d KB/s    画面 %dx%d    %s",
				g_ip, g_port, g_last_rtt,
				g_stat_fps_x10 / 10, g_stat_fps_x10 % 10, g_stat_kbps,
				iw, ih, g_encoder == RC_ENC_JPEG ? "JPEG" : "PNG");
		else
			_snprintf_s(text, _TRUNCATE, "● 已连接  %s:%d    等待第一帧画面 ...", g_ip, g_port);
	}
	else
	{
		_snprintf_s(text, _TRUNCATE, "○ 未连接 —— 填写被控端 IP/端口后点“连接”");
	}
	SetWindowTextA(GetDlgItem(hwnd, IDC_STATIC_STATUS), text);
	SetWindowTextA(hwnd, g_connected ? "RemoteControl 控制端 v3.0 —— 已连接"
		: "RemoteControl 控制端 v3.0 —— 未连接");
	InvalidateRect(GetDlgItem(hwnd, IDC_STATIC_STATUS), NULL, TRUE);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		g_hwnd = hwnd;
		HINSTANCE hInst = GetModuleHandleA(NULL);

		Mk(hwnd, "STATIC", "IP:", 0, 10, 12, 22, 18, -1);
		Mk(hwnd, "EDIT", g_ip, WS_BORDER | ES_AUTOHSCROLL, 34, 8, 130, 22, IDC_EDIT_IP);
		Mk(hwnd, "STATIC", "端口:", 0, 172, 12, 40, 18, -1);
		Mk(hwnd, "EDIT", "9999", WS_BORDER | ES_NUMBER, 214, 8, 60, 22, IDC_EDIT_PORT);
		Mk(hwnd, "BUTTON", "连接", BS_PUSHBUTTON, 282, 8, 66, 24, IDC_BTN_CONNECT);
		Mk(hwnd, "BUTTON", "断开", BS_PUSHBUTTON, 354, 8, 66, 24, IDC_BTN_DISCONNECT);
		Mk(hwnd, "STATIC", "帧率:", 0, 428, 12, 38, 18, -1);
		HWND cf = Mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 468, 8, 62, 200, IDC_COMBO_FPS);
		Mk(hwnd, "STATIC", "编码:", 0, 538, 12, 38, 18, -1);
		HWND ce = Mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 578, 8, 76, 200, IDC_COMBO_ENC);
		Mk(hwnd, "STATIC", "画质:", 0, 662, 12, 38, 18, -1);
		HWND cq = Mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 702, 8, 70, 200, IDC_COMBO_Q);
		/* 状态栏：整宽一条放在窗口最底部，任何窗口尺寸都看得见（Layout 会重排） */
		Mk(hwnd, "STATIC", "未连接 —— 填写被控端 IP/端口后点“连接”", 0, 8, 0, 400, 18, IDC_STATIC_STATUS);

		const char* fpsItems[] = { "2", "5", "10", "15", "20", "30" };
		for (int i = 0; i < 6; ++i)
			SendMessageA(cf, CB_ADDSTRING, 0, (LPARAM)fpsItems[i]);
		SendMessageA(cf, CB_SETCURSEL, 2, 0);
		SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)"PNG");
		SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)"JPEG");
		SendMessageA(ce, CB_SETCURSEL, 1, 0);      /* 默认 JPEG：明显更省带宽、更流畅 */
		g_encoder = RC_ENC_JPEG;
		const char* qItems[] = { "30", "50", "70", "90" };
		for (int i = 0; i < 4; ++i)
			SendMessageA(cq, CB_ADDSTRING, 0, (LPARAM)qItems[i]);
		SendMessageA(cq, CB_SETCURSEL, 2, 0);      /* 默认 70 */
		g_quality = 70;

		/* 面板 */
		Mk(hwnd, "STATIC", "路径:", 0, 0, 46, 44, 18, IDC_LBL_PATH);
		Mk(hwnd, "EDIT", g_cur_path, WS_BORDER | ES_AUTOHSCROLL, 0, 44, 100, 22, IDC_EDIT_PATH);
		Mk(hwnd, "BUTTON", "刷新", BS_PUSHBUTTON, 0, 72, 70, 24, IDC_BTN_REFRESH);
		Mk(hwnd, "BUTTON", "上一级", BS_PUSHBUTTON, 0, 72, 70, 24, IDC_BTN_UP);
		Mk(hwnd, "BUTTON", "下载", BS_PUSHBUTTON, 0, 72, 70, 24, IDC_BTN_DOWNLOAD);
		Mk(hwnd, "BUTTON", "上传", BS_PUSHBUTTON, 0, 72, 70, 24, IDC_BTN_UPLOAD);
		Mk(hwnd, "BUTTON", "新建目录", BS_PUSHBUTTON, 0, 102, 70, 24, IDC_BTN_MKDIR);
		Mk(hwnd, "BUTTON", "重命名", BS_PUSHBUTTON, 0, 102, 70, 24, IDC_BTN_RENAME);
		Mk(hwnd, "BUTTON", "删除", BS_PUSHBUTTON, 0, 102, 70, 24, IDC_BTN_DELETE);
		Mk(hwnd, "STATIC", "名称:", 0, 0, 138, 44, 18, IDC_LBL_NAME);
		Mk(hwnd, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 0, 134, 100, 22, IDC_EDIT_NAME);
		Mk(hwnd, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 0, 162, 100, 100, IDC_LIST_FILES);
		Mk(hwnd, "EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			0, 300, 100, 100, IDC_EDIT_LOG);

		g_view = CreateWindowExA(0, VIEW_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			8, 44, 100, 100, hwnd, NULL, hInst, NULL);
	DragAcceptFiles(g_view, TRUE);
	DragAcceptFiles(hwnd, TRUE);   /* 整个窗口也接受拖放(含工具条/边框) */
	/* 控制端若以管理员运行: UIPI 会拦掉资源管理器(中完整性)发来的拖放消息, 必须显式放行 */
	ExtAllowDropMessages(g_view);
	ExtAllowDropMessages(hwnd);
	ImmAssociateContext(hwnd, NULL);
	ImmAssociateContext(g_view, NULL);   /* 画面窗口禁用输入法, 避免本地输入法抢占远程输入 */   /* 画面窗口接受文件拖放(拖进来即上传) */
		SetFocus(g_view);

		g_frame_evt = CreateEventA(NULL, FALSE, FALSE, NULL);   /* 乒乓式请求用的“已回帧”事件 */
		g_stat_base = GetTickCount64();
		SetTimer(hwnd, 1, 500, NULL);                          /* 状态栏每 0.5s 刷新 */
		SetTimer(hwnd, 2, 300, NULL);                          /* v9: 剪贴板同步轮询 */

		Layout(hwnd);
		ExtCreateControls(hwnd);
		UpdateStatusBar(hwnd);
		LogLine("控制端就绪：填写被控端 IP/端口后点“连接”");
	}
	return 0;

	case WM_SIZE:
		Layout(hwnd);
		ExtLayout(hwnd);
		return 0;

	case WM_GETMINMAXINFO:
	{
		MINMAXINFO* mmi = (MINMAXINFO*)lParam;
		mmi->ptMinTrackSize.x = 900;
		mmi->ptMinTrackSize.y = 620;
	}
	return 0;

	case WM_APP_LOG:
	{
		HWND ed = GetDlgItem(hwnd, IDC_EDIT_LOG);
		if (ed != NULL)
		{
			std::string all;
			EnterCriticalSection(&g_state_lock);
			size_t start = g_logs.size() > 120 ? g_logs.size() - 120 : 0;
			for (size_t i = start; i < g_logs.size(); ++i)
			{
				all += g_logs[i];
				all += "\r\n";
			}
			LeaveCriticalSection(&g_state_lock);
			SetWindowTextA(ed, all.c_str());
			SendMessageA(ed, EM_SETSEL, (WPARAM)-1, -1);
			SendMessageA(ed, EM_SCROLLCARET, 0, 0);
		}
	}
	return 0;

	case WM_APP_FILLLIST:
		FillFileList();
		return 0;

	case WM_APP_STATUS:
		UpdateStatusBar(hwnd);
		ExtOnConnected();
		return 0;

	case WM_NOTIFY:
		if (ExtOnNotify(hwnd, lParam))
			return 0;
		break;

	case WM_APP_SETPAGE:

		ExtSetPage(hwnd, (int)wParam);

		return 0;



	case WM_APP_CONSOLE:
		ExtConsoleRefresh(hwnd);
		return 0;

	case WM_SETFOCUS:
		/* 主窗口拿到焦点(切回本程序/点击空白处)时把键盘直接交给远程画面: 连上就能直接打字 */
		if (g_view != NULL)
			SetFocus(g_view);
		return 0;

	case WM_TIMER:
	{
		if (wParam == 2)                       /* v9: 剪贴板同步轮询(300ms) */
		{
			if (g_connected && InterlockedCompareExchange(&g_cclip_auto, 0, 0) != 0 &&
				InterlockedCompareExchange(&g_cclip_ready, 0, 0) != 0 &&
				InterlockedCompareExchange(&g_cclip_hold, 0, 0) == 0)
			{
				const DWORD sq = GetClipboardSequenceNumber();
				if (sq != 0 && sq != g_cclip_last_seq)
				{
					g_cclip_last_seq = sq;
					CClipOfferSelf();
				}
			}
			return 0;
		}
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG base = g_stat_base;
		if (base != 0 && now > base)
		{
			const ULONGLONG dt = now - base;
			if (dt >= 400)
			{
				const LONG frames = InterlockedExchange(&g_stat_frames, 0);
				const LONG bytes = InterlockedExchange(&g_stat_bytes, 0);
				g_stat_fps_x10 = (int)((ULONGLONG)frames * 10000 / dt);
				g_stat_kbps = (int)((ULONGLONG)bytes * 1000 / dt / 1024);
				g_stat_base = now;
			}
		}
		else
		{
			g_stat_base = now;
		}
		/* 版本握手超时: 连接上了、也发了 CMD_HELLO，但 2 秒内没有任何版本回包
		   → 对端是旧版被控端(不认识 v3 新命令)，此时必须明确告警，而不是让用户面对"没有响应"。 */
		if (g_connected && g_hello_tick != 0 && !g_server_hello && !g_hello_warned &&
			GetTickCount64() - g_hello_tick > 2000)
		{
			InterlockedExchange(&g_hello_warned, 1);
			LogLine("被控端未响应版本查询(CMD_HELLO) —— 疑似旧版被控端");
			ConPrint("");
			ConPrint("⚠ 被控端版本过旧：它不响应版本查询(v3 新命令)。");
			ConPrint("  症状: ping 有回包，但 系统信息/定位/截图/录制/剪贴板 都没有任何回应。");
			ConPrint("  解决: 把最新的 RemoteControl.exe 复制到被控端机器覆盖并重启该程序。");
			ExtConsoleRefresh(hwnd);
		}
		{
			static bool s_kbHinted = false;
			if (!s_kbHinted)
			{
				s_kbHinted = true;
				LogLine("键盘已接管: 鼠标移到远程画面上即可直接打字; 中文请用 type <文本>");
			}
		}
		UpdateStatusBar(hwnd);
	}
	return 0;

	case WM_CTLCOLORSTATIC:
		if ((HWND)lParam == GetDlgItem(hwnd, IDC_STATIC_STATUS))
		{
			HDC dc = (HDC)wParam;
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, g_connected ? RGB(0, 140, 0) : RGB(200, 0, 0));
			return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
		}
		break;

	case WM_COMMAND:
	{
		int id = LOWORD(wParam);
		int code = HIWORD(wParam);
		if (ExtOnCommand(hwnd, id, code))
			return 0;

		switch (id)
		{
		case IDC_PAGEBTN0: ExtSetPage(hwnd, 0); break;
		case IDC_PAGEBTN1: ExtSetPage(hwnd, 1); break;
		case IDC_PAGEBTN2: ExtSetPage(hwnd, 2); break;
		case IDC_PAGEBTN3: ExtSetPage(hwnd, 3); break;

		case IDC_BTN_CONNECT:
			DoConnect();
			break;

		case IDC_BTN_DISCONNECT:
			g_manual_disconnect = 1;
			if (g_sock != INVALID_SOCKET)
			{
				SendCmdPacket(CMD_DISCONNECT, NULL, 0);
				closesocket(g_sock);
				g_sock = INVALID_SOCKET;
			}
			SetConnected(false);
			LogLine("已断开连接");
			break;

		case IDC_BTN_REFRESH:
		{
			std::string p;
			GetPathEdit(p);
			strncpy_s(g_cur_path, p.c_str(), _TRUNCATE);
			SendText(CMD_FILE_LIST, g_cur_path);
			break;
		}

		case IDC_BTN_UP:
		{
			std::string p = g_cur_path;
			if (p.size() > 3)
			{
				if (p.back() == '\\' || p.back() == '/')
					p.pop_back();
				size_t pos = p.find_last_of("\\/");
				if (pos != std::string::npos)
				{
					std::string up = p.substr(0, pos);
					if (up.size() == 2 && up[1] == ':')
						up += "\\";
					strncpy_s(g_cur_path, up.c_str(), _TRUNCATE);
					SetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_PATH), g_cur_path);
					SendText(CMD_FILE_LIST, g_cur_path);
				}
			}
			break;
		}

		case IDC_BTN_DOWNLOAD:
		{
			FileEntry e;
			if (!GetSelectedEntry(e))
			{
				LogLine("请先在列表中选中一个文件");
				break;
			}
			if (e.kind == 'D')
			{
				LogLine("目录不支持下载，双击可进入");
				break;
			}
			DoDownload(e, JoinPath(g_cur_path, e.name));
			break;
		}

		case IDC_BTN_UPLOAD:
			if (!g_connected)
				LogLine("未连接，无法上传");
			else
				StartUpload();
			break;

		case IDC_BTN_MKDIR:
		{
			std::string name;
			GetNameEdit(name);
			if (name.empty())
			{
				LogLine("请在“名称”框输入新目录名");
				break;
			}
			SendFileOp(RC_OP_MKDIR, JoinPath(g_cur_path, name), "");
			SendText(CMD_FILE_LIST, g_cur_path);
			break;
		}

		case IDC_BTN_RENAME:
		{
			FileEntry e;
			std::string name;
			if (!GetSelectedEntry(e))
			{
				LogLine("请先在列表中选中要重命名的项");
				break;
			}
			GetNameEdit(name);
			if (name.empty())
			{
				LogLine("请在“名称”框输入新名称");
				break;
			}
			SendFileOp(RC_OP_RENAME, JoinPath(g_cur_path, e.name), JoinPath(g_cur_path, name));
			SendText(CMD_FILE_LIST, g_cur_path);
			break;
		}

		case IDC_BTN_DELETE:
		{
			FileEntry e;
			if (!GetSelectedEntry(e))
			{
				LogLine("请先在列表中选中要删除的项");
				break;
			}
			std::string full = JoinPath(g_cur_path, e.name);
			char confirm[600];
			_snprintf_s(confirm, _TRUNCATE, "确认删除被控端上的：\n%s\n\n该操作不可撤销！", full.c_str());
			if (MessageBoxA(hwnd, confirm, "删除确认", MB_YESNO | MB_ICONWARNING) == IDYES)
			{
				SendFileOp(RC_OP_DELETE, full, "");
				SendText(CMD_FILE_LIST, g_cur_path);
			}
			break;
		}

		case IDC_LIST_FILES:
			if (code == LBN_DBLCLK)
			{
				FileEntry e;
				if (GetSelectedEntry(e))
				{
					if (e.kind == 'D')
					{
						std::string p = JoinPath(g_cur_path, e.name);
						strncpy_s(g_cur_path, p.c_str(), _TRUNCATE);
						SetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_PATH), g_cur_path);
						SendText(CMD_FILE_LIST, g_cur_path);
					}
					else
					{
						DoDownload(e, JoinPath(g_cur_path, e.name));
					}
				}
			}
			break;

		case IDC_COMBO_FPS:
			if (code == CBN_SELCHANGE)
			{
				char buf[16] = { 0 };
				GetWindowTextA(GetDlgItem(hwnd, IDC_COMBO_FPS), buf, sizeof(buf) - 1);
				int f = atoi(buf);
				if (f > 0)
				{
					g_fps = f;
					ApplyScreenCtrl(false);   /* 只调速率，无需重建基准帧 */
					LogLine("帧率设置为 %d fps", g_fps);
				}
			}
			break;

		case IDC_COMBO_ENC:
			if (code == CBN_SELCHANGE)
			{
				char buf[16] = { 0 };
				GetWindowTextA(GetDlgItem(hwnd, IDC_COMBO_ENC), buf, sizeof(buf) - 1);
				g_encoder = (_stricmp(buf, "JPEG") == 0) ? RC_ENC_JPEG : RC_ENC_PNG;
				ApplyScreenCtrl(true);    /* 编码变了：重建基准帧 */
				LogLine("编码方式: %s", g_encoder == RC_ENC_JPEG ? "JPEG" : "PNG");
			}
			break;
		}
	}
	return 0;

	case WM_HSCROLL:
	{
		HWND h = (HWND)lParam;
		if (h != NULL && h == GetDlgItem(hwnd, IDC_SLD_AUDSYS))
		{
			SndSetVolume(&g_sndSys, (int)SendMessageA(h, TBM_GETPOS, 0, 0));
			ExtAudioUpdateUI(hwnd);
			return 0;
		}
		if (h != NULL && h == GetDlgItem(hwnd, IDC_SLD_AUDMIC))
		{
			SndSetVolume(&g_sndMic, (int)SendMessageA(h, TBM_GETPOS, 0, 0));
			ExtAudioUpdateUI(hwnd);
			return 0;
		}
		break;
	}

	case WM_IME_SETCONTEXT:
		lParam &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;   /* 本机输入法不在远程画面上弹候选窗 */
		break;

	case WM_IME_STARTCOMPOSITION:
	case WM_IME_COMPOSITION:
		return 0;

	case WM_CLOSE:
		InterlockedExchange(&g_running, 0);
		InterlockedExchange(&g_auto_reconnect, 0);
		if (g_sock != INVALID_SOCKET)
		{
			SendCmdPacket(CMD_DISCONNECT, NULL, 0);
			closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, 1);
		EnterCriticalSection(&g_state_lock);
		CacheFree();
		LeaveCriticalSection(&g_state_lock);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	InitializeCriticalSection(&g_send_lock);
	InitializeCriticalSection(&g_state_lock);
	InitializeCriticalSection(&g_cclip_lock);            /* v9: 剪贴板通道 */
	g_cclip_last_seq = GetClipboardSequenceNumber();

	LoadSettings();   /* v12: 先读 client.ini, 随后命令行参数可覆盖 */

	/* 命令行: client.exe [IP] [端口] */
	if (lpCmdLine != NULL && lpCmdLine[0] != 0)
	{
		char buf[256] = { 0 };
		strncpy_s(buf, lpCmdLine, _TRUNCATE);
		char* ctx = NULL;
		char* tok = strtok_s(buf, " \t", &ctx);
		if (tok != NULL)
			strncpy_s(g_ip, tok, _TRUNCATE);
		tok = strtok_s(NULL, " \t", &ctx);
		if (tok != NULL)
		{
			int p = atoi(tok);
			if (p > 0 && p < 65536)
				g_port = p;
		}
	}

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	/* --dev "<子命令>": 无头执行设备命令(脚本/自测用, 不创建窗口) */
	if (lpCmdLine != NULL && _strnicmp(lpCmdLine, "--dev", 5) == 0 &&
		(lpCmdLine[5] == '\0' || lpCmdLine[5] == ' ' || lpCmdLine[5] == '\t'))
	{
		InterlockedExchange(&g_dev_headless, 1);
		DevEnsureLock();
		int rc = DevHeadlessMain(std::string(lpCmdLine + 5));
		WSACleanup();
		return rc;
	}

	WNDCLASSEXA wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = MainProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = MAIN_CLASS;
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	RegisterClassExA(&wc);

	WNDCLASSEXA vc;
	memset(&vc, 0, sizeof(vc));
	vc.cbSize = sizeof(vc);
	vc.style = CS_DBLCLKS;
	vc.lpfnWndProc = ViewProc;
	vc.hInstance = hInstance;
	vc.hCursor = LoadCursor(NULL, IDC_ARROW);
	vc.lpszClassName = VIEW_CLASS;
	RegisterClassExA(&vc);

	HWND hwnd = CreateWindowExA(0, MAIN_CLASS, "RemoteControl 控制端 v2.0 - 未连接",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820,
		NULL, NULL, hInstance, NULL);
	if (hwnd == NULL)
	{
		MessageBoxA(NULL, "创建窗口失败！", "错误", MB_OK | MB_ICONERROR);
		return 0;
	}
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	DWORD tid = 0;
	HANDLE h;
	h = CreateThread(NULL, 0, RecvThread, NULL, 0, &tid);      if (h) CloseHandle(h);
	h = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, &tid); if (h) CloseHandle(h);
	h = CreateThread(NULL, 0, ScreenThread, NULL, 0, &tid);    if (h) CloseHandle(h);

	MSG msg;
	while (GetMessageA(&msg, NULL, 0, 0) > 0)
	{
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	WSACleanup();
	return 0;
}
