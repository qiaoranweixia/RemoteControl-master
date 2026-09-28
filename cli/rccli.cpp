/* ============================================================
 *  rccli.cpp - RemoteControl 命令行控制端 (控制端 CLI)
 *  用法: rccli --host 127.0.0.1 --port 9527 <命令> [参数...]
 * ============================================================ */
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include "../common/protocol.h"

#pragma comment(lib, "ws2_32.lib")

static SOCKET       g_sock = INVALID_SOCKET;
static PacketStream g_stream;
static unsigned int g_file_id = 1;

/* ---------------- 基础收发 ---------------- */
static bool SendAll(const char* data, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = send(g_sock, data + sent, len - sent, 0);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

static bool SendPkt(int cmd, const void* body, int len)
{
	std::vector<char> buf(sizeof(PacketHeader) + (len > 0 ? len : 0));
	PacketHeader h;
	h.magic = RC_MAGIC;
	h.cmd = cmd;
	h.body_len = len;
	memcpy(buf.data(), &h, sizeof(h));
	if (len > 0)
		memcpy(buf.data() + sizeof(h), body, len);
	return SendAll(buf.data(), (int)buf.size());
}

static void SendText(int cmd, const std::string& s) { SendPkt(cmd, s.data(), (int)s.size()); }

static Packet* RecvPkt(int timeout_ms)
{
	DWORD start = GetTickCount();
	std::vector<char> buf(65536);
	for (;;)
	{
		Packet* p = g_stream.Next();
		if (p != NULL)
			return p;

		int left = timeout_ms - (int)(GetTickCount() - start);
		if (left <= 0)
			return NULL;

		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(g_sock, &rd);
		timeval tv;
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		int sel = select(0, &rd, NULL, NULL, &tv);
		if (sel <= 0)
			return NULL;

		int n = recv(g_sock, buf.data(), (int)buf.size(), 0);
		if (n <= 0)
			return NULL;
		g_stream.Append(buf.data(), n);
	}
}

static Packet* WaitFor(int want_cmd, int timeout_ms)
{
	DWORD start = GetTickCount();
	for (;;)
	{
		int left = timeout_ms - (int)(GetTickCount() - start);
		if (left <= 0)
			return NULL;
		Packet* p = RecvPkt(left);
		if (p == NULL)
			return NULL;
		if (want_cmd < 0 || p->header.cmd == want_cmd)
			return p;
		free(p);
	}
}

static std::string BodyStr(Packet* p)
{
	return std::string(p->body, (size_t)p->header.body_len);
}

/* ---------------- 连接 ---------------- */
static bool ConnectTo(const char* host, int port)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		fprintf(stderr, "WSAStartup 失败\n");
		return false;
	}
	g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_sock == INVALID_SOCKET)
	{
		fprintf(stderr, "创建套接字失败\n");
		return false;
	}
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	if (InetPtonA(AF_INET, host, &addr.sin_addr) != 1)
	{
		hostent* he = gethostbyname(host);
		if (he == NULL)
		{
			fprintf(stderr, "无法解析主机 %s\n", host);
			return false;
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
	}
	if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) != 0)
	{
		fprintf(stderr, "连接 %s:%d 失败 (错误码 %d)\n", host, port, WSAGetLastError());
		return false;
	}
	return true;
}

/* ---------------- 屏幕 ---------------- */
static void SendScreenCtrl(int fps, int quality, int encode, int force_full)
{
	ScreenCtrl sc;
	sc.fps = fps;
	sc.quality = quality;
	sc.encode = encode;
	sc.monitor = 0;
	sc.force_full = force_full;
	SendPkt(CMD_SCREEN_CTRL, &sc, sizeof(sc));
}

