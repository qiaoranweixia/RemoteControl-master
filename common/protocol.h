#pragma once
/*
 * ============================================================
 *  公共通信协议 v2  (被控端 RemoteControl / 控制端 client 共用)
 *  包结构: [ magic(4) | cmd(4) | body_len(4) | body(body_len) ]
 *  本文件不依赖 windows.h / winsock，可单独使用
 * ============================================================
 */
#define RC_MAGIC         0x12345678
#define RC_PROTOCOL_VER  3
#define RC_MAX_BODY      (16 * 1024 * 1024)  /* 单包 body 上限，防御性校验 */
#define RC_FILE_CHUNK    (64 * 1024)         /* 文件分块大小 */
#define RC_IDLE_TIMEOUT  30000               /* 无数据超时(ms) */
#define RC_HB_INTERVAL   5000                /* 心跳发送间隔(ms) */
#define RC_HB_TIMEOUT    20000               /* 心跳超时判定掉线(ms) */

/* ---- 局域网设备发现: 控制端 UDP 广播 "RC_DISCOVER_V1", 被控端单播回 DiscReply ---- */
#define RC_DISC_PORT   9998
#define RC_DISC_MAGIC  0x12345679
#define RC_DISC_REQ    "RC_DISCOVER_V1"
#pragma pack(push,1)
struct DiscReply
{
	int            magic;   /* RC_DISC_MAGIC */
	int            proto;   /* 被控端协议版本 */
	unsigned short port;    /* 被控端 TCP 监听端口 */
	unsigned short rsv;     /* 保留 */
	char           name[64];/* 被控端主机名 */
};
#pragma pack(pop)

#include <string>
#include <cstring>
#include <cstdlib>

#pragma pack(push,1)
struct PacketHeader
{
	int magic;     /* 4字节包头 0x12345678 */
	int cmd;       /* 4字节命令号 */
	int body_len;  /* 数据长度 */
};
struct FileChunk   /* 文件数据块头 (位于 body 起始处) */
{
	unsigned int       file_id;   /* 传输会话ID */
	unsigned long long offset;    /* 本块在文件中的偏移 */
	unsigned int       data_len;  /* 后续负载长度 */
	unsigned int       flags;     /* RC_FF_xxx */
};
struct FileRequest /* 文件请求 (CMD_FILE_DOWNLOAD / CMD_FILE_UPLOAD_BEGIN) */
{
	unsigned int       file_id;
	unsigned long long offset;    /* 起始偏移，支持断点续传 */
	unsigned long long file_size; /* 文件总大小(未知填0) */
	char               path[520];
};
struct FileOp      /* CMD_FILE_OP */
{
	int  op;            /* RC_OP_xxx */
	char path1[520];
	char path2[520];    /* 重命名目标 / 忽略 */
};
#pragma pack(pop)

struct Packet
{
	PacketHeader header;
	char body[];   /* 柔性数组 */
};

/* ---------------- 命令号 ---------------- */
enum CMD
{
	CMD_SCREEN = 1,          /* 请求/响应屏幕数据(客户端→服务端请求) */
	CMD_MOUSE = 2,           /* 鼠标事件(客户端→服务端) */
	CMD_KEYBOARD = 4,        /* 键盘事件(客户端→服务端) */
	CMD_HEARTBEAT = 6,       /* 心跳(双向) */
	CMD_DISCONNECT = 8,      /* 主动断开 */

	/* 文件传输 10~19 */
	CMD_FILE_LIST = 10,      /* req: 路径字符串 → res: CMD_FILE_LIST_RES */
	CMD_FILE_LIST_RES = 11,  /* res: 文本列表 */
	CMD_FILE_DOWNLOAD = 12,  /* req: FileRequest，服务端回 CMD_FILE_DATA 流 */
	CMD_FILE_UPLOAD_BEGIN = 13, /* req: FileRequest，开始上传 */
	CMD_FILE_DATA = 14,      /* FileChunk + 负载 (双向) */
	CMD_FILE_END = 15,       /* FileChunk(flags 标记结束/错误) */
	CMD_FILE_OP = 16,        /* FileOp: 删除/新建目录/重命名 */
	CMD_FILE_OP_RES = 17,    /* res: 字符串 ok / err:xxx */
	CMD_FILE_PROGRESS = 18,  /* res: 传输进度/结果文本 */

