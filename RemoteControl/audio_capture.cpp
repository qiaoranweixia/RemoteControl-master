// ============================================================
//  Phase 4 音频回传 (服务端):
//    系统声音: 播放设备环回采集 (WASAPI LOOPBACK) -> CMD_AUDIO_FRAME
//    麦克风  : 录音设备直接采集 (WASAPI capture)  -> CMD_MIC_FRAME
//  两者独立线程、可同时运行; 设备/增益/包长由控制端选择
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"cfgmgr32.lib")
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <cstdio>
#include "..\common\protocol.h"

#pragma comment(lib,"ole32.lib")

/* ---------- 主程序(RemoteControl.cpp) 提供的接口 ---------- */
extern SOCKET g_client_socket;
void Log(const char* fmt, ...);
bool SendCmd(SOCKET s, int cmd, const char* body, int len);
bool SendText(SOCKET s, int cmd, const std::string& text);

/* ------------------------------------------------------------------ */
/*  采集源描述: 系统声音 / 麦克风 复用同一套采集代码                    */
/* ------------------------------------------------------------------ */
struct AudioSrc
{
	const char* name;      /* 日志名: audio / mic */
	const char* label;     /* 中文名: 系统声音 / 麦克风 */
	EDataFlow   flow;      /* eRender = 播放设备环回, eCapture = 录音设备 */
	DWORD       sflags;    /* AUDCLNT_STREAMFLAGS_LOOPBACK 或 0 */
	int         cmdFrame;  /* 音频帧命令号 */
	int         cmdRes;    /* 状态文本命令号 */
	volatile LONG want;    /* 1 = 采集线程继续 */
	volatile LONG seq;     /* 已发送帧数 */
	volatile LONG bytes;   /* 已发送字节 */
	volatile LONG running; /* 线程存活标志 */
	int         bufms;     /* 每包时长(ms) */
	int         dev;       /* 0 = 默认设备, N = 设备列表第 N 个 */
	int         gain;      /* 音量百分比, 100 = 原样 */
	HANDLE      thread;
	char        info[192];
};

static AudioSrc g_sys =
{ "audio", "系统声音", eRender, AUDCLNT_STREAMFLAGS_LOOPBACK, CMD_AUDIO_FRAME, CMD_AUDIO_RES,
  0, 0, 0, 0, 40, 0, 100, NULL, "not started" };

static AudioSrc g_mic =
{ "mic", "麦克风", eCapture, 0, CMD_MIC_FRAME, CMD_MIC_RES,
  0, 0, 0, 0, 40, 0, 100, NULL, "not started" };

enum SampleKind { SK_UNKNOWN = 0, SK_F32, SK_S16, SK_S32, SK_S24, SK_S8 };

/* ---------- 宽字符 -> UTF8 ---------- */
static std::string W2U(const wchar_t* w)
{
	if (w == NULL) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 1) return std::string();
	std::string s((size_t)n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
	return s;
}

/* ---------- PCM 格式识别 / 转换 ---------- */
static SampleKind KindFromBits(int bits, bool isFloat)
{
	if (isFloat)   return SK_F32;
	if (bits == 16) return SK_S16;
	if (bits == 32) return SK_S32;
	if (bits == 24) return SK_S24;
	if (bits == 8)  return SK_S8;
	return SK_UNKNOWN;
}

static SampleKind DetectKind(const WAVEFORMATEX* f)
{
	if (f == NULL) return SK_UNKNOWN;
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return SK_F32;
	if (f->wFormatTag == WAVE_FORMAT_PCM)        return KindFromBits(f->wBitsPerSample, false);
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
	{
		const WAVEFORMATEXTENSIBLE* we = (const WAVEFORMATEXTENSIBLE*)f;
		DWORD sub = we->SubFormat.Data1;   /* 1=PCM, 3=IEEE_FLOAT */
		return KindFromBits(f->wBitsPerSample, sub == WAVE_FORMAT_IEEE_FLOAT);
	}
	return SK_UNKNOWN;
}

