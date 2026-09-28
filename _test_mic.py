# -*- coding: utf-8 -*-
# 麦克风/系统声音 双源回传端到端实测
import os, sys, socket, struct, subprocess, time, math, array, wave, tempfile

ROOT = r"F:\cpp interview\RemoteControl-master"
EXE  = os.path.join(ROOT, "RemoteControl", "x64", "Release", "RemoteControl.exe")
PORT = 19997
MAGIC = 0x12345678
CMD_AUDIO, CMD_AUDIO_FRAME, CMD_AUDIO_RES = 70, 71, 72
CMD_MIC,   CMD_MIC_FRAME,   CMD_MIC_RES   = 76, 77, 78
HDR = struct.Struct("<iii")
AREQ = struct.Struct("<iiiiiii")     # op, buf_ms, rate, ch, bits, dev, gain
(A_OP, A_BUF, A_RATE, A_CH, A_BITS, A_DEV, A_GAIN) = range(7)
RC_AUD_STOP, RC_AUD_START, RC_AUD_QUERY, RC_AUD_LIST = 0, 1, 2, 3
AFH = struct.Struct("<iiiiii")       # seq, rate, ch, bits, bytes, flags


def pack(cmd, body=b""):
    return HDR.pack(MAGIC, cmd, len(body)) + body


def req(op, buf_ms=40, dev=0, gain=100, rate=0, ch=0, bits=0):
    return AREQ.pack(op, buf_ms, rate, ch, bits, dev, gain)


class Conn:
    def __init__(self, s):
        self.s = s
        self.buf = b""
        self.txt = []
        self.frames = {"mic": [], "audio": []}   # (rate, ch, bits, pcm)
        self.hdrs = {"mic": None, "audio": None}

    def pump(self, seconds):
        t0 = time.time()
        got = 0
        while time.time() - t0 < seconds:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            self.buf += d
            got += self.drain()
        return got

    def drain(self):
        n = 0
        while len(self.buf) >= 12:
            mg, cmd, ln = HDR.unpack_from(self.buf, 0)
            if mg != MAGIC:
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 12 + ln:
                break
            body = self.buf[12:12 + ln]
            self.buf = self.buf[12 + ln:]
            n += 1
            if cmd in (CMD_MIC_FRAME, CMD_AUDIO_FRAME):
                key = "mic" if cmd == CMD_MIC_FRAME else "audio"
                seq, r, c, b, nb, fl = AFH.unpack_from(body, 0)
                self.hdrs[key] = (r, c, b)
                self.frames[key].append(body[AFH.size:AFH.size + nb])
            elif cmd in (CMD_MIC_RES, CMD_AUDIO_RES):
                self.txt.append(("mic" if cmd == CMD_MIC_RES else "audio",
                                 body.decode("utf-8", "replace").strip()))
        return n