	/* 系统管理 20~49 (Phase 2 预留) */
	CMD_SHELL_EXEC = 20,
	CMD_SHELL_RESULT = 21,
	CMD_SYSINFO = 30,
	CMD_SYSINFO_RES = 31,
	CMD_PROC_LIST = 32,
	CMD_PROC_LIST_RES = 33,
	CMD_PROC_KILL = 34,
	CMD_POWER = 35,
	CMD_SCREEN_CTRL = 40,    /* ScreenCtrl 画质/帧率控制 */
	CMD_CLIPBOARD = 50,      /* 剪贴板同步: ClipReq(op) [+ 文本] */
	CMD_CLIPBOARD_RES = 51,  /* res: 剪贴板文本 */

	/* v9: 剪贴板/文件拖拽 虚拟通道(状态机驱动, 见 RC_CLIP_MSG)
	   —— 与 RDP CLIPRDR 同构: offer(格式列表) → request → data, 传输期间 lock/unlock 防回环。
	   文本/位图走内存;CF_HDROP 文件列表只先发元数据(延迟渲染), 粘贴/拖出时才按需取内容。 */
	CMD_CLIP_CHANNEL = 52,   /* body = ClipMsg [+ payload] */
	CMD_CLIP_LIST    = 53,   /* body = ClipMsg + N×ClipFileDesc(文件清单应答) */

	/* 远程控制扩展 60~79 */
	CMD_CURSOR = 60,         /* 定位: 查询被控端光标位置/屏幕尺寸 */
	CMD_CURSOR_RES = 61,     /* res: CursorInfo */
	CMD_INPUT_REC = 62,      /* 键鼠录制: InputRecReq 开始/停止/状态 */
	CMD_INPUT_LOG = 63,      /* res: InputLog 头 + 记录文本(可分块) */

	/* 输入增强 64~69 (v5: 本地化操控手感) */
	CMD_KEYTEXT  = 64,       /* 文本直投: body = UTF-8 文本 → 被控端按 Unicode 逐字上屏(中文/表情可用) */
	CMD_MOUSEABS = 65,       /* 预留: 绝对坐标鼠标(当前走 CMD_MOUSE) */

	CMD_TESTCONNECT = 200,

	/* 版本握手: 控制端连接成功后发送 CMD_HELLO (body 可为空或 int 协议版本),
	   被控端回 CMD_HELLO_RES (body = HelloInfo)。旧版被控端不认识该命令、不回包,
	   控制端据此在界面上明确提示"被控端版本过旧"，避免"命令发出去没回包"的困惑。 */
	CMD_HELLO     = 201,
	CMD_HELLO_RES = 202,

	/* v8: 开机自启管理(被控端用「计划任务: 登录时以最高权限启动」实现,
	   这样开机自启的实例天然是管理员权限, C盘写入/UIPI 操控问题一并解决) */
	CMD_AUTOSTART     = 210,  /* req: int op  0=关闭自启 1=开启自启 2=查询状态 */
	CMD_AUTOSTART_RES = 211   /* res: GBK 文本描述(控制端直接打印) */
};

#define RC_PROTO_VERSION 5   /* 5 = 增加剪贴板同步虚拟通道/文件剪贴板/拖拽(4 = 系统声音/摄像头/麦克风) */