static bool SaveShot(const char* out, int quality, int encode, bool verbose)
{
	SendScreenCtrl(10, quality, encode, 1);   /* 先强制整屏，保证拿到完整画面 */
	SendPkt(CMD_SCREEN, NULL, 0);

	Packet* p = WaitFor(CMD_SCREEN, 10000);
	if (p == NULL)
	{
		fprintf(stderr, "等待屏幕数据超时\n");
		return false;
	}
	if (p->header.body_len < (int)sizeof(ScreenFrameHead))
	{
		fprintf(stderr, "屏幕数据过短\n");
		free(p);
		return false;
	}
	ScreenFrameHead fh;
	memcpy(&fh, p->body, sizeof(fh));
	int payload = p->header.body_len - (int)sizeof(fh);
	if (payload <= 0 || fh.w <= 0 || fh.h <= 0)
	{
		fprintf(stderr, "空帧(画面无变化)\n");
		free(p);
		return false;
	}
	FILE* fp = NULL;
	if (fopen_s(&fp, out, "wb") != 0 || fp == NULL)
	{
		fprintf(stderr, "无法写入 %s\n", out);
		free(p);
		return false;
	}
	fwrite(p->body + sizeof(fh), 1, (size_t)payload, fp);
	fclose(fp);
	if (verbose)
		printf("已保存 %s  区域=%dx%d@%d,%d 整屏=%dx%d 格式=%s 载荷=%d 字节\n",
			out, fh.w, fh.h, fh.x, fh.y, fh.full_w, fh.full_h,
			fh.format == RC_ENC_JPEG ? "JPEG" : "PNG", payload);
	free(p);
	SendScreenCtrl(10, quality, encode, 0);
	return true;
}

static int CmdWatch(int frames, int interval_ms, const char* save_last, int quality, int encode)
{
	int fps = interval_ms > 0 ? (1000 / interval_ms) : 10;
	if (fps < 1) fps = 1;
	SendScreenCtrl(fps, quality, encode, 0);
	printf("连续抓帧 %d 帧...\n", frames);
	printf("   帧号  类型     区域           载荷      耗时\n");
	std::string last;
	unsigned long long total = 0;
	DWORD t0 = GetTickCount();
	int got = 0;
	for (int i = 0; i < frames; i++)
	{
		DWORD t = GetTickCount();
		SendPkt(CMD_SCREEN, NULL, 0);
		Packet* p = WaitFor(CMD_SCREEN, 5000);
		if (p == NULL)
		{
			printf("  超时\n");
			continue;
		}
		DWORD dt = GetTickCount() - t;
		ScreenFrameHead fh;
		memset(&fh, 0, sizeof(fh));
		if (p->header.body_len >= (int)sizeof(fh))
			memcpy(&fh, p->body, sizeof(fh));
		int payload = p->header.body_len - (int)sizeof(fh);
		if (payload < 0) payload = 0;
		total += payload;
		got++;
		const char* kind = (fh.flags & RC_FR_FULL) ? "整屏" :
			((fh.flags & RC_FR_PARTIAL) ? "增量" : "无变化");
		printf("  %5d  %-6s  %4dx%-4d@%-5d,%-5d %7d B  %4lu ms\n",
			i, kind, fh.w, fh.h, fh.x, fh.y, payload, dt);
		if (save_last != NULL && payload > 0)
			last.assign(p->body + sizeof(fh), (size_t)payload);
		free(p);
		if (interval_ms > 0)
			Sleep(interval_ms);
	}
	DWORD span = GetTickCount() - t0;
	if (got > 0)
		printf("共 %d 帧, 合计 %llu 字节, 平均 %llu B/帧, 平均 %lu ms/帧\n",
			got, total, total / got, span / (DWORD)got);
	if (save_last != NULL && !last.empty())
	{
		FILE* fp = NULL;
		if (fopen_s(&fp, save_last, "wb") == 0 && fp != NULL)
		{
			fwrite(last.data(), 1, last.size(), fp);
			fclose(fp);
			printf("最后一帧已保存: %s\n", save_last);
		}
	}
	return 0;
}

/* ---------------- 文件 ---------------- */
static std::string BaseName(const std::string& p)
{
	size_t k = p.find_last_of("\\/");
	return (k == std::string::npos) ? p : p.substr(k + 1);
}

