"""
test_agent.py -- protocol test script for the RemoteOps Agent (IT24103736)

Usage (from the folder that contains agent_736):
    terminal 1:  ./agent_736
    terminal 2:  python3 test_agent.py

It checks AUTH, framing (partial / combined lines), EXEC whitelist, PUT/GET byte-for-byte,
FILE_TOO_LARGE, UDP monitoring, 8 concurrent controllers and ungraceful disconnects.
It reads ./agentfiles/IT24103736/ so run it from the same folder as the Agent.
Written with AI assistance (see prompt log).
"""
import socket, os, time, threading, hashlib, sys
HOST, PORT, SID = "127.0.0.1", 9410, "SID:6373"
fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (" -> " + str(info) if not cond else ""))
    if not cond: fails += 1
class C:
    def __init__(s):
        s.sk = socket.create_connection((HOST, PORT)); s.buf = b""
    def send(s, b): s.sk.sendall(b if isinstance(b, bytes) else b.encode())
    def line(s):
        while b"\n" not in s.buf:
            d = s.sk.recv(65536)
            if not d: return None
            s.buf += d
        l, s.buf = s.buf.split(b"\n", 1); return l.decode()
    def body(s, n):
        while len(s.buf) < n:
            d = s.sk.recv(65536)
            if not d: break
            s.buf += d
        b, s.buf = s.buf[:n], s.buf[n:]; return b
    def cmd(s, c): s.send(c + "\n"); return s.line()
    def auth(s): return s.cmd("AUTH OPS-3736")

c = C()
r = c.cmd("SYSINFO"); check("unauth rejected", r == "ERR 006 NOT_AUTHENTICATED " + SID, r)
r = c.cmd("AUTH wrong"); check("bad token", r == "ERR 001 AUTH_FAILED " + SID, r)
r = c.auth(); check("good token", r == "OK AUTHENTICATED " + SID, r)

# framing: partial lines + several lines in one send
c.send("SYS"); time.sleep(0.2); c.send("INFO\nLISTP"); time.sleep(0.2); c.send("ROC\nEXEC HOST"); time.sleep(0.2); c.send("NAME\n")
r1, r2, r3 = c.line(), c.line(), c.line()
import re
check("partial SYSINFO", re.fullmatch(r"OK SYSINFO [\d.]+ \d+ \d+ SID:6373", r1 or ""), r1)
check("partial LISTPROC", (r2 or "").startswith("OK PROCS ") and r2.endswith(SID) and ":" in r2, (r2 or "")[:80])
check("partial EXEC", (r3 or "").startswith("OK EXEC_RESULT ") and r3.endswith(SID), r3)
c.send("SYSINFO\nSYSINFO\nSYSINFO\n")
check("3 lines one buffer", all(re.fullmatch(r"OK SYSINFO .* SID:6373", c.line() or "") for _ in range(3)))

for n in ["DATE","UPTIME","DISKFREE","HOSTNAME","WHOAMI"]:
    r = c.cmd("EXEC " + n); check("EXEC " + n, r.startswith("OK EXEC_RESULT ") and r.endswith(SID), r)
for bad in ["EXEC rm -rf /", "EXEC date", "EXEC DATE; id", "EXEC", "EXEC LS"]:
    r = c.cmd(bad); check("reject '%s'" % bad, r in ("ERR 002 COMMAND_NOT_ALLOWED " + SID, "ERR 007 BAD_ARGUMENTS " + SID), r)
r = c.cmd("FOO"); check("unknown cmd", r == "ERR 003 UNKNOWN_COMMAND " + SID, r)

# PUT in awkward chunks, with next command glued to the body
data = os.urandom(300000)
c.send("PUT sample.bin %d\n" % len(data) + "")
i = 0
for step in (1, 7, 4096, 100000):
    c.send(data[i:i+step]); i += step; time.sleep(0.05)
