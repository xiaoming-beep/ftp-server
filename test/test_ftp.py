#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ftp-server 冒烟测试：覆盖登录、全部命令、PASV/PORT 两种数据模式、
-ro 只读、路径越界防护、并发下载。

用法: python test/test_ftp.py [ftp-server.exe] [端口]
"""
import ftplib
import io
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, "..", "build",
                           "ftp-server" + (".exe" if os.name == "nt" else ""))
EXE = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3721
RO_PORT = PORT + 1  # 只读模式服务器端口
STALL_PORT = PORT + 2  # -stall 短超时服务器端口（停滞传输/连接超时测试用）

ROOT = tempfile.mkdtemp(prefix="ftp_srv_test_")
procs = []
passed = []
failed = []


def banner(t):
    print("\n== " + t + " ==")


def check(name, cond, extra=""):
    if cond:
        passed.append(name)
        print("  PASS  " + name)
    else:
        failed.append(name)
        print("  FAIL  " + name + ("   [" + extra + "]" if extra else ""))


def start(port, extra=None):
    p = subprocess.Popen(
        [EXE, "-path", ROOT, "-port", str(port)] + (extra or []),
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    procs.append(p)
    for _ in range(200):
        try:
            f = ftplib.FTP()
            f.connect("127.0.0.1", port, timeout=2)
            f.quit()
            return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError("服务器未能启动: " + (p.stderr.read() or "无错误输出"))


def expects_550(fn, *a, **k):
    """期望操作抛出 550 权限/不存在错误"""
    try:
        fn(*a, **k)
        return False
    except ftplib.error_perm as e:
        return str(e).startswith("550")


def expect_prefix(fn, prefix, *a, **k):
    """期望应答（无论返回还是抛出）以指定前缀开头"""
    try:
        r = fn(*a, **k)
        return isinstance(r, str) and r.startswith(prefix)
    except ftplib.Error as e:
        return str(e).startswith(prefix)


def raw_ctrl(port):
    """裸 socket 控制连接，返回 (sock, cmd, read_reply)。
    用于 ftplib 发不出来的命令（非法字节、需要自己管理数据连接等场景）。"""
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    f = s.makefile("rb")

    def read_reply():
        return f.readline().decode("utf-8", "replace").strip()

    def cmd(c):
        s.sendall(c if isinstance(c, bytes) else (c + "\r\n").encode())
        return read_reply()

    read_reply()  # 220 欢迎语
    cmd("USER a")
    cmd("PASS b")
    cmd("TYPE I")
    return s, cmd, read_reply


def main():
    start(PORT)
    start(RO_PORT, ["-ro"])
    start(STALL_PORT, ["-stall", "2"])

    ftp = ftplib.FTP()
    ftp.connect("127.0.0.1", PORT, timeout=10)
    check("欢迎语 220", ftp.getwelcome().startswith("220"))

    banner("登录与会话命令")
    check("任意账号密码登录", ftp.login("admin", "123456") == "230 Login successful")
    check("SYST 返回 UNIX", "UNIX" in ftp.sendcmd("SYST"))
    check("FEAT 含 UTF8", "UTF8" in ftp.sendcmd("FEAT"))
    check("NOOP", ftp.sendcmd("NOOP").startswith("200"))
    check("未知命令 502", expect_prefix(ftp.sendcmd, "502", "BOGUS"))
    check("TYPE A", ftp.sendcmd("TYPE A").startswith("200"))
    check("TYPE I", ftp.sendcmd("TYPE I").startswith("200"))
    check("非法 TYPE 504", expect_prefix(ftp.sendcmd, "504", "TYPE X"))
    check("REST 断点参数", ftp.sendcmd("REST 100").startswith("350"))
    check("非法 REST 501", expect_prefix(ftp.sendcmd, "501", "REST abc"))

    # 未登录时的 530
    f2 = ftplib.FTP()
    f2.connect("127.0.0.1", PORT, timeout=10)
    check("未登录执行命令 530", expect_prefix(f2.sendcmd, "530", "PWD"))
    f2.quit()

    banner("目录与路径")
    check("初始 PWD = /", ftp.pwd() == "/")
    check("MKD dir1", ftp.mkd("dir1") == "/dir1")
    check("CWD dir1", ftp.cwd("dir1").startswith("250") and ftp.pwd() == "/dir1")
    check("CDUP 回根", ftp.cwd("..").startswith("250") and ftp.pwd() == "/")
    check("嵌套 MKD", ftp.mkd("dir1/sub") == "/dir1/sub")
    check("CWD 不存在的目录 550", expects_550(ftp.cwd, "nope"))
    check("RMD 空目录", ftp.rmd("dir1/sub").startswith("250"))
    check("RMD 不存在 550", expects_550(ftp.rmd, "nope"))
    # 路径越界防护
    check("CWD .. 越界 550", expects_550(ftp.cwd, ".."))
    check("RETR 越界 550", expects_550(ftp.retrbinary, "RETR ../secret.txt", lambda b: None))
    check("RETR 绝对越界 550", expects_550(ftp.retrbinary, "RETR /../../etc/passwd", lambda b: None))
    check("CWD 盘符 550", expects_550(ftp.cwd, "C:/"))

    banner("上传下载")
    payload = bytes(random.randrange(256) for _ in range(65536))
    with open(os.path.join(ROOT, "blob.bin"), "wb") as f:
        f.write(payload)
    check("STOR 上传", ftp.storbinary("STOR up.bin", io.BytesIO(payload)).startswith("226"))
    got = io.BytesIO()
    check("RETR 下载一致", ftp.retrbinary("RETR up.bin", got.write).startswith("226")
          and got.getvalue() == payload)
    check("SIZE 正确", ftp.size("up.bin") == len(payload))
    check("SIZE 不存在 550", expects_550(ftp.size, "nope"))
    check("MDTM 格式", len(ftp.sendcmd("MDTM up.bin").split()[1]) == 14)
    t0 = 1767225600  # 2026-01-01 00:00:00 UTC
    os.utime(os.path.join(ROOT, "up.bin"), (t0, t0))
    check("MDTM 为 UTC (RFC 3659)",
          ftp.sendcmd("MDTM up.bin") == "213 " + time.strftime("%Y%m%d%H%M%S", time.gmtime(t0)))
    check("DELE", ftp.delete("up.bin").startswith("250"))
    check("DELE 不存在 550", expects_550(ftp.delete, "nope"))

    # ASCII 模式换行转换
    ftp.voidcmd("TYPE A")
    conn = ftp.transfercmd("STOR a.txt")
    conn.sendall(b"line1\r\nline2\nline3\r\n")
    conn.close()
    ftp.voidresp()
    check("TYPE A 存储换行归一化",
          open(os.path.join(ROOT, "a.txt"), "rb").read() == b"line1\nline2\nline3\n")
    conn = ftp.transfercmd("RETR a.txt")
    chunks = []
    while True:
        c = conn.recv(4096)
        if not c:
            break
        chunks.append(c)
    conn.close()
    ftp.voidresp()
    check("TYPE A 下载 CRLF", b"".join(chunks) == b"line1\r\nline2\r\nline3\r\n")
    ftp.voidcmd("TYPE I")

    # APPE 追加
    ftp.storbinary("STOR ap.txt", io.BytesIO(b"abc"))
    ftp.storbinary("APPE ap.txt", io.BytesIO(b"def"))
    check("APPE 追加", open(os.path.join(ROOT, "ap.txt"), "rb").read() == b"abcdef")

    # REST 断点续传
    half = len(payload) // 2
    got2 = io.BytesIO()
    ftp.retrbinary("RETR blob.bin", got2.write, rest=half)
    check("REST 断点下载", got2.getvalue() == payload[half:])

    # STOR 覆盖
    ftp.storbinary("STOR blob.bin", io.BytesIO(b"x"))
    check("STOR 覆盖", open(os.path.join(ROOT, "blob.bin"), "rb").read() == b"x")
    ftp.storbinary("STOR blob.bin", io.BytesIO(payload))  # 还原

    check("STOR 父目录不存在 550",
          expects_550(ftp.storbinary, "STOR nope/x.bin", io.BytesIO(b"z")))

    banner("列表")
    with open(os.path.join(ROOT, "dir1", "in1.txt"), "wb") as f:
        f.write(b"hello")
    lines = []
    ftp.retrlines("LIST", lines.append)
    check("LIST 包含文件", any("blob.bin" in l for l in lines))
    check("LIST 包含目录", any(l.startswith("d") and "dir1" in l for l in lines))
    names = ftp.nlst()
    check("NLST 包含名字", "blob.bin" in names and "dir1" in names)

    # 中文文件名（UTF-8）
    zh = "中文文件.txt"
    ftp.storbinary("STOR " + zh, io.BytesIO(b"ok"))
    check("中文名上传", os.path.exists(os.path.join(ROOT, zh)))
    check("中文名列表", zh in ftp.nlst())
    out3 = io.BytesIO()
    ftp.retrbinary("RETR " + zh, out3.write)
    check("中文名下载", out3.getvalue() == b"ok")
    ftp.delete(zh)

    banner("重命名")
    ftp.storbinary("STOR rn.bin", io.BytesIO(b"rn"))
    check("RNFR/RNTO", ftp.rename("rn.bin", "rn2.bin").startswith("250")
          and os.path.exists(os.path.join(ROOT, "rn2.bin")))
    check("无 RNFR 直接 RNTO 503", expect_prefix(ftp.sendcmd, "503", "RNTO x"))
    check("RNFR 不存在 550", expects_550(ftp.sendcmd, "RNFR nope.bin"))

    banner("PORT 主动模式")
    ftp.set_pasv(False)
    got4 = io.BytesIO()
    check("PORT 下载", ftp.retrbinary("RETR blob.bin", got4.write).startswith("226")
          and got4.getvalue() == payload)
    check("PORT 上传", ftp.storbinary("STOR via_port.bin", io.BytesIO(b"pp")).startswith("226")
          and open(os.path.join(ROOT, "via_port.bin"), "rb").read() == b"pp")
    ftp.set_pasv(True)

    banner("并发下载")
    errors = []

    def dl(i):
        try:
            f = ftplib.FTP()
            f.connect("127.0.0.1", PORT, timeout=15)
            f.login("u", "p")
            g = io.BytesIO()
            f.retrbinary("RETR blob.bin", g.write)
            if g.getvalue() != payload:
                errors.append("线程%d 内容不一致" % i)
            f.quit()
        except Exception as e:
            errors.append("线程%d: %s" % (i, e))

    ts = [threading.Thread(target=dl, args=(i,)) for i in range(4)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    check("4 线程并发下载", not errors, str(errors))

    banner("安全加固")
    # 防 FTP 弹跳攻击：PORT/EPRT 目标必须就是客户端自己，且禁止低端口
    check("PORT 弹跳目标 501", expect_prefix(ftp.sendcmd, "501", "PORT 8,8,8,8,4,1"))
    check("PORT 低端口目标 501", expect_prefix(ftp.sendcmd, "501", "PORT 127,0,0,1,0,22"))
    check("EPRT 弹跳目标 501", expect_prefix(ftp.sendcmd, "501", "EPRT |1|8.8.8.8|4444|"))
    check("PORT 本机合法目标 200", ftp.sendcmd("PORT 127,0,0,1,255,254").startswith("200"))

    # 非法 UTF-8 文件名必须拒绝，而不是静默解析到错误位置
    s, cmd, rr = raw_ctrl(PORT)
    check("非法 UTF-8 文件名 550",
          cmd(b"RETR \xff\xfe.bin\r\n").startswith("550"))
    s.close()

    # PASV 数据端口来源校验：异源冒充者先连会被丢弃，真客户端传输不受影响。
    # 冒充者用 127.0.0.2 作为源地址（loopback 网段内异于控制连接的 127.0.0.1）
    s, cmd, rr = raw_ctrl(PORT)
    r = cmd("PASV")
    nums = r.split("(")[1].rstrip(")").split(",")
    dport = int(nums[4]) * 256 + int(nums[5])
    thief = socket.socket()
    try:
        thief.bind(("127.0.0.2", 0))
    except OSError:
        print("  SKIP  PASV 来源校验（本机不支持 127.0.0.2 回环源地址）")
        thief.close()
        s.close()
    else:
        thief.settimeout(3)
        thief.connect(("127.0.0.1", dport))
        time.sleep(0.2)
        s.sendall(b"RETR blob.bin\r\n")  # 服务器 accept 到冒充者并将其关闭
        thief.settimeout(5)
        thief_closed = False
        try:
            while thief.recv(4096):
                pass
            thief_closed = True  # recv 返回空 -> 服务器关闭了冒充者连接
        except socket.timeout:
            pass
        real = socket.create_connection(("127.0.0.1", dport), timeout=3)
        got5 = b""
        real.settimeout(10)
        while True:
            c = real.recv(65536)
            if not c:
                break
            got5 += c
        real.close()
        thief.close()
        check("PASV 异源抢占连接被丢弃、真客户端传输正常",
              thief_closed and rr().startswith("150") and rr().startswith("226")
              and got5 == payload)
        s.close()

    banner("停滞传输与连接超时 (-stall 2)")
    # 黑洞 listener：接受连接但永不读取 -> 服务器停滞超时后应回 426 并释放文件
    with open(os.path.join(ROOT, "stall.bin"), "wb") as f:
        f.write(os.urandom(16 * 1024 * 1024))
    bh = socket.socket()
    bh.bind(("127.0.0.1", 0))
    bh.listen(1)
    bh_port = bh.getsockname()[1]
    holder = []  # 持有 accept 到的连接，防止 GC 关闭它（否则 RST 会立刻中断传输）

    def black_hole():
        conn, _ = bh.accept()
        holder.append(conn)

    threading.Thread(target=black_hole, daemon=True).start()
    s, cmd, rr = raw_ctrl(STALL_PORT)
    cmd("PORT 127,0,0,1,%d,%d" % (bh_port >> 8, bh_port & 0xFF))
    t = time.time()
    cmd("RETR stall.bin")  # 150
    reply = rr()
    elapsed = time.time() - t
    check("停滞传输超时 426 (%.1fs)" % elapsed,
          reply.startswith("426") and 1.5 < elapsed < 20)
    s.close()

    # PORT 目标本机无监听端口 -> 连接被拒应回 425（仅一条应答）
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    free_port = probe.getsockname()[1]
    probe.close()
    s, cmd, rr = raw_ctrl(STALL_PORT)
    cmd("PORT 127,0,0,1,%d,%d" % (free_port >> 8, free_port & 0xFF))
    t = time.time()
    reply = cmd("RETR blob.bin")
    elapsed = time.time() - t
    check("PORT 连接失败 425 (%.1fs)" % elapsed,
          reply.startswith("425") and elapsed < 25)
    s.close()

    banner("只读模式 (-ro)")
    ro = ftplib.FTP()
    ro.connect("127.0.0.1", RO_PORT, timeout=10)
    check("ro 登录", ro.login("a", "b").startswith("230"))
    check("ro RETR 正常", ro.retrbinary("RETR blob.bin", lambda b: None).startswith("226"))
    check("ro LIST 正常", ro.retrlines("LIST", lambda l: None).startswith("226"))
    check("ro STOR 550", expects_550(ro.storbinary, "STOR x.bin", io.BytesIO(b"x")))
    check("ro APPE 550", expects_550(ro.storbinary, "APPE blob.bin", io.BytesIO(b"x")))
    check("ro MKD 550", expects_550(ro.mkd, "newdir"))
    check("ro RMD 550", expects_550(ro.rmd, "dir1"))
    check("ro DELE 550", expects_550(ro.delete, "blob.bin"))
    check("ro RNFR 550", expects_550(ro.sendcmd, "RNFR blob.bin"))
    check("ro RNTO 550", expects_550(ro.sendcmd, "RNTO x.bin"))
    ro.quit()

    ftp.quit()
    print("\n结果: %d 通过, %d 失败" % (len(passed), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        rc = main()
    finally:
        for p in procs:
            p.terminate()
            try:
                p.wait(timeout=5)
            except Exception:
                p.kill()
        shutil.rmtree(ROOT, ignore_errors=True)
    sys.exit(rc)