/* 任意格式 -> 16bit 小端 PCM, 返回写入 sample 数 */
static int ConvertToS16(const BYTE* src, UINT32 frames, int ch, SampleKind kind, short* dst)
{
	int n = (int)frames * ch;
	int i;
	switch (kind)
	{
	case SK_F32:
		{
			const float* p = (const float*)src;
			for (i = 0; i < n; ++i)
			{
				float v = p[i];
				if (v > 1.0f)  v = 1.0f;
				if (v < -1.0f) v = -1.0f;
				dst[i] = (short)(v * 32767.0f);
			}
		}
		break;
	case SK_S16:
		memcpy(dst, src, (size_t)n * 2);
		break;
	case SK_S32:
		{
			const int* p = (const int*)src;
			for (i = 0; i < n; ++i) dst[i] = (short)(p[i] >> 16);
		}
		break;
	case SK_S24:
		{
			for (i = 0; i < n; ++i)
			{
				const BYTE* b = src + (size_t)i * 3;
				int v = b[0] | (b[1] << 8) | (b[2] << 16);
				if (v & 0x800000) v |= ~0xFFFFFF;   /* 符号扩展 */
				dst[i] = (short)(v >> 16);
			}
		}
		break;
	case SK_S8:
		{
			const BYTE* p = src;
			for (i = 0; i < n; ++i) dst[i] = (short)((int)(p[i] - 128) << 8);
		}
		break;
	default:
		memset(dst, 0, (size_t)n * 2);
		break;
	}
	return n;
}

/* 音量增益(麦克风常用): gain 为百分比, 100 = 原样 */
static void ApplyGain(short* p, int n, int gain)
{
	int i;
	if (gain == 100 || gain <= 0) return;
	for (i = 0; i < n; ++i)
	{
		int v = (int)p[i] * gain / 100;
		if (v > 32767)  v = 32767;
		if (v < -32768) v = -32768;
		p[i] = (short)v;
	}
}

/* ---------- 设备枚举 / 选择 ---------- */
static std::string DeviceLabel(IMMDevice* pDev)
{
	std::string r;
	IPropertyStore* ps = NULL;
	if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &ps)) && ps != NULL)
	{
		PROPVARIANT pv;
		PropVariantInit(&pv);
		if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
			r = W2U(pv.pwszVal);
		PropVariantClear(&pv);
		ps->Release();
	}
	return r;
}

/* 列出 flow 方向的设备(含未插入/已禁用), 返回多行文本; 序号与 PickDevice 一致 */
static std::string ListEndpoints(EDataFlow flow)
{
	std::string out;
	IMMDeviceEnumerator* pEnum = NULL;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* 处理线程未必初始化过 COM(MTA) */
	IMMDeviceCollection* pc = NULL;
	IMMDevice*           pDef = NULL;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), (void**)&pEnum);
	if (FAILED(hr) || pEnum == NULL) return std::string("err:无法创建音频设备枚举器\n");
	pEnum->GetDefaultAudioEndpoint(flow, eConsole, &pDef);
	std::string defId;
	if (pDef != NULL)
	{
		LPWSTR id = NULL;
		if (SUCCEEDED(pDef->GetId(&id)) && id != NULL) { defId = W2U(id); CoTaskMemFree(id); }
	}
	UINT n = 0;
	hr = pEnum->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &pc);
	if (FAILED(hr) || pc == NULL)
	{
		if (pDef) pDef->Release();
		pEnum->Release();
		return std::string("err:枚举音频设备失败\n");
	}
	pc->GetCount(&n);
	char line[640];
	for (UINT i = 0; i < n; ++i)
	{
		IMMDevice* pDev = NULL;
		if (FAILED(pc->Item(i, &pDev)) || pDev == NULL) continue;
		std::string nm = DeviceLabel(pDev);
		std::string sid;
		LPWSTR id = NULL;
		if (SUCCEEDED(pDev->GetId(&id)) && id != NULL) { sid = W2U(id); CoTaskMemFree(id); }
		bool isDef = (!defId.empty() && sid == defId);
		DWORD st = 0;
		if (FAILED(pDev->GetState(&st))) st = 0;
		const char* stag = (st & DEVICE_STATE_ACTIVE) ? "可用"
			: (st & DEVICE_STATE_UNPLUGGED) ? "未插入"
			: (st & DEVICE_STATE_DISABLED) ? "已禁用" : "不存在";
		sprintf_s(line, sizeof(line), "[%u] %s[%s]%s",
			(unsigned)(i + 1), isDef ? "(默认)" : "", stag,
			nm.empty() ? " (未知设备)" : nm.c_str());
		out += line;
		out += "\n";
		pDev->Release();
	}
	if (n == 0)
		out += "(没有任何音频设备。被控端若无声卡/未装驱动, 请安装虚拟声卡如 VB-Cable)\n";
	pc->Release();
	if (pDef) pDef->Release();
	pEnum->Release();
	return out;
}

