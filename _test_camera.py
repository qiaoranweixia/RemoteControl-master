import os, socket, struct, subprocess, time, tempfile

ROOT = r"F:\cpp interview\RemoteControl-master"
EXE  = os.path.join(ROOT, "RemoteControl", "x64", "Release", "RemoteControl.exe")
PORT = 19998
MAGIC = 0x12345678
CMD_HELLO, CMD_CAMERA, CMD_CAMERA_FRAME, CMD_CAMERA_RES = 201, 73, 74, 75
HDR = struct.Struct("<iii")
REQ = struct.Struct("<iiiiii")
FH  = struct.Struct("<iiiiii")

def pack(cmd, body=b""):
    return HDR.pack(MAGIC, cmd, len(body)) + body

wd = tempfile.mkdtemp(prefix="camtest_")
logf = open(os.path.join(wd, "srv.log"), "wb")
proc = subprocess.Popen([EXE, "--port", str(PORT)], cwd=wd, stdout=logf, stderr=subprocess.STDOUT)
print("server pid", proc.pid)
time.sleep(2.5)

s = socket.create_connection(("127.0.0.1", PORT), timeout=8)
s.settimeout(3)
# 读 hello
buf = b""
try:
    d = s.recv(65536)
    buf += d
    print("hello bytes", len(d))
except Exception as e:
    print("hello recv err", e)
if len(buf) >= 12:
    mg, cmd, ln = HDR.unpack(buf[:12])
    print("   first msg cmd=%d len=%d magic_ok=%s" % (cmd, ln, mg == MAGIC))

def query(op, idx=0, w=0, h=0, fps=10, q=70, wait=3.0, save=None):
    s.sendall(pack(CMD_CAMERA, REQ.pack(op, idx, w, h, fps, q)))
    t0 = time.time()
    frames = 0
    total = 0
    sizes = []
    info = []
    dims = set()
    while time.time() - t0 < wait:
        try:
            head = b""
            while len(head) < 12:
                c = s.recv(12 - len(head))
                if not c:
                    raise EOFError("closed")
                head += c
            mg, cmd, ln = HDR.unpack(head)
            if mg != MAGIC:
                print("   BAD MAGIC", hex(mg)); break
            body = b""
            while len(body) < ln:
                c = s.recv(min(65536, ln - len(body)))
                if not c:
                    raise EOFError("closed")
                body += c
            if cmd == CMD_CAMERA_FRAME:
                seq, index, fw, fh, fmt, nbytes = FH.unpack(body[:24])
                jpg = body[24:]
                frames += 1
                total += len(jpg)
                sizes.append(len(jpg))
                dims.add((fw, fh, fmt))
                if frames == 1:
                    print("   first frame: seq=%d %dx%d fmt=%d bytes=%d (head.bytes=%d, actual=%d) jpeg_magic=%s"
                          % (seq, fw, fh, fmt, len(jpg), nbytes, len(jpg), jpg[:2].hex()))
                    if save:
                        open(save, "wb").write(jpg)
            else:
                txt = body.decode("utf-8", "replace")
                info.append((cmd, txt))
                print("   [cmd=%d] %s" % (cmd, txt.replace("\r\n", " | ").strip()[:220]))
        except socket.timeout:
            continue
        except EOFError as e:
            print("   conn closed:", e); break
    return frames, total, sizes, dims, info

print("--- 1) 查询摄像头 ---")
f, t, sz, dm, inf = query(2, wait=2.5)
print("--- 2) 启动采集 5 秒 ---")
f, t, sz, dm, inf = query(1, idx=0, fps=10, q=70, wait=5.0, save=os.path.join(wd, "cam_first.jpg"))
print("frames=%d total=%d avg=%s dims=%s" % (f, t, (round(t / f) if f else 0), dm))
if sz:
    print("   size min/max", min(sz), max(sz))
print("--- 3) 停止 ---")
s.sendall(pack(CMD_CAMERA, REQ.pack(0, 0, 0, 0, 0, 0)))
time.sleep(1.0)
try:
    d = s.recv(65536)
    if len(d) >= 12:
        mg, cmd, ln = HDR.unpack(d[:12])
        print("   stop res cmd=%d %s" % (cmd, d[12:12+ln].decode("utf-8", "replace").strip()[:200]))
except Exception as e:
    print("   stop recv:", e)
s.close()
time.sleep(0.5)
proc.terminate()
time.sleep(0.5)
logf.close()
print("--- 服务端 stdout ---")
print(open(os.path.join(wd, "srv.log"), "rb").read().decode("gbk", "replace")[-1200:])
print("workdir", wd)