/* HelloInfo.caps 能力位 */
#define RC_CAP_SCREEN     1
#define RC_CAP_FILE       2
#define RC_CAP_SYSINFO    4
#define RC_CAP_CURSOR     8
#define RC_CAP_INPUTREC  16
#define RC_CAP_CLIPBOARD 32
#define RC_CAP_CLIPADV   64   /* v9: 剪贴板虚拟通道(自动同步/文件剪贴板/拖拽) */

struct HelloInfo
{
	int  proto_version;   /* 被控端协议版本 */
	int  caps;            /* RC_CAP_xxx 能力位或 */
	char build[64];       /* 版本描述(GBK 文本) */
};

/* 鼠标动作：右键、左键、中间   状态：按下、放开、双击 */
enum ENUM_MOUSE
{
	MOUSE_MOVE = 1,
	MOUSE_LDOWN = 2,
	MOUSE_LUP = 3,
	MOUSE_RDOWN = 4,
	MOUSE_RUP = 5,
	MOUSE_MDOWN = 6,
	MOUSE_MUP = 7,
	MOUSE_LCLICK = 8,
	MOUSE_RCLICK = 9,
	MOUSE_MCLICK = 10,
	MOUSE_LDCLICK = 11,
	MOUSE_RDCLICK = 12,
	MOUSE_MDCLICK = 13,
	MOUSE_WHEEL   = 14,   /* 垂直滚轮, delta 用 Mouse.delta(通常 ±120) */
	MOUSE_HWHEEL  = 15,   /* 水平滚轮 */
	MOUSE_X1DOWN  = 16,   /* 侧键1(后退) */
	MOUSE_X1UP    = 17,
	MOUSE_X2DOWN  = 18,   /* 侧键2(前进) */
	MOUSE_X2UP    = 19
};

enum RC_FILE_FLAG
{
	RC_FF_BEGIN = 1,  /* 开始(携带首块数据) */
	RC_FF_DATA  = 2,  /* 中间数据块 */
	RC_FF_END   = 4,  /* 传输结束 */
	RC_FF_ERROR = 8   /* 出错，body 为错误描述 */
};

enum RC_FILE_OP
{
	RC_OP_DELETE = 1,
	RC_OP_MKDIR  = 2,
	RC_OP_RENAME = 3
};

enum RC_SCREEN_ENCODE { RC_ENC_PNG = 0, RC_ENC_JPEG = 1 };

struct RC_Point { int x; int y; };
/* v5: Mouse 增加 delta(滚轮增量 ±120)；KeyBoard 增加扫描码与扩展键标志，
   使被控端能以「扫描码+VK+扩展位」完整重放按键(方向键/小键盘/Alt组合/游戏都能正确响应)。 */
struct Mouse    { int action; RC_Point ptXY; int delta; };
struct KeyBoard { int virtual_code; int key_status; unsigned int scan_code; unsigned int flags; };
/* KeyBoard.flags */
#define RC_KF_EXTENDED 1u   /* 扩展键(方向键/Insert/Delete/小键盘回车/RAlt 等, 对应 lParam bit24) */
struct ScreenCtrl { int fps; int quality; int encode; int monitor; int force_full; };
/* encode: RC_ENC_xxx，quality 1~100(JPEG)，force_full!=0 → 被控端下发整屏(客户端首次/重连后建立基准帧) */

/*
 * 屏幕帧头 v3：CMD_SCREEN 的 body = ScreenFrameHead + 图像数据
 *  - w==0 && h==0 : 画面无变化（无图像数据，客户端直接跳过）
 *  - flags & RC_FR_FULL    : 整屏帧，(x,y)=(0,0)，客户端直接替换缓冲
 *  - flags & RC_FR_PARTIAL : 增量帧，仅 (x,y,w,h) 区域有变化，客户端把解码后的子图贴到对应位置
 */
struct ScreenFrameHead
{
	int x;        /* 本帧图像在整屏中的左上角 */
	int y;
	int w;        /* 本帧图像宽高；0 表示无变化 */
	int h;
	int full_w;   /* 整屏尺寸 */
	int full_h;
	int format;   /* RC_ENC_xxx */
	int flags;    /* RC_FR_xxx */
};

