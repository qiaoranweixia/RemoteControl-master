import os, socket, struct, subprocess, time, tempfile, glob

ROOT = r"F:\cpp interview\RemoteControl-master"
EXE  = os.path.join(ROOT, "RemoteControl", "x64", "Release", "RemoteControl.exe")
PORT = 19997
MAGIC = 0x12345678
CMD_HELLO, CMD_CAMERA, CMD_CAMERA_FRAME, CMD_CAMERA_RES = 201, 73, 74, 75
HDR = struct.Struct("<iii")
REQ = struct.Struct("<iiiiii")
FH  = struct.Struct("<iiiiii")

def pack(cmd, body=b""):
    return HDR.pack(MAGIC, cmd, len(body)) + body

wd = tempfile.mkdtemp(prefix="camrepro_")
logp = os.path.join(wd, "srv.log")
logf = open(logp, "wb")
proc = subprocess.Popen([EXE, "--port", str(PORT)], cwd=wd, stdout=logf, stderr=subprocess.STDOUT)
print("pid", proc.pid)
time.sleep(2.5)

s = socket.create_connection(("127.0.0.1", PORT), timeout=6)
s.settimeout(2)
frames = 0
try:
    s.sendall(pack(CMD_CAMERA, REQ.pack(1, 0, 0, 0, 5, 70)))   # START, 5fps
    t0 = time.time()
    while time.time() - t0 < 8:
        try:
            head = s.recv(12)
            if not head: print("server closed"); break
            mg, cmd, ln = HDR.unpack(head)
            body = b""
            while len(body) < ln:
                c = s.recv(ln - len(body))
                if not c: break
                body += c
            if cmd == CMD_CAMERA_FRAME:
                frames += 1
                if frames == 1:
                    seq, idx, w, h, fmt, nb = FH.unpack(body[:24])
                    print("first frame %dx%d fmt=%d bytes=%d" % (w, h, fmt, len(body)-24))
            else:
                print("[cmd=%d] %s" % (cmd, body.decode('utf-8', 'replace').strip()[:200]))
        except socket.timeout:
            print("  ... idle %.1fs frames=%d alive=%s" % (time.time()-t0, frames, proc.poll() is None))
except Exception as e:
    print("EXC", type(e).__name__, e)
print("frames total", frames, "server alive:", proc.poll() is None, "exit:", proc.poll())
try: s.close()
except: pass
time.sleep(1)
if proc.poll() is None:
    proc.terminate(); time.sleep(0.5)
logf.close()
print("=== srv stdout ===")
print(open(logp, "rb").read().decode("gbk", "replace")[-1500:])
ag = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "RemoteControl", "agent.log")
if os.path.exists(ag):
    print("=== agent.log tail ===")
    print(open(ag, "rb").read().decode("gbk", "replace")[-1500:])
print("dir", wd)
