/* ============================================================
 *  Phase 4 协议扩展: 系统声音回传 / 摄像头 / 麦克风 / 控制端本地录制
 *  由 common/protocol.h 末尾 #include 引入, 控制端与被控端共用
 * ============================================================ */
#ifndef RC_PHASE4_H
#define RC_PHASE4_H

/* HelloInfo.caps 新增能力位 */
#define RC_CAP_AUDIO     64    /* 系统声音回传 (WASAPI 环回采集) */
#define RC_CAP_CAMERA   128    /* 摄像头画面 */
#define RC_CAP_MIC      256    /* 麦克风回传 */
#define RC_CAP_LOCALREC 512    /* 控制端本地录制/回放 */

/* 新增命令号 (60~79 扩展区) */
enum CMD_PHASE4
{
	CMD_AUDIO         = 70,   /* 控制端->被控端: AudioReq 开始/停止/查询 */
	CMD_AUDIO_FRAME   = 71,   /* 被控端->控制端: AudioFrameHead + PCM */
	CMD_AUDIO_RES     = 72,   /* 被控端->控制端: 文本状态 */
	CMD_CAMERA        = 73,   /* 控制端->被控端: CameraReq */
	CMD_CAMERA_FRAME  = 74,   /* 被控端->控制端: CameraFrameHead + JPEG */
	CMD_CAMERA_RES    = 75,
	CMD_MIC           = 76,   /* 控制端->被控端: AudioReq (麦克风) */
	CMD_MIC_FRAME     = 77,   /* 被控端->控制端: AudioFrameHead + PCM */
	CMD_MIC_RES       = 78
};

#pragma pack(push,1)

enum RC_AUD_OP
{
	RC_AUD_STOP  = 0,
	RC_AUD_START = 1,
	RC_AUD_QUERY = 2,
	RC_AUD_LIST  = 3     /* 列出该方向的音频设备 */
};
struct AudioReq          /* CMD_AUDIO / CMD_MIC 请求体 */
{
	int op;              /* RC_AUD_xxx */
	int buf_ms;          /* 每包时长(ms), 默认 40 */
	int want_rate;       /* 期望采样率, 0=设备原生 */
	int want_ch;         /* 期望声道数, 0=原生 */
	int want_bits;       /* 期望位深, 0=统一转 16bit */
	int dev;             /* 设备序号: 0=系统默认, N=设备列表第 N 个 */
	int gain;            /* 音量百分比 10~800, 100=原样 (麦克风常用) */
};

enum RC_AUD_FRAME_FLAG { RC_AF_SILENCE = 1, RC_AF_FIRST = 2 };
struct AudioFrameHead    /* 音频帧头, 后接 PCM(16bit 小端交织) */
{
	int seq;
	int rate;
	int ch;
	int bits;
	int bytes;           /* 后续 PCM 字节数 */
	int flags;           /* RC_AF_xxx */
};

enum RC_CAM_OP { RC_CAM_STOP = 0, RC_CAM_START = 1, RC_CAM_QUERY = 2 };
struct CameraReq         /* CMD_CAMERA 请求体 */
{
	int op;              /* RC_CAM_xxx */
	int index;           /* 摄像头序号, 0 = 默认 */
	int width;           /* 期望宽, 0 = 原生 */
	int height;
	int fps;             /* 帧率, 默认 10 */
	int quality;         /* JPEG 质量 1~100, 默认 75 */
};

#define RC_CAM_FMT_JPEG 1
struct CameraFrameHead   /* 摄像头帧头, 后接图像数据 */
{
	int seq;
	int index;
	int w;
	int h;
	int format;          /* RC_CAM_FMT_xxx */
	int bytes;
};

#pragma pack(pop)

#endif /* RC_PHASE4_H */
