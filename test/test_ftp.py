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
import subprocess
import sys
import tempfile
import threading
import time

EXE = sys.argv[1] if len(sys.argv) > 1 else r"M:\ftp-server\build\ftp-server.exe"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3721
RO_PORT = PORT + 1  # 只读模式服务器端口

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


def main():
    start(PORT)
    start(RO_PORT, ["-ro"])

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