/* ==================================================================
   v19: 音频可用性自愈
   目标: 无论被控端有没有物理音响 / 设备是否被禁用 / 音频服务是否在跑,
         都尽量把"系统声音"抓出来。
   手段(按顺序):
     1. 确保 AudioEndpointBuilder + Audiosrv 服务处于运行状态(停了就拉起)
     2. 端点候选不再只取"默认设备", 而是 默认(console/multimedia/communications)
        -> 全部端点(含 未插入/已禁用/不存在), 按 可用性 + 虚拟声卡优先 排序
     3. 逐个候选尝试 LOOPBACK, 第一个成功的就用
     4. 全都失败 -> 尝试启用被禁用的音频设备(SetupAPI DICS_ENABLE), 再重试
   ================================================================== */

/* Media 设备类 GUID (直接写常量, 避免 devguid.h 的 GUID 链接方式差异) */
static const GUID RC_GUID_DEVCLASS_MEDIA =
{ 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* 确保 Windows 音频服务在运行: 被停止/禁用时自动拉起
   (被控端以 LocalSystem 服务身份运行时具备该权限; 普通用户下失败也不致命) */
static void EnsureAudioServices()
{
	static const char* names[2] = { "AudioEndpointBuilder", "Audiosrv" };
	SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm == NULL) return;
	for (int i = 0; i < 2; ++i)
	{
		SC_HANDLE s = OpenServiceA(scm, names[i], SERVICE_QUERY_STATUS | SERVICE_START);
		if (s == NULL)
		{
			Log("audio: OpenService(%s) failed %lu", names[i], GetLastError());
			continue;
		}
		SERVICE_STATUS st;
		memset(&st, 0, sizeof(st));
		if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING)
		{
			CloseServiceHandle(s);
			continue;
		}
		if (StartServiceA(s, 0, NULL))
		{
			Log("audio: started service %s (was %lu)", names[i], st.dwCurrentState);
			for (int k = 0; k < 25; ++k)
			{
				Sleep(200);
				if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING) break;
			}
		}
		else
			Log("audio: StartService(%s) failed %lu", names[i], GetLastError());
		CloseServiceHandle(s);
	}
	CloseServiceHandle(scm);
}

/* 把所有处于"已禁用"状态的音频设备重新启用(需要管理员/LocalSystem)
   返回本次启用的设备数量 */
static int EnableDisabledAudioEndpoints()
{
	int changed = 0;
	HDEVINFO h = SetupDiGetClassDevsA(&RC_GUID_DEVCLASS_MEDIA, NULL, NULL,
		DIGCF_PRESENT | DIGCF_ALLCLASSES);
	if (h == INVALID_HANDLE_VALUE) return 0;
	SP_DEVINFO_DATA dd;
	memset(&dd, 0, sizeof(dd));
	dd.cbSize = sizeof(dd);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i)
	{
		ULONG status = 0, problem = 0;
		if (CM_Get_DevNode_Status(&status, &problem, dd.DevInst, 0) != CR_SUCCESS) continue;
		if (problem != CM_PROB_DISABLED && problem != CM_PROB_HARDWARE_DISABLED) continue;
		SP_PROPCHANGE_PARAMS pp;
		memset(&pp, 0, sizeof(pp));
		pp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
		pp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
		pp.StateChange = DICS_ENABLE;
		pp.Scope = DICS_FLAG_GLOBAL;
		pp.HwProfile = 0;
		if (!SetupDiSetClassInstallParamsA(h, &dd, &pp.ClassInstallHeader, sizeof(pp))) continue;
		if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, &dd)) continue;
		pp.Scope = DICS_FLAG_CONFIGSPECIFIC;
		SetupDiSetClassInstallParamsA(h, &dd, &pp.ClassInstallHeader, sizeof(pp));
		SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, &dd);
		++changed;
		Log("audio: re-enabled disabled audio device (index %lu, problem %lu)", i, problem);
	}
	SetupDiDestroyDeviceInfoList(h);
	return changed;
}

/* 设备可用性排名: 越小越优先 */
static int StateRank(DWORD st)
{
	if (st & DEVICE_STATE_ACTIVE)    return 0;
	if (st & DEVICE_STATE_UNPLUGGED) return 1;
	if (st & DEVICE_STATE_DISABLED)  return 2;
	return 3;
}