def rms_of(chunks):
    pcm = b"".join(chunks)
    if len(pcm) < 4:
        return 0.0, 0, 0
    pcm = pcm[:len(pcm) // 2 * 2]
    a = array.array("h")
    a.frombytes(pcm)
    if not a:
        return 0.0, 0, 0
    m = sum(float(x) * x for x in a) / len(a)
    return math.sqrt(m), max(abs(x) for x in a), len(a)


wd = tempfile.mkdtemp(prefix="rcmic_")
logf = open(os.path.join(wd, "srv.log"), "wb")
DETACHED = 0x00000008 | 0x08000000
proc = subprocess.Popen([EXE, "--port", str(PORT)], cwd=wd, stdout=logf, stderr=subprocess.STDOUT,
                        creationflags=DETACHED)
print("[srv] pid", proc.pid, "workdir", wd)
time.sleep(2.5)

s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
s.settimeout(2)
c = Conn(s)
res = {}

# ---- 1) 麦克风设备列表 ----
print("\n[1] mic list")
s.sendall(pack(CMD_MIC, req(RC_AUD_LIST)))
c.pump(2.0)
for who, t in c.txt:
    print("    res(%s):" % who, t.replace("\n", " | ")[:600])
res["list"] = [t for w, t in c.txt if w == "mic"]

# ---- 2) 麦克风采集: 静音段 vs 放音段 ----
print("\n[2] mic start (gain=100), 静音2s")
c.txt.clear()
s.sendall(pack(CMD_MIC, req(RC_AUD_START, 40, 0, 100)))
c.pump(2.0)
for who, t in c.txt:
    print("    res(%s):" % who, t)
sil = list(c.frames["mic"])
print("    静音段: frames=%d fmt=%s" % (len(sil), c.hdrs["mic"]))

print("    播放 880Hz 音 2s (扬声器外放, 期望被麦拾取)")
rate, dur, amp = 44100, 2.0, 20000
a = array.array("h", (int(amp * math.sin(2 * math.pi * 880 * i / rate)) for i in range(int(rate * dur))))
wavp = os.path.join(wd, "tone.wav")
wf = wave.open(wavp, "wb"); wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(rate)
wf.writeframes(a.tobytes()); wf.close()
import winsound
c.frames["mic"] = []
winsound.PlaySound(wavp, winsound.SND_FILENAME | winsound.SND_ASYNC)
c.pump(2.5)
loud = list(c.frames["mic"])
print("    放音段: frames=%d" % len(loud))

rs, ps, ns = rms_of(sil)
rl, pl, nl = rms_of(loud)
print("    静音 RMS=%.1f peak=%d n=%d" % (rs, ps, ns))
print("    放音 RMS=%.1f peak=%d n=%d" % (rl, pl, nl))
res["mic_rms_silent"] = rs
res["mic_rms_loud"] = rl
res["mic_fmt"] = c.hdrs["mic"]
if loud:
    out = os.path.join(wd, "mic_captured.wav")
    r, ch, b = c.hdrs["mic"]
    wf = wave.open(out, "wb"); wf.setnchannels(ch); wf.setsampwidth(b // 8); wf.setframerate(r)
    wf.writeframes(b"".join(loud)); wf.close()
    print("    wav:", out)

# ---- 3) 增益生效验证 (gain=800) ----
print("\n[3] mic gain=800 复采")
s.sendall(pack(CMD_MIC, req(RC_AUD_STOP)))
c.pump(0.6)
c.frames["mic"] = []
s.sendall(pack(CMD_MIC, req(RC_AUD_START, 40, 0, 800)))
c.pump(0.8)
winsound.PlaySound(wavp, winsound.SND_FILENAME | winsound.SND_ASYNC)
c.frames["mic"] = []
c.pump(2.5)
g8 = list(c.frames["mic"])
rg, pg, ng = rms_of(g8)
print("    增益800 RMS=%.1f peak=%d frames=%d" % (rg, pg, len(g8)))
res["mic_rms_gain800"] = rg

# ---- 4) 系统声音 + 麦克风 同时回传 ----
print("\n[4] audio start + mic 同时运行")
c.txt.clear()
c.frames["audio"] = []
c.frames["mic"] = []
s.sendall(pack(CMD_AUDIO, req(RC_AUD_START, 40, 0, 100)))
time.sleep(0.4)
winsound.PlaySound(wavp, winsound.SND_FILENAME | winsound.SND_ASYNC)
c.pump(3.0)
ra, pa, na = rms_of(c.frames["audio"])
rm, pm, nm = rms_of(c.frames["mic"])
print("    系统声音: frames=%d fmt=%s RMS=%.1f" % (len(c.frames["audio"]), c.hdrs["audio"], ra))
print("    麦克风  : frames=%d fmt=%s RMS=%.1f" % (len(c.frames["mic"]), c.hdrs["mic"], rm))
res["both"] = (len(c.frames["audio"]), len(c.frames["mic"]), ra, rm)
for who, t in c.txt:
    print("    res(%s):" % who, t)

# ---- 5) 停止 ----
print("\n[5] stop both")
s.sendall(pack(CMD_MIC, req(RC_AUD_STOP)))
s.sendall(pack(CMD_AUDIO, req(RC_AUD_STOP)))
c.pump(1.0)
c.txt.clear()
s.sendall(pack(CMD_MIC, req(RC_AUD_QUERY)))
s.sendall(pack(CMD_AUDIO, req(RC_AUD_QUERY)))
c.pump(1.5)
for who, t in c.txt:
    print("    res(%s):" % who, t)

print("\n=== 判定 ===")
print("设备列表非空         :", len(res["list"]) > 0)
print("麦克风有数据帧       :", len(sil) + len(loud) > 0)
print("现场声音被拾取(RMS升):", rl > rs * 1.5 + 1)
print("增益生效(RMS上升)    :", rg > rl * 1.5 + 1)
print("双源同时回传         :", res["both"][0] > 0 and res["both"][1] > 0)
args = [str(x) for x in [res["mic_rms_silent"], res["mic_rms_loud"], res["mic_rms_gain800"]]]
open(os.path.join(ROOT, "_mic_test_result.txt"), "w", encoding="utf-8").write(
    "\n".join(args + [repr(res["mic_fmt"]), repr(res["both"])]))
print("workdir:", wd)
try:
    s.close()
except Exception:
    pass