/* 被控端起始目录(--root 沙箱根)，首次使用时查询并缓存 */
static std::string g_remote_base;
static std::string RemoteBase()
{
	if (!g_remote_base.empty())
		return g_remote_base;
	SendText(CMD_FILE_LIST, std::string());
	Packet* p = WaitFor(CMD_FILE_LIST_RES, 8000);
	if (p == NULL)
		return std::string();
	std::string body = BodyStr(p);
	free(p);
	if (body.compare(0, 5, "@path") == 0)
	{
		size_t nl = body.find('\n');
		size_t end = (nl == std::string::npos) ? body.size() : nl;
		if (end > 6)
			g_remote_base = body.substr(6, end - 6);
	}
	return g_remote_base;
}

/* 相对路径 → 被控端绝对路径；已是绝对路径则原样返回 */
static std::string RemoteResolve(const char* path)
{
	if (path == NULL || *path == 0)
		return std::string();
	std::string s(path);
	if (s.find(':') != std::string::npos || s[0] == '\\' || s[0] == '/')
		return s;                                  /* 绝对路径交给被控端校验 */
	std::string base = RemoteBase();
	if (base.empty())
		return s;
	char last = base[base.size() - 1];
	if (last != '\\' && last != '/')
		base += '\\';
	return base + s;
}

static int CmdLs(const char* path)
{
	std::string rp = RemoteResolve(path);
	SendText(CMD_FILE_LIST, rp);
	Packet* p = WaitFor(CMD_FILE_LIST_RES, 8000);
	if (p == NULL)
	{
		fprintf(stderr, "列目录超时\n");
		return 1;
	}
	std::string body = BodyStr(p);
	free(p);
	if (body.compare(0, 4, "err:") == 0)
	{
		fprintf(stderr, "%s\n", body.c_str());
		return 1;
	}
	int nl = (int)body.find('\n');
	std::string first = (nl == (int)std::string::npos) ? body : body.substr(0, nl);
	std::string rest = (nl == (int)std::string::npos) ? std::string() : body.substr(nl + 1);
	printf("目录: %s\n",
		first.compare(0, 5, "@path") == 0 ? first.substr(6).c_str() : first.c_str());
	printf("%-4s %-40s %12s  %s\n", "类型", "名称", "大小", "修改时间");
	int count = 0;
	size_t pos = 0;
	while (pos < rest.size())
	{
		size_t e = rest.find('\n', pos);
		std::string line = rest.substr(pos, (e == std::string::npos ? rest.size() : e) - pos);
		pos = (e == std::string::npos) ? rest.size() : e + 1;
		if (line.empty())
			continue;
		size_t t1 = line.find('\t');
		size_t t2 = line.find('\t', t1 + 1);
		size_t t3 = line.find('\t', t2 + 1);
		if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos)
			continue;
		std::string type = (line[0] == 'D') ? "目录" : "文件";
		std::string name = line.substr(t1 + 1, t2 - t1 - 1);
		std::string size = line.substr(t2 + 1, t3 - t2 - 1);
		std::string tm = line.substr(t3 + 1);
		printf("%-4s %-40s %12s  %s\n", type.c_str(), name.c_str(), size.c_str(), tm.c_str());
		count++;
	}
	printf("共 %d 项\n", count);
	return 0;
}