c.send(data[i:] + b"SYSINFO\n")
r = c.line(); check("PUT ok", r == "OK FILE_RECEIVED sample.bin " + SID, r)
r = c.line(); check("cmd glued after body", (r or "").startswith("OK SYSINFO"), r)
c.send("GET sample.bin\n"); h = c.line()
check("GET header", h == "OK FILE_SEND sample.bin %d %s" % (len(data), SID), h)
got = c.body(len(data)); check("GET byte-identical", got == data)
check("stored on disk identical", open("agentfiles/IT24103736/sample.bin","rb").read() == data)
r = c.cmd("SYSINFO"); check("in sync after GET", (r or "").startswith("OK SYSINFO"), r)
# zero byte file
r = c.cmd("PUT empty.txt 0"); check("PUT 0 bytes", r == "OK FILE_RECEIVED empty.txt " + SID, r)
c.send("GET empty.txt\n"); check("GET 0 bytes", c.line() == "OK FILE_SEND empty.txt 0 " + SID)
r = c.cmd("GET nothere.txt"); check("GET missing", r == "ERR 005 FILE_NOT_FOUND " + SID, r)
r = c.cmd("GET ../etc/passwd"); check("traversal GET", (r or "").startswith("ERR 008"), r)
c.send("PUT ../evil 5\nhello"); r = c.line(); check("traversal PUT", (r or "").startswith("ERR 008"), r)
check("evil not created", not os.path.exists("agentfiles/evil") and not os.path.exists("evil"))
# too large (11MB): body must be drained, stream stays in sync
big = 11 * 1024 * 1024
c.send("PUT big.bin %d\n" % big)
chunk = b"x" * 65536; sent = 0
while sent < big:
    n = min(len(chunk), big - sent); c.send(chunk[:n]); sent += n
r = c.line(); check("FILE_TOO_LARGE", r == "ERR 004 FILE_TOO_LARGE " + SID, r)
r = c.cmd("SYSINFO"); check("in sync after too-large", (r or "").startswith("OK SYSINFO"), r)

# UDP monitor
us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); us.bind(("127.0.0.1", 9555)); us.settimeout(5)
r = c.cmd("MONITOR START 9555"); check("MONITOR START", r == "OK MONITOR_STARTED " + SID, r)
d1 = us.recvfrom(512)[0].decode().strip(); d2 = us.recvfrom(512)[0].decode().strip()
check("UDP datagrams w/ SID", all(re.fullmatch(r"SYSINFO [\d.]+ \d+ \d+ SID:6373", d) for d in (d1, d2)), (d1, d2))
r = c.cmd("MONITOR STOP"); check("MONITOR STOP", r == "OK MONITOR_STOPPED " + SID, r)
us.settimeout(0.5)
try:
    while True: us.recvfrom(512)  # drain anything in flight
except socket.timeout: pass
us.settimeout(4.5)
try: us.recvfrom(512); stopped = False
except socket.timeout: stopped = True
check("UDP stops after STOP", stopped)
r = c.cmd("MONITOR START abc"); check("MONITOR bad port", r == "ERR 007 BAD_ARGUMENTS " + SID, r)
r = c.cmd("QUIT"); check("QUIT", r == "OK BYE " + SID, r)
check("closed after QUIT", c.sk.recv(10) == b"")

# 8 concurrent clients
res = []
def worker2(k):
    try:
        w = C(); assert w.auth() == "OK AUTHENTICATED " + SID
        d = os.urandom(50000 + k); name = "c%d.bin" % k
        w.send("PUT %s %d\n" % (name, len(d))); w.send(d)
        assert w.line() == "OK FILE_RECEIVED %s %s" % (name, SID)
        w.send("MONITOR START %d\n" % (9600 + k)); assert w.line().startswith("OK MONITOR_STARTED")
        time.sleep(0.5)
        w.send("GET %s\n" % name); h = w.line(); assert h == "OK FILE_SEND %s %d %s" % (name, len(d), SID), h
        assert w.body(len(d)) == d
        assert w.cmd("QUIT") == "OK BYE " + SID
        res.append((k, "ok"))
    except Exception as e: res.append((k, repr(e)))
ts = [threading.Thread(target=worker2, args=(k,)) for k in range(8)]
[t.start() for t in ts]; [t.join() for t in ts]
check("8 concurrent clients", len(res) == 8 and all(v == "ok" for _, v in res), res)

# ungraceful disconnects: mid-PUT, with monitor running, and plain RST
w = C(); w.auth(); w.send("MONITOR START 9700\n"); w.line()
w.send("PUT half.bin 100000\n"); w.send(b"a" * 1000); w.sk.close()
w = C(); w.auth(); w.send("PUT half2.bin 100000\n"); w.send(b"a" * 10)
import struct; w.sk.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)); w.sk.close()
w = C(); w.send("AUTH OPS-3736\nSYSINFO\n"); w.sk.close()
time.sleep(0.5)
w = C(); r = w.auth(); check("agent alive after ungraceful disconnects", r == "OK AUTHENTICATED " + SID, r)
check("partial upload removed", not [f for f in os.listdir("agentfiles/IT24103736") if f.startswith("half")], os.listdir("agentfiles/IT24103736"))
# 3 bad auths -> dropped
w = C()
for _ in range(3): w.cmd("AUTH nope")
check("dropped after 3 bad AUTH", w.sk.recv(10) == b"")
print("FAILS:", fails); sys.exit(1 if fails else 0)
