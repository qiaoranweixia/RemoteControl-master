# -*- coding: utf-8 -*-
# 麦克风: 设备列表(COM修复验证) + 增益 A/B(交替100/800各2轮) + dev 选择
import os, sys, socket, struct, subprocess, time, math, array, wave, tempfile, winsound

ROOT = r"F:\cpp interview\RemoteControl-master"
EXE  = os.path.join(ROOT, "RemoteControl", "x64", "Release", "RemoteControl.exe")
PORT = 19996
MAGIC = 0x12345678
CMD_MIC, CMD_MIC_FRAME, CMD_MIC_RES = 76, 77, 78
HDR = struct.Struct("<iii")
AREQ = struct.Struct("<iiiiiii")     # op, buf_ms, rate, ch, bits, dev, gain
RC_AUD_STOP, RC_AUD_START, RC_AUD_QUERY, RC_AUD_LIST = 0, 1, 2, 3
AFH = struct.Struct("<iiiiii")       # seq, rate, ch, bits, bytes, flags


def pack(cmd, body=b""):
    return HDR.pack(MAGIC, cmd, len(body)) + body


def req(op, buf_ms=40, dev=0, gain=100, rate=0, ch=0, bits=0):
    return AREQ.pack(op, buf_ms, rate, ch, bits, dev, gain)


class Conn:
    def __init__(self, s):
        self.s = s; self.buf = b""; self.txt = []; self.frames = []; self.hdr = None

    def pump(self, seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            self.buf += d
            self.drain()

    def drain(self):
        while len(self.buf) >= 12:
            mg, cmd, ln = HDR.unpack_from(self.buf, 0)
            if mg != MAGIC:
                self.buf = self.buf[1:]; continue
            if len(self.buf) < 12 + ln:
                break
            body = self.buf[12:12 + ln]; self.buf = self.buf[12 + ln:]
            if cmd == CMD_MIC_FRAME:
                seq, r, c, b, nb, fl = AFH.unpack_from(body, 0)
                self.hdr = (r, c, b)
                self.frames.append(body[AFH.size:AFH.size + nb])
            elif cmd == CMD_MIC_RES:
                self.txt.append(body.decode("utf-8", "replace").strip())


def rms_of(chunks):
    pcm = b"".join(chunks)
    if len(pcm) < 4:
        return 0.0, 0
    pcm = pcm[:len(pcm) // 2 * 2]
    a = array.array("h"); a.frombytes(pcm)
    if not a:
        return 0.0, 0
    m = sum(float(x) * x for x in a) / len(a)
    return math.sqrt(m), max(abs(x) for x in a)


wd = tempfile.mkdtemp(prefix="rcgain_")
logf = open(os.path.join(wd, "srv.log"), "wb")
DETACHED = 0x00000008 | 0x08000000
proc = subprocess.Popen([EXE, "--port", str(PORT)], cwd=wd, stdout=logf,
                        stderr=subprocess.STDOUT, creationflags=DETACHED)
print("[srv] pid", proc.pid, "wd", wd)
time.sleep(2.5)
s = socket.create_connection(("127.0.0.1", PORT), timeout=5); s.settimeout(2)
c = Conn(s)
out = []

# 1) 设备列表
print("\n[1] list")
s.sendall(pack(CMD_MIC, req(RC_AUD_LIST)))
c.pump(2.0)
for t in c.txt:
    print("    ", t.replace("\n", " | ")[:700])
out.append("LIST_OK=%s" % (len(c.txt) > 0 and "error" not in c.txt[0].lower()))

# 提示音(扬声器外放,由麦克风拾取)
rate, dur, amp = 44100, 1.6, 22000
a = array.array("h", (int(amp * math.sin(2 * math.pi * 660 * i / rate)) for i in range(int(rate * dur))))
wavp = os.path.join(wd, "tone.wav")
wf = wave.open(wavp, "wb"); wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(rate)
wf.writeframes(a.tobytes()); wf.close()

# 2) 增益 A/B: 100 / 800 交替 2 轮
print("\n[2] gain A/B")
s.sendall(pack(CMD_MIC, req(RC_AUD_LIST)))   # 预热
c.pump(0.8)
gain_res = {100: [], 800: []}
for rnd in range(2):
    for g in (100, 800):
        s.sendall(pack(CMD_MIC, req(RC_AUD_STOP))); c.pump(0.4); c.txt.clear()
        s.sendall(pack(CMD_MIC, req(RC_AUD_START, 40, 0, g))); c.pump(0.7)
        for t in c.txt:
            if "gain" in t or "error" in t.lower():
                print("    srv:", t.replace("\n", " | ")[:200])
        c.txt.clear(); c.frames = []
        winsound.PlaySound(wavp, winsound.SND_FILENAME | winsound.SND_ASYNC)
        c.pump(1.8)
        r, p = rms_of(c.frames)
        gain_res[g].append((r, p, len(c.frames)))
        print("    rnd%d gain=%d  frames=%d  RMS=%.1f peak=%d" % (rnd + 1, g, len(c.frames), r, p))
        time.sleep(0.3)

med = {g: sorted(v[0] for v in gain_res[g])[len(gain_res[g]) // 2] for g in (100, 800)}
print("    中位 RMS: 100%%=%.1f  800%%=%.1f  比值=%.2f" %
      (med[100], med[800], med[800] / med[100] if med[100] > 0.5 else -1))
out.append("RMS100=%.1f RMS800=%.1f ratio=%.2f" %
           (med[100], med[800], med[800] / med[100] if med[100] > 0.5 else -1))

# 3) dev 选择
print("\n[3] dev=1 选择")
s.sendall(pack(CMD_MIC, req(RC_AUD_STOP))); c.pump(0.4); c.txt.clear()
s.sendall(pack(CMD_MIC, req(RC_AUD_START, 40, 1, 100))); c.pump(1.2)
for t in c.txt:
    print("    ", t.replace("\n", " | ")[:250])
out.append("DEV1=" + (c.txt[0].replace("\n", " | ")[:150] if c.txt else "(no reply)"))
s.sendall(pack(CMD_MIC, req(RC_AUD_STOP))); c.pump(0.5)

print("\n=== 结果 ===")
for l in out:
    print(" ", l)
open(os.path.join(ROOT, "_mic_gain_result.txt"), "w", encoding="utf-8").write("\n".join(out))
try:
    s.close()
except Exception:
    pass
proc.kill()
print("wd:", wd)