/* 名称里带这些字的通常是虚拟声卡(ToDesk/NoMachine/VB-Cable/远程桌面等),
   它们一定存在, 且不依赖物理喇叭插没插 */
static bool LooksVirtual(const std::string& n)
{
	static const char* kws[] = { "virtual", "cable", "voicemeeter", "todesk",
		"nomachine", "sunlogin", "parsec", "streaming", "remote audio", "wave out" };
	std::string s;
	s.reserve(n.size());
	for (size_t i = 0; i < n.size(); ++i) s += (char)tolower((unsigned char)n[i]);
	for (int i = 0; i < 10; ++i)
		if (s.find(kws[i]) != std::string::npos) return true;
	return false;
}

struct AudioCand { IMMDevice* dev; int rank; int virt; };

/* 收集候选端点(调用方负责 Release 每个返回的 IMMDevice)
   dev >= 1: 用户从设备列表里选的第 dev 个(与 ListEndpoints 同序, 不做优选)
   dev <= 0: 默认设备优先, 然后按 可用性/虚拟声卡 排序的全部端点 */
static void CollectCandidates(IMMDeviceEnumerator* pEnum, EDataFlow flow, int dev,
	std::vector<IMMDevice*>* out)
{
	if (out == NULL) return;
	IMMDeviceCollection* pc = NULL;
	UINT n = 0;
	if (dev >= 1)
	{
		if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &pc)) && pc != NULL)
		{
			pc->GetCount(&n);
			if ((UINT)dev <= n)
			{
				IMMDevice* d = NULL;
				if (SUCCEEDED(pc->Item((UINT)dev - 1, &d)) && d != NULL) out->push_back(d);
			}
			pc->Release();
		}
		return;
	}

	std::vector<std::string> seen;
	EDataFlow dummyFlow = flow;
	(void)dummyFlow;

	/* 1) 三个角色的默认设备(console / multimedia / communications) */
	const ERole roles[3] = { eConsole, eMultimedia, eCommunications };
	for (int r = 0; r < 3; ++r)
	{
		IMMDevice* d = NULL;
		if (FAILED(pEnum->GetDefaultAudioEndpoint(flow, roles[r], &d)) || d == NULL) continue;
		LPWSTR id = NULL;
		std::string sid;
		if (SUCCEEDED(d->GetId(&id)) && id != NULL) { sid = W2U(id); CoTaskMemFree(id); }
		bool dup = false;
		for (size_t k = 0; k < seen.size(); ++k) if (seen[k] == sid) dup = true;
		if (dup) { d->Release(); continue; }
		seen.push_back(sid);
		out->push_back(d);
	}

	/* 2) 全部端点(含 未插入/已禁用/不存在), 排序后追加 */
	if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &pc)) && pc != NULL)
	{
		pc->GetCount(&n);
		std::vector<AudioCand> cs;
		for (UINT i = 0; i < n; ++i)
		{
			IMMDevice* d = NULL;
			if (FAILED(pc->Item(i, &d)) || d == NULL) continue;
			DWORD st = 0;
			if (FAILED(d->GetState(&st))) st = 0;
			AudioCand c;
			c.dev = d;
			c.rank = StateRank(st);
			c.virt = LooksVirtual(DeviceLabel(d)) ? 1 : 0;
			cs.push_back(c);
		}
		/* 稳定排序: 可用优先, 同档次虚拟声卡优先 */
		for (size_t a = 0; a + 1 < cs.size(); ++a)
			for (size_t b = 0; b + 1 < cs.size() - a; ++b)
			{
				bool sw = false;
				if (cs[b + 1].rank < cs[b].rank) sw = true;
				else if (cs[b + 1].rank == cs[b].rank && cs[b + 1].virt > cs[b].virt) sw = true;
				if (sw) { AudioCand t = cs[b]; cs[b] = cs[b + 1]; cs[b + 1] = t; }
			}
		for (size_t a = 0; a < cs.size(); ++a)
		{
			LPWSTR id = NULL;
			std::string sid;
			if (SUCCEEDED(cs[a].dev->GetId(&id)) && id != NULL) { sid = W2U(id); CoTaskMemFree(id); }
			bool dup = false;
			for (size_t k = 0; k < seen.size(); ++k) if (seen[k] == sid) dup = true;
			if (dup) { cs[a].dev->Release(); continue; }
			seen.push_back(sid);
			out->push_back(cs[a].dev);
		}
		pc->Release();
	}
}