enum RC_FRAME_FLAG
{
	RC_FR_FULL    = 1,   /* 整屏帧 */
	RC_FR_PARTIAL = 2,   /* 增量帧 */
	RC_FR_NODATA  = 4    /* 无变化，无图像数据 */
};

#pragma pack(push,1)
/* v9: 剪贴板虚拟通道消息头(CMD_CLIP_CHANNEL 的 body 起始处) */
struct ClipMsg
{
	int                msg;     /* RC_CM_xxx */
	int                fmt;     /* RC_CF_xxx */
	unsigned int       seq;     /* 序号 / 文件索引(OFFER 时) */
	unsigned long long offset;  /* 分块偏移(REQUEST/DATA 时) */
	unsigned long long size;    /* 内容/文件总大小 */
	int                last;    /* 1 = 末块 */
	int                count;   /* 文件个数(OFFER 且含 RC_CF_FILES) */
	int                flags;   /* 保留(能力位) */
};
/* 文件清单项: OFFER 时紧跟 ClipMsg 之后 count 个 */
struct ClipFileDesc
{
	unsigned long long size;    /* 文件大小(目录填 0) */
	unsigned int       mtime;   /* 修改时间(Unix 秒, 0=未知) */
	unsigned int       is_dir;  /* 1 = 目录 */
	char               name[520];/* 路径: 被控端侧为绝对路径; 控制端侧为本地路径 */
};
struct CursorInfo      /* CMD_CURSOR_RES: 被控端光标位置 + 屏幕尺寸 */
{
	int x;
	int y;
	int screen_w;
	int screen_h;
	int monitor_w;     /* 主显示器宽高(与光标坐标系一致) */
	int monitor_h;
};
enum RC_REC_FLAG       /* InputRecReq.flags 位或 */
{
	RC_REC_MOUSE = 1,  /* 记录鼠标移动/按键 */
	RC_REC_KEY   = 2   /* 记录键盘按键 */
};
struct InputRecReq     /* CMD_INPUT_REC: duration_ms=0 且 stop=1 表示停止 */
{
	int flags;         /* RC_REC_xxx，0 = 仅查询状态 */
	int duration_ms;   /* 录制时长，0 = 手动停止 */
	int sample_ms;     /* 采样间隔，默认 15ms */
	int stop;          /* 1 = 请求停止当前录制 */
};
struct InputLog        /* CMD_INPUT_LOG 头，后接文本块 */
{
	int index;         /* 块序号，从 0 开始 */
	int last;          /* 1 = 最后一块 */
};
struct ClipReq         /* CMD_CLIPBOARD 体 */
{
	int op;            /* RC_CLIP_xxx */
};
enum RC_CLIP_OP
{
	RC_CLIP_GET = 0,   /* 读取被控端剪贴板(回 CMD_CLIPBOARD_RES) */
	RC_CLIP_SET = 1,   /* 设置被控端剪贴板(body 后接 UTF-8 文本) */
	RC_CLIP_CLEAR = 2  /* 清空 */
};

