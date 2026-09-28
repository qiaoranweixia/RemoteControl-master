import os, sys, socket, struct, subprocess, time, math, threading, tempfile
sys.path.insert(0, r"F:\cpp interview\GenericAgent-main\temp")

ROOT = r"F:\cpp interview\RemoteControl-master"
EXE  = os.path.join(ROOT, "RemoteControl", "x64", "Release", "RemoteControl.exe")
PORT = 19999
MAGIC = 0x12345678
CMD_AUDIO, CMD_AUDIO_FRAME, CMD_AUDIO_RES = 70, 71, 72
HDR = struct.Struct("<iii")

def pack(cmd, body=b""):
    return HDR.pack(MAGIC, cmd, len(body)) + body

wd = tempfile.mkdtemp(prefix="rcaud_")
logf = open(os.path.join(wd, "srv.log"), "wb")
DETACHED = 0x00000008 | 0x08000000
proc = subprocess.Popen([EXE, "--port", str(PORT)], cwd=wd, stdout=logf, stderr=subprocess.STDOUT,
                        creationflags=DETACHED)
print("server pid", proc.pid)
time.sleep(2.0)

s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
s.settimeout(3)
req = struct.pack("<iiiii", 1, 40, 0, 0, 0)      # RC_AUD_START, 40ms/包
s.sendall(pack(CMD_AUDIO, req))
print("sent CMD_AUDIO START")

# 本机播放 2 秒 440Hz 低音量音(走默认播放设备 -> 被 WASAPI 环回采集)
import winsound, wave, array
wavp = os.path.join(wd, "tone.wav")
rate, dur, amp = 44100, 2.0, 3000
a = array.array("h", (int(amp * math.sin(2 * math.pi * 440 * i / rate)) for i in range(int(rate * dur))))
wf = wave.open(wavp, "wb"); wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(rate)
wf.writeframes(a.tobytes()); wf.close()
winsound.PlaySound(wavp, winsound.SND_FILENAME | winsound.SND_ASYNC)

buf = b""
frames, pcm, hdr_info, res_txt = 0, bytearray(), None, []
t0 = time.time()
while time.time() - t0 < 4.0:
    try:
        d = s.recv(65536)
    except socket.timeout:
        continue
    if not d:
        print("server closed"); break
    buf += d
    while len(buf) >= 12:
        mg, cmd, ln = HDR.unpack_from(buf, 0)
        if mg != MAGIC:
            buf = buf[1:]; continue
        if len(buf) < 12 + ln: break
        body = buf[12:12 + ln]; buf = buf[12 + ln:]
        if cmd == CMD_AUDIO_FRAME:
            seq, r, ch, bits, nb, flags = struct.unpack_from("<iiiiii", body, 0)
            hdr_info = (r, ch, bits)
            frames += 1
            pcm += body[24:24 + nb]
        elif cmd == CMD_AUDIO_RES:
            res_txt.append(body.decode("utf-8", "replace").strip())

print("frames=%d pcm_bytes=%d fmt=%s" % (frames, len(pcm), hdr_info))
if pcm:
    sam = array.array("h"); sam.frombytes(bytes(pcm[:len(pcm) // 2 * 2]))
    rms = math.sqrt(sum(float(x) * x for x in sam) / max(1, len(sam)))
    print("samples=%d rms=%.1f peak=%d" % (len(sam), rms, max(abs(x) for x in sam)))
    r, ch, bits = hdr_info
    out = os.path.join(wd, "captured.wav")
    wf = wave.open(out, "wb"); wf.setnchannels(ch); wf.setsampwidth(bits // 8); wf.setframerate(r)
    wf.writeframes(bytes(pcm)); wf.close()
    print("wav:", out)
print("res_text:", res_txt)

s.sendall(pack(CMD_AUDIO, struct.pack("<iiiii", 0, 0, 0, 0, 0)))
time.sleep(0.8)
try:
    s.settimeout(1.5); d = s.recv(65536)
    print("after stop tail bytes", len(d))
except Exception as e:
    print("after stop recv:", e)
s.close()
time.sleep(0.5)
proc.terminate()
print("--- server log ---")
logf.close()
print(open(os.path.join(wd, "srv.log"), encoding="gbk", errors="replace").read()[-800:])