/* 选择单个设备(兼容旧接口): dev<=0 用默认, 否则取列表第 dev 个(含禁用/未插入) */
static HRESULT PickDevice(IMMDeviceEnumerator* pEnum, EDataFlow flow, int dev, IMMDevice** ppOut)
{
	if (ppOut == NULL) return E_POINTER;
	*ppOut = NULL;
	std::vector<IMMDevice*> cands;
	CollectCandidates(pEnum, flow, dev, &cands);
	if (cands.empty()) return E_NOTFOUND;
	*ppOut = cands[0];                     /* 交给调用方 */
	for (size_t i = 1; i < cands.size(); ++i) cands[i]->Release();
	return S_OK;
}

/* ---------- 组装并发送一帧 PCM ---------- */
static bool SendFramePack(AudioSrc* S, int seq, int rate, int ch, int bytes, const char* pcm, int flags)
{
	SOCKET s = g_client_socket;
	if (s == INVALID_SOCKET) return false;
	int total = (int)sizeof(AudioFrameHead) + bytes;
	std::vector<char> body((size_t)total);
	AudioFrameHead h;
	memset(&h, 0, sizeof(h));
	h.seq = seq; h.rate = rate; h.ch = ch; h.bits = 16; h.bytes = bytes; h.flags = flags;
	memcpy(body.data(), &h, sizeof(h));
	if (bytes > 0 && pcm != NULL)
		memcpy(body.data() + sizeof(h), pcm, (size_t)bytes);
	if (!SendCmd(s, S->cmdFrame, body.data(), total))
	{
		Log("%s: send frame failed, stop capture", S->name);
		InterlockedExchange(&S->want, 0);
		return false;
	}
	InterlockedIncrement(&S->seq);
	InterlockedExchangeAdd(&S->bytes, (LONG)bytes);
	return true;
}