/* ---- v9: 剪贴板虚拟通道状态机(CmdClipChannel) ---- */
enum RC_CLIP_MSG
{
	RC_CM_HELLO   = 1,  /* 能力协商: flags = RC_CF_xxx 支持位(位或) */
	RC_CM_OFFER   = 2,  /* 本端剪贴板变了(延迟渲染): fmt + size + count(文件数) */
	RC_CM_REQUEST = 3,  /* 请求对端数据: fmt(+ seq 为文件索引) */
	RC_CM_DATA    = 4,  /* 数据分块: offset/size/last + payload */
	RC_CM_LOCK    = 5,  /* 锁定剪贴板(传输期间, 防同步回环, 同 CLIPRDR lock_clipboard) */
	RC_CM_UNLOCK  = 6,  /* 解锁 */
	RC_CM_ABORT   = 7,  /* 失败/中止: payload = GBK 说明文本 */
	RC_CM_PASTE   = 8   /* 请求对端把当前剪贴板内容直接粘贴到其前台应用(模拟 Ctrl+V) */
};
/* 剪贴板格式位(RC_CM_HELLO.flags / ClipMsg.fmt) */
enum RC_CLIP_FMT
{
	RC_CF_TEXT    = 1,   /* CF_TEXT (ANSI 文本, 传输时统一 UTF-8) */
	RC_CF_RTF     = 2,   /* CF_RTF */
	RC_CF_HTML    = 4,   /* HTML Format */
	RC_CF_PNG     = 8,   /* PNG(自定义格式, 跨端无损) */
	RC_CF_FILES   = 16,  /* CF_HDROP 文件列表(延迟渲染) */
	RC_CF_DIB     = 32,  /* CF_DIB(V5 头 + 像素) */
	RC_CF_UNICODE = 64   /* CF_UNICODETEXT */
};
#pragma pack(pop)

/* ---------- Phase 4 扩展: 系统声音/摄像头/麦克风/控制端本地录制 ---------- */
#include "rc_phase4.h"

#define RC_KEY_DOWN 0
#define RC_KEY_UP   2   /* KEYEVENTF_KEYUP */

/* ---------------- 打包 / 解包 ---------------- */
inline int GetPacketLen(Packet* pck)
{
	if (pck != NULL)
		return pck->header.body_len + (int)sizeof(PacketHeader);
	return 0;
}

inline Packet* PackPacket(int cmd, const char* buffer, int buffer_len)
{
	Packet* pck = (Packet*)malloc(sizeof(PacketHeader) + (buffer_len > 0 ? buffer_len : 0));
	pck->header.magic = RC_MAGIC;
	pck->header.cmd = cmd;
	pck->header.body_len = buffer_len;
	if (buffer_len > 0 && buffer != NULL)
		memcpy(pck->body, buffer, buffer_len);
	return pck;
}

/*
 * 流式拆包器：解决 TCP 粘包/半包
 *  - Append(): 把 recv 到的原始字节追加进缓冲
 *  - Next()  : 取出一个完整包(malloc，调用方 free)，无完整包返回 NULL
 */
class PacketStream
{
public:
	void Append(const char* data, int len)
	{
		if (len > 0)
			m_buf.append(data, len);
	}

	Packet* Next()
	{
		for (;;)
		{
			/* 1. 重新同步魔数 */
			size_t pos = m_buf.find("\x78\x56\x34\x12", 0, 4); /* 0x12345678 小端 */
			if (pos == std::string::npos)
			{
				/* 保留最后 3 字节(可能是魔数前缀)，其余丢弃 */
				if (m_buf.size() > 3)
					m_buf.erase(0, m_buf.size() - 3);
				return NULL;
			}
			if (pos > 0)
				m_buf.erase(0, pos);
			if (m_buf.size() < sizeof(PacketHeader))
				return NULL;

			/* 2. 读包头并校验 */
			PacketHeader h;
			memcpy(&h, m_buf.data(), sizeof(PacketHeader));
			if (h.body_len < 0 || h.body_len > RC_MAX_BODY)
			{
				m_buf.erase(0, 1); /* 伪包头，滑动一字节继续找 */
				continue;
			}

			/* 3. 完整包？ */
			size_t total = sizeof(PacketHeader) + (size_t)h.body_len;
			if (m_buf.size() < total)
				return NULL;

			Packet* pck = (Packet*)malloc(total);
			memcpy(pck, m_buf.data(), total);
			m_buf.erase(0, total);
			return pck;
		}
	}

	size_t Buffered() const { return m_buf.size(); }
	void   Reset() { m_buf.clear(); }

private:
	std::string m_buf;
};
