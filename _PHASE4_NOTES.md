# RemoteControl Phase4 开发笔记 (2026-09-19)
## 已完成并实测
- 系统声音回传: 服务端 WASAPI loopback -> CMD_AUDIO_FRAME -> 客户端 waveOut 播放。实测 48kHz/2ch, RMS=1935 非静音。
- 摄像头采集(服务端): RemoteControl/camera_capture.cpp
  * MF SourceReader + MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING => 请求 RGB32, 失败则 MJPG/NV12/YUY2
  * GDI+ JPEG 编码(RGB32/BGRA) -> CMD_CAMERA_FRAME (74); 状态文本走 CMD_CAMERA_RES(75)
  * 实测: 查询设备 OK("1 摄像头: [0] Integrated Camera"), 采集 1280x720 ~8fps, JPEG 可解码
  * 坑1: 直接用 RGB32 假定 stride=w*4 会越界崩(该摄像头 native NV12 stride=5120) => 必须读 MF_MT_DEFAULT_STRIDE + 校验 curLen>=absStride*h
  * 坑2: MJPG 缓冲固定 1MB 且带 0 填充 => 用 pSample->GetTotalLength(DWORD*) 截断 + 回找 FFD9
  * 坑3: 该摄像头 MJPG 分支可直接透传(FFD8 开头)
- 构建: call "D:\\vs2022\\VC\\Auxiliary\\Build\\vcvarsall.bat" x64 && cd /d <proj> && MSBuild <x.vcxproj> /p:Configuration=Release /p:Platform=x64 /v:m /m
- 服务端 API: SendCmd(SOCKET,int,const char*,int) / SendText(SOCKET,int,const std::string&) / Log(fmt,...); 命令号 CMD_CAMERA=73, CMD_CAMERA_FRAME=74, CMD_CAMERA_RES=75; struct CameraReq{op,index,w,h,fps,quality}, CameraFrameHead{seq,index,w,h,format,bytes}
- 测试脚本: <proj>/_test_audio.py, <proj>/_test_camera.py (起服务端 --port N, 存 srv.log / cam_first.jpg)
## 待办
- 客户端摄像头显示窗口 + 命令 camera list/start/stop/shot
- 麦克风采集(被控端) - 目前 camera_capture.cpp 尾部仅有桩
- 控制端本地录制 lrec/lplay (与被控端 rec 分开)