/* ---------- 采集线程(系统声音 / 麦克风 共用) ---------- */
/* ---------- 采集线程(系统声音 / 麦克风 共用) ---------- */
static DWORD WINAPI AudioThread(LPVOID param)
{
	AudioSrc* S = (AudioSrc*)param;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* pEnum = NULL;
	IAudioClient*        pCli  = NULL;
	IAudioCaptureClient* pCap  = NULL;
	WAVEFORMATEX*        pwfx  = NULL;
	short*               conv  = NULL;
	size_t               convCap = 0;
	std::vector<char>    pending;
	std::string          devDesc = "默认";
	bool                 ok = false;
	int                  rate = 0, ch = 0;
	int                  candCount = 0;
	SampleKind           kind = SK_UNKNOWN;
	HRESULT hr = S_OK;

	InterlockedExchange(&S->running, 1);

	/* v19: 最多三轮。每轮先确保音频服务在跑; 一轮全失败就启用"已禁用"的音频设备再试。 */
	for (int attempt = 0; attempt < 3 && !ok; ++attempt)
	{
		if (attempt > 0) { Log("%s: retry #%d", S->name, attempt); Sleep(1200); }

		EnsureAudioServices();

		if (pEnum == NULL)
		{
			hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), (void**)&pEnum);
			if (FAILED(hr) || pEnum == NULL)
			{
				Log("%s: CoCreateInstance failed 0x%08x", S->name, (unsigned)hr);
				break;
			}
		}

		std::vector<IMMDevice*> cands;
		CollectCandidates(pEnum, S->flow, S->dev, &cands);
		candCount = (int)cands.size();
		Log("%s: %d candidate endpoint(s), dev=%d", S->name, candCount, S->dev);

		for (size_t ci = 0; ci < cands.size() && !ok; ++ci)
		{
			IMMDevice* pDev = cands[ci];
			std::string nm = DeviceLabel(pDev);
			do
			{
				hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pCli);
				if (FAILED(hr) || pCli == NULL)
				{ Log("%s: [%d] Activate failed 0x%08x", S->name, (int)ci, (unsigned)hr); break; }

				hr = pCli->GetMixFormat(&pwfx);
				if (FAILED(hr) || pwfx == NULL)
				{ Log("%s: [%d] GetMixFormat failed 0x%08x", S->name, (int)ci, (unsigned)hr); break; }

				kind = DetectKind(pwfx);
				rate = (int)pwfx->nSamplesPerSec;
				ch   = (int)pwfx->nChannels;
				if (kind == SK_UNKNOWN || rate <= 0 || ch <= 0)
				{ Log("%s: [%d] unsupported format tag=%d bits=%d", S->name, (int)ci,
					(int)pwfx->wFormatTag, (int)pwfx->wBitsPerSample); break; }

				/* 系统声音: 播放设备 + LOOPBACK(不需要用户开"立体声混音", 也不需要插喇叭);
				   麦克风: 录音设备 + 普通采集 */
				hr = pCli->Initialize(AUDCLNT_SHAREMODE_SHARED, S->sflags, 500000, 0, pwfx, NULL);
				if (FAILED(hr))
				{ Log("%s: [%d] Initialize(loopback=%d) failed 0x%08x, try next endpoint",
					S->name, (int)ci, (S->sflags != 0) ? 1 : 0, (unsigned)hr); break; }

				hr = pCli->GetService(__uuidof(IAudioCaptureClient), (void**)&pCap);
				if (FAILED(hr) || pCap == NULL)
				{ Log("%s: [%d] GetService(IAudioCaptureClient) failed 0x%08x", S->name, (int)ci, (unsigned)hr); break; }

				hr = pCli->Start();
				if (FAILED(hr))
				{ Log("%s: [%d] Start failed 0x%08x", S->name, (int)ci, (unsigned)hr); break; }

				devDesc = nm.empty() ? "未知设备" : nm;
				ok = true;
			} while (0);

			if (!ok)
			{
				if (pCap) { pCap->Release(); pCap = NULL; }
				if (pCli) { pCli->Release(); pCli = NULL; }
				if (pwfx) { CoTaskMemFree(pwfx); pwfx = NULL; }
			}
		}

		for (size_t ci = 0; ci < cands.size(); ++ci) cands[ci]->Release();

		if (!ok && attempt == 0)
		{
			int en = EnableDisabledAudioEndpoints();
			Log("%s: first round failed; re-enabled %d disabled audio device(s)", S->name, en);
		}
	}

	if (ok)
	{
		sprintf_s(S->info, sizeof(S->info), "%s %dHz %dch %dbit dev=%s gain=%d%%",
			S->label, rate, ch, (int)(pwfx ? pwfx->wBitsPerSample : 16), devDesc.c_str(), S->gain);
		Log("%s: %s", S->name, S->info);

		int seq = 0;
		int need = rate * ch * 2 * (S->bufms > 0 ? S->bufms : 40) / 1000;
		if (need < 1024) need = 1024;
		bool first = true;
		DWORD lastData = GetTickCount();

		while (InterlockedCompareExchange(&S->want, 1, 1) != 0)
		{
			Sleep(5);
			UINT32 pkt = 0;
			if (FAILED(pCap->GetNextPacketSize(&pkt))) break;

			while (pkt != 0)
			{
				BYTE* data = NULL;
				UINT32 frames = 0;
				DWORD flags = 0;
				hr = pCap->GetBuffer(&data, &frames, &flags, NULL, NULL);
				if (hr != S_OK) break;

				if (frames > 0)
				{
					size_t n = (size_t)frames * ch;
					if (n > convCap)
					{
						free(conv);
						convCap = n * 2;
						conv = (short*)malloc(convCap * sizeof(short));
					}
					int got = 0;
					if (conv == NULL)
					{
						pCap->ReleaseBuffer(frames);
						break;
					}
					if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
					{
						memset(conv, 0, n * sizeof(short));
						got = (int)n;
					}
					else
					{
						got = ConvertToS16(data, frames, ch, kind, conv);
						ApplyGain(conv, got, S->gain);
					}
					const char* p = (const char*)conv;
					pending.insert(pending.end(), p, p + (size_t)got * 2);
					lastData = GetTickCount();
				}
				pCap->ReleaseBuffer(frames);
				if (FAILED(pCap->GetNextPacketSize(&pkt))) break;
			}

			/* v19: 当前系统没有任何音频流在播放时, LOOPBACK 不会送包。
			   这里主动补静音, 保持控制端播放缓冲连续(避免"没声音时看起来卡住")。 */
			if (pending.empty() && (GetTickCount() - lastData) >= 400)
			{
				pending.insert(pending.end(), (size_t)need, (char)0);
				lastData = GetTickCount();
			}

			while ((int)pending.size() >= need)
			{
				if (!SendFramePack(S, seq++, rate, ch, need, pending.data(), first ? RC_AF_FIRST : 0))
					break;
				first = false;
				pending.erase(pending.begin(), pending.begin() + need);
			}
			if (g_client_socket == INVALID_SOCKET) break;
		}

		pCli->Stop();
	}
	else
	{
		char b[360];
		sprintf_s(b, sizeof(b),
			"fail:%s 没有可用的音频设备(已尝试 %d 个候选, 含被禁用/未插入)。"
			"若被控端确实无声卡, 请安装虚拟声卡(如 VB-Cable / ToDesk虚拟声卡)后重试。",
			S->label, candCount);
		sprintf_s(S->info, sizeof(S->info), "%s", b);
		Log("%s: %s", S->name, b);
		SendText(g_client_socket, S->cmdRes, std::string(b));
	}

	if (pCap) pCap->Release();
	if (pCli) pCli->Release();
	if (pwfx) CoTaskMemFree(pwfx);
	if (pEnum) pEnum->Release();
	free(conv);
	CoUninitialize();
	InterlockedExchange(&S->want, 0);
	InterlockedExchange(&S->running, 0);
	Log("%s: capture thread exit (ok=%d)", S->name, ok ? 1 : 0);
	return 0;
}