static int CmdGet(const char* remote, const char* local)
{
	std::string rpath = RemoteResolve(remote);
	FileRequest req;
	memset(&req, 0, sizeof(req));
	req.file_id = g_file_id;
	req.offset = 0;
	req.file_size = 0;
	strncpy_s(req.path, sizeof(req.path), rpath.c_str(), _TRUNCATE);

	std::string out = (local != NULL) ? std::string(local) : BaseName(rpath);
	FILE* fp = NULL;
	if (fopen_s(&fp, out.c_str(), "wb") != 0 || fp == NULL)
	{
		fprintf(stderr, "无法写入 %s\n", out.c_str());
		return 1;
	}

	SendPkt(CMD_FILE_DOWNLOAD, &req, sizeof(req));
	unsigned long long total = 0;
	DWORD start = GetTickCount();
	int failed = 0;
	for (;;)
	{
		Packet* p = WaitFor(-1, 15000);
		if (p == NULL)
		{
			fprintf(stderr, "下载超时\n");
			failed = 1;
			break;
		}
		int cmd = p->header.cmd;
		if (cmd == CMD_FILE_DATA)
		{
			if (p->header.body_len >= (int)sizeof(FileChunk))
			{
				int n = p->header.body_len - (int)sizeof(FileChunk);
				fwrite(p->body + sizeof(FileChunk), 1, (size_t)n, fp);
				total += n;
			}
		}
		else if (cmd == CMD_FILE_END)
		{
			if (p->header.body_len >= (int)sizeof(FileChunk))
			{
				FileChunk c;
				memcpy(&c, p->body, sizeof(c));
				if (c.flags == RC_FF_ERROR)
				{
					failed = 1;
					fprintf(stderr, "被控端返回错误(路径越权或文件不存在)\n");
				}
			}
			free(p);
			break;
		}
		else if (cmd == CMD_FILE_PROGRESS)
		{
			printf("被控端: %s\n", BodyStr(p).c_str());
		}
		free(p);
	}
	fclose(fp);
	if (failed)
	{
		remove(out.c_str());
		return 1;
	}
	printf("已下载 %s  %llu 字节  %lu ms\n", out.c_str(), total, GetTickCount() - start);
	return 0;
}