/* ---------- 启停 / 命令处理 ---------- */
static void SrcStop(AudioSrc* S)
{
	InterlockedExchange(&S->want, 0);
	if (S->thread != NULL)
	{
		WaitForSingleObject(S->thread, 3000);
		CloseHandle(S->thread);
		S->thread = NULL;
	}
	InterlockedExchange(&S->running, 0);
}

static void SrcStart(AudioSrc* S)
{
	DWORD tid = 0;
	InterlockedExchange(&S->want, 1);
	InterlockedExchange(&S->seq, 0);
	InterlockedExchange(&S->bytes, 0);
	InterlockedExchange(&S->running, 1);
	S->thread = CreateThread(NULL, 0, AudioThread, S, 0, &tid);
	if (S->thread == NULL)
	{
		InterlockedExchange(&S->want, 0);
		InterlockedExchange(&S->running, 0);
		Log("%s: CreateThread failed %d", S->name, GetLastError());
	}
}

void HandleAudioStop() { SrcStop(&g_sys); }
void HandleMicStop()   { SrcStop(&g_mic); }

static void SrcHandle(AudioSrc* S, Packet* p)
{
	AudioReq req;
	memset(&req, 0, sizeof(req));
	int n = p->header.body_len < (int)sizeof(req) ? p->header.body_len : (int)sizeof(req);
	if (n > 0) memcpy(&req, p->body, n);
	char b[512];

	if (req.op == RC_AUD_START)
	{
		SrcStop(S);
		if (req.buf_ms >= 10 && req.buf_ms <= 500) S->bufms = req.buf_ms;
		if (req.gain >= 10 && req.gain <= 800)     S->gain  = req.gain;
		if (req.dev >= 0)                          S->dev   = req.dev;
		SrcStart(S);
		sprintf_s(b, sizeof(b), "ok:%s starting (%s packet=%dms dev=%d gain=%d%%)",
			S->name, S->label, S->bufms, S->dev, S->gain);
		SendText(g_client_socket, S->cmdRes, b);
	}
	else if (req.op == RC_AUD_STOP)
	{
		SrcStop(S);
		sprintf_s(b, sizeof(b), "ok:%s stopped", S->name);
		SendText(g_client_socket, S->cmdRes, b);
	}
	else if (req.op == RC_AUD_LIST)
	{
		std::string list = ListEndpoints(S->flow);
		sprintf_s(b, sizeof(b), "ok:%s %s 设备列表:\n", S->name, S->label);
		SendText(g_client_socket, S->cmdRes, std::string(b) + list);
	}
	else
	{
		sprintf_s(b, sizeof(b), "ok:%s running=%d frames=%d bytes=%d dev=%d gain=%d%% %s",
			S->name, (int)InterlockedCompareExchange(&S->want, 1, 1) ? 1 : 0,
			(int)InterlockedCompareExchange(&S->seq, 0, 0),
			(int)InterlockedCompareExchange(&S->bytes, 0, 0),
			S->dev, S->gain, S->info);
		SendText(g_client_socket, S->cmdRes, b);
	}
}

void HandleAudioPacket(Packet* p) { SrcHandle(&g_sys, p); }
void HandleMicPacket(Packet* p)   { SrcHandle(&g_mic, p); }