static int CmdPut(const char* local, const char* remote)
{
	FILE* fp = NULL;
	if (fopen_s(&fp, local, "rb") != 0 || fp == NULL)
	{
		fprintf(stderr, "无法读取 %s\n", local);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	long long size = _ftelli64(fp);
	fseek(fp, 0, SEEK_SET);

	std::string dst;
	if (remote == NULL)
		dst = RemoteResolve(BaseName(local).c_str());
	else
	{
		std::string r = RemoteResolve(remote);
		char lastc = r.empty() ? 0 : r[r.size() - 1];
		dst = (lastc == '\\' || lastc == '/') ? (r + BaseName(local)) : r;   /* 目录 + 文件名 */
	}
	FileRequest req;
	memset(&req, 0, sizeof(req));
	req.file_id = g_file_id;
	req.offset = 0;
	req.file_size = (unsigned long long)size;
	strncpy_s(req.path, sizeof(req.path), dst.c_str(), _TRUNCATE);
	SendPkt(CMD_FILE_UPLOAD_BEGIN, &req, sizeof(req));

	std::vector<char> buf(RC_FILE_CHUNK);
	unsigned long long off = 0;
	DWORD start = GetTickCount();
	for (;;)
	{
		size_t n = fread(buf.data(), 1, buf.size(), fp);
		if (n == 0)
			break;
		std::vector<char> body(sizeof(FileChunk) + n);
		FileChunk c;
		memset(&c, 0, sizeof(c));
		c.file_id = g_file_id;
		c.offset = off;
		c.data_len = (unsigned int)n;
		c.flags = RC_FF_DATA;
		memcpy(body.data(), &c, sizeof(c));
		memcpy(body.data() + sizeof(c), buf.data(), n);
		if (!SendPkt(CMD_FILE_DATA, body.data(), (int)body.size()))   /* 必须带头! */
		{
			fprintf(stderr, "发送中断\n");
			fclose(fp);
			return 1;
		}
		off += n;
	}
	fclose(fp);

	FileChunk end;
	memset(&end, 0, sizeof(end));
	end.file_id = g_file_id;
	end.offset = off;
	end.data_len = 0;
	end.flags = RC_FF_END;
	SendPkt(CMD_FILE_END, &end, sizeof(end));

	int ok = 0;
	for (;;)
	{
		Packet* p = WaitFor(-1, 8000);
		if (p == NULL)
			break;
		if (p->header.cmd == CMD_FILE_PROGRESS)
		{
			std::string msg = BodyStr(p);
			printf("被控端: %s\n", msg.c_str());
			free(p);
			if (msg.find("失败") != std::string::npos)
				return 1;
			ok = 1;
			break;
		}
		if (p->header.cmd == CMD_FILE_END)
		{
			FileChunk c;
			memcpy(&c, p->body, sizeof(c));
			free(p);
			if (c.flags == RC_FF_ERROR)
			{
				fprintf(stderr, "被控端拒绝上传(路径越权?)\n");
				return 1;
			}
			break;
		}
		free(p);
	}
	if (ok)
		printf("已上传 %s -> %s  %llu 字节  %lu ms\n", local, dst.c_str(), off, GetTickCount() - start);
	else
		printf("已上传 %s -> %s  %llu 字节 (未收到确认)\n", local, dst.c_str(), off);
	return 0;
}

static int CmdFileOp(int op, const char* p1, const char* p2)
{
	FileOp fop;
	memset(&fop, 0, sizeof(fop));
	fop.op = op;
	std::string r1 = RemoteResolve(p1);
	strncpy_s(fop.path1, sizeof(fop.path1), r1.c_str(), _TRUNCATE);
	std::string r2 = p2 ? p2 : "";
	if (!r2.empty() && r2.find('\\') == std::string::npos && r2.find('/') == std::string::npos &&
		r2.find(':') == std::string::npos)
	{
		size_t k = r1.find_last_of("\\/");       /* 纯新名字 → 在 path1 同目录内改名 */
		r2 = (k == std::string::npos) ? r2 : (r1.substr(0, k + 1) + r2);
	}
	else
	{
		r2 = RemoteResolve(r2.c_str());
	}
	strncpy_s(fop.path2, sizeof(fop.path2), r2.c_str(), _TRUNCATE);
	SendPkt(CMD_FILE_OP, &fop, sizeof(fop));
	Packet* p = WaitFor(CMD_FILE_OP_RES, 8000);
	if (p == NULL)
	{
		fprintf(stderr, "操作超时\n");
		return 1;
	}
	std::string r = BodyStr(p);
	free(p);
	printf("%s\n", r.c_str());
	return (r.compare(0, 4, "err:") == 0) ? 1 : 0;
}

/* ---------------- 鼠标 / 键盘 ---------------- */
static int CmdMouse(int action, int x, int y)
{
	Mouse m;
	memset(&m, 0, sizeof(m));
	m.action = action;
	m.ptXY.x = x;
	m.ptXY.y = y;
	SendPkt(CMD_MOUSE, &m, sizeof(m));
	printf("鼠标指令已发送 action=%d (%d,%d)\n", action, x, y);
	return 0;
}

static void SendKeyVk(int vk, bool down)
{
	KeyBoard kb;
	kb.virtual_code = vk;
	kb.key_status = down ? RC_KEY_DOWN : RC_KEY_UP;
	SendPkt(CMD_KEYBOARD, &kb, sizeof(kb));
}

static int CmdKey(const char* name)
{
	static const struct { const char* n; int vk; } tbl[] = {
		{"enter", VK_RETURN}, {"return", VK_RETURN}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
		{"tab", VK_TAB}, {"space", VK_SPACE}, {"backspace", VK_BACK}, {"delete", VK_DELETE},
		{"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
		{"home", VK_HOME}, {"end", VK_END}, {"pgup", VK_PRIOR}, {"pgdn", VK_NEXT},
		{"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"shift", VK_SHIFT}, {"win", VK_LWIN},
		{"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4}, {"f5", VK_F5}, {"f6", VK_F6},
		{"f7", VK_F7}, {"f8", VK_F8}, {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
	};
	std::string k = name;
	for (size_t i = 0; i < k.size(); i++)
		k[i] = (char)tolower((unsigned char)k[i]);

	int vk = -1;
	for (int i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++)
		if (k == tbl[i].n) { vk = tbl[i].vk; break; }
	if (vk < 0 && k.size() == 1)
	{
		SHORT r = VkKeyScanA(k[0]);
		if (r != -1) vk = r & 0xFF;
	}
	if (vk < 0)
	{
		fprintf(stderr, "未知按键: %s\n", name);
		return 1;
	}
	SendKeyVk(vk, true);
	Sleep(20);
	SendKeyVk(vk, false);
	printf("按键已发送: %s (vk=%d)\n", name, vk);
	return 0;
}

static int CmdText(const char* s)
{
	int n = 0;
	for (const char* p = s; *p != 0; p++)
	{
		unsigned char ch = (unsigned char)*p;
		if (ch == '\r')
			continue;
		SHORT r = VkKeyScanA((char)ch);
		if (r == -1)
			continue;
		int vk = r & 0xFF;
		bool needShift = (r & 0x100) != 0;
		bool needCtrl = (r & 0x200) != 0;
		if (needShift) SendKeyVk(VK_SHIFT, true);
		if (needCtrl) SendKeyVk(VK_CONTROL, true);
		SendKeyVk(vk, true);
		Sleep(10);
		SendKeyVk(vk, false);
		if (needCtrl) SendKeyVk(VK_CONTROL, false);
		if (needShift) SendKeyVk(VK_SHIFT, false);
		Sleep(15);
		n++;
	}
	printf("已输入 %d 个字符\n", n);
	return 0;
}

/* ---------------- 帮助 ---------------- */
static void Usage()
{
	printf(
		"RemoteControl 命令行控制端 rccli\n"
		"用法: rccli [--host H] [--port N] <命令> [参数]\n"
		"默认: --host 127.0.0.1 --port 9527\n"
		"\n"
		"屏幕:\n"
		"  shot [文件] [--png|--jpeg] [--quality N]    截屏保存(默认 rc_shot.jpg)\n"
		"  watch [帧数] [--save 文件] [--jpeg] [--q N] 连续抓帧并统计(默认 30 帧)\n"
		"  screen <fps> <quality> <png|jpeg>           设置采集参数\n"
		"文件:\n"
		"  ls [远程路径]                               列目录\n"
		"  get <远程文件> [本地文件]                   下载\n"
		"  put <本地文件> [远程文件]                   上传\n"
		"  mkdir <远程路径>                            新建目录\n"
		"  rename <旧路径> <新路径>                    重命名\n"
		"  rm <路径>                                   删除(文件或空目录)\n"
		"控制:\n"
		"  mouse <动作> <x> <y>  1=移动 2=左按下 3=左抬起 4=右按下 5=右抬起\n"
		"                        6=中按下 7=中抬起 8=左单击 9=右单击 10=中单击\n"
		"  key <键名>            例如 key enter / key f5 / key ctrl\n"
		"  text <字符串>         逐字符键入\n"
		"  ping                  连接测试并测延迟\n");
}

int main(int argc, char** argv)
{
	const char* host = "127.0.0.1";
	int port = 9527;
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		if (a == "--host" && i + 1 < argc) { host = argv[++i]; continue; }
		if (a == "--port" && i + 1 < argc) { port = atoi(argv[++i]); continue; }
		if (a == "-h" || a == "--help") { Usage(); return 0; }
		args.push_back(a);
	}
	if (args.empty())
	{
		Usage();
		return 1;
	}
	if (!ConnectTo(host, port))
		return 1;

	std::string cmd = args[0];
	int rc = 0;

	if (cmd == "ping")
	{
		DWORD t = GetTickCount();
		SendPkt(CMD_TESTCONNECT, NULL, 0);
		Packet* p = WaitFor(CMD_TESTCONNECT, 5000);
		if (p == NULL)
		{
			fprintf(stderr, "%s:%d 无响应\n", host, port);
			rc = 1;
		}
		else
		{
			printf("%s:%d 在线  延迟 %lu ms\n", host, port, GetTickCount() - t);
			free(p);
		}
	}
	else if (cmd == "shot")
	{
		std::string out = "rc_shot.jpg";
		int quality = 70, encode = RC_ENC_JPEG;
		for (size_t i = 1; i < args.size(); i++)
		{
			if (args[i] == "--png") { encode = RC_ENC_PNG; }
			else if (args[i] == "--jpeg") encode = RC_ENC_JPEG;
			else if (args[i] == "--quality" && i + 1 < args.size()) quality = atoi(args[++i].c_str());
			else if (args[i][0] != '-') out = args[i];
		}
		if (encode == RC_ENC_PNG && out.size() > 4 && out.compare(out.size() - 4, 4, ".png") != 0
			&& out.find('.') == std::string::npos)
			out += ".png";
		rc = SaveShot(out.c_str(), quality, encode, true) ? 0 : 1;
	}
	else if (cmd == "watch")
	{
		int frames = 30, quality = 70, encode = RC_ENC_JPEG, interval = 0;
		const char* save = NULL;
		for (size_t i = 1; i < args.size(); i++)
		{
			if (args[i] == "--save" && i + 1 < args.size()) save = args[++i].c_str();
			else if (args[i] == "--jpeg") encode = RC_ENC_JPEG;
			else if (args[i] == "--png") encode = RC_ENC_PNG;
			else if ((args[i] == "--q" || args[i] == "--quality") && i + 1 < args.size()) quality = atoi(args[++i].c_str());
			else if (args[i] == "--interval" && i + 1 < args.size()) interval = atoi(args[++i].c_str());
			else if (args[i][0] != '-') frames = atoi(args[i].c_str());
		}
		rc = CmdWatch(frames, interval, save, quality, encode);
	}
	else if (cmd == "screen")
	{
		if (args.size() < 4) { Usage(); rc = 1; }
		else
		{
			int enc = (args[3] == "png" || args[3] == "PNG") ? RC_ENC_PNG : RC_ENC_JPEG;
			SendScreenCtrl(atoi(args[1].c_str()), atoi(args[2].c_str()), enc, 0);
			printf("已设置 fps=%s quality=%s encode=%s\n",
				args[1].c_str(), args[2].c_str(), enc == RC_ENC_PNG ? "png" : "jpeg");
		}
	}
	else if (cmd == "ls")
		rc = CmdLs(args.size() > 1 ? args[1].c_str() : "");
	else if (cmd == "get")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdGet(args[1].c_str(), args.size() > 2 ? args[2].c_str() : NULL);
	}
	else if (cmd == "put")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdPut(args[1].c_str(), args.size() > 2 ? args[2].c_str() : NULL);
	}
	else if (cmd == "mkdir")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdFileOp(RC_OP_MKDIR, args[1].c_str(), NULL);
	}
	else if (cmd == "rename")
	{
		if (args.size() < 3) { Usage(); rc = 1; }
		else rc = CmdFileOp(RC_OP_RENAME, args[1].c_str(), args[2].c_str());
	}
	else if (cmd == "rm")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdFileOp(RC_OP_DELETE, args[1].c_str(), NULL);
	}
	else if (cmd == "mouse")
	{
		if (args.size() < 4) { Usage(); rc = 1; }
		else rc = CmdMouse(atoi(args[1].c_str()), atoi(args[2].c_str()), atoi(args[3].c_str()));
	}
	else if (cmd == "key")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdKey(args[1].c_str());
	}
	else if (cmd == "text")
	{
		if (args.size() < 2) { Usage(); rc = 1; }
		else rc = CmdText(args[1].c_str());
	}
	else
	{
		fprintf(stderr, "未知命令: %s\n", cmd.c_str());
		Usage();
		rc = 1;
	}

	SendPkt(CMD_DISCONNECT, NULL, 0);
	Sleep(50);
	closesocket(g_sock);
	WSACleanup();
	return rc;
}
