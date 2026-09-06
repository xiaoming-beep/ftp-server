// session.cpp - FTP 会话实现：命令分发、数据连接、文件传输
#include "session.h"

#include "util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace ftp {

using namespace std;
namespace fs = filesystem;
using asio::ip::tcp;

static constexpr auto kIdleTimeout = chrono::minutes(10); // 控制连接空闲超时
static constexpr auto kDataAcceptTimeout = chrono::seconds(30); // PASV 等待客户端连接
static constexpr auto kConnectTimeout = chrono::seconds(10);    // PORT 主动连接超时

Session::Session(Server& server, shared_ptr<tcp::socket> control)
    : server_(server), vfs_(server.vfs()), control_(std::move(control)),
      stall_timeout_(server.config().stall_timeout) {}

void Session::close() { closing_.store(true); }

// ---------------- 控制连接 ----------------

bool Session::read_line(string& out) {
    while (true) {
        auto nl = recv_buf_.find('\n');
        if (nl != string::npos) {
            out = recv_buf_.substr(0, nl);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            recv_buf_.erase(0, nl + 1);
            last_activity_ = chrono::steady_clock::now();
            return true;
        }
        if (recv_buf_.size() > 8192) return false; // 行过长，断开

        char buf[2048];
        error_code ec;
        size_t n = control_->receive(asio::buffer(buf), 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            if (closing_.load() || server_.stopping()) return false;
            if (chrono::steady_clock::now() - last_activity_ > kIdleTimeout) {
                log_info("Control connection idle timeout, disconnecting");
                return false;
            }
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }
        if (ec || n == 0) return false;
        recv_buf_.append(buf, n);
    }
}

bool Session::send_raw(const string& data) {
    size_t off = 0;
    auto last_progress = chrono::steady_clock::now();
    while (off < data.size()) {
        error_code ec;
        size_t n = control_->send(asio::buffer(data.data() + off, data.size() - off), 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            if (closing_.load()) return false;
            if (chrono::steady_clock::now() - last_progress > stall_timeout_) {
                log_error("Control connection send stall timeout, disconnecting");
                return false;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
            continue;
        }
        if (ec) return false;
        off += n;
        last_progress = chrono::steady_clock::now();
    }
    return true;
}

bool Session::send_reply(int code, const string& text) {
    char head[16];
    snprintf(head, sizeof head, "%d ", code);
    return send_raw(string(head) + text + "\r\n");
}

bool Session::send_multiline(int code, const string& first,
                             const vector<string>& lines, const string& last) {
    char head[16];
    snprintf(head, sizeof head, "%d-", code);
    string out = string(head) + first + "\r\n";
    for (auto& l : lines) out += " " + l + "\r\n";
    snprintf(head, sizeof head, "%d ", code);
    out += string(head) + last + "\r\n";
    return send_raw(out);
}

// ---------------- 数据连接 ----------------

// 地址相等性比较（把 IPv4-mapped IPv6 归一化后比较）
static bool addr_equal(const asio::ip::address& a, const asio::ip::address& b) {
    asio::ip::address x = a, y = b;
    if (x.is_v6() && x.to_v6().is_v4_mapped())
        x = asio::ip::make_address_v4(asio::ip::v4_mapped, x.to_v6());
    if (y.is_v6() && y.to_v6().is_v4_mapped())
        y = asio::ip::make_address_v4(asio::ip::v4_mapped, y.to_v6());
    return x == y;
}

static bool wait_socket_ready(tcp::socket& s, int timeout_ms);

bool Session::open_data_conn(shared_ptr<tcp::socket>& out) {
    auto stop_wait = [&]() {
        return closing_.load() || server_.stopping();
    };

    if (pasv_) {
        // PASV 模式下必须先回 150 再等数据连接：Windows 资源管理器的 FTP 客户端
        // 收到 150 之前不会连接数据端口，反过来等会让双方互相卡死。
        // PORT 模式不需要：客户端在发命令前就已监听数据端口。
        send_reply(150, "Opening data connection");
        // 同步非阻塞 accept + 轮询：asio 异步 accept 存在边缘触发丢事件的问题
        // （客户端赶在命令到达前连接时，事件可能被之前 transfer 的轮询吞掉，
        // accept 永不完成，客户端超时）。轮询是电平判断，连接一定可见。
        error_code ec;
        pasv_->non_blocking(true, ec);
        auto deadline = chrono::steady_clock::now() + kDataAcceptTimeout;
        auto acc = pasv_;
        while (true) {
            if (stop_wait() || chrono::steady_clock::now() >= deadline) break;
            shared_ptr<tcp::socket> s = make_shared<tcp::socket>(ioc_);
            error_code ce;
            acc->accept(*s, ce);
            if (!ce) {
                tcp::endpoint rem = s->remote_endpoint(ce);
                if (!ce && addr_equal(rem.address(), peer_addr_)) {
                    s->non_blocking(true, ce);
                    pasv_.reset();
                    out = s;
                    return true;
                }
                // 来源不是控制连接的对端：丢弃这条连接并继续等待（防 PASV 抢占）
                s->close(ce);
                continue;
            }
            if (ce == asio::error::would_block || ce == asio::error::try_again) {
                this_thread::sleep_for(chrono::milliseconds(10));
                continue;
            }
            break; // 其他错误：放弃本次数据连接
        }
        pasv_.reset();
        return false;
    }

    if (port_ep_) {
        // 非阻塞 connect + select 等待完成：主动模式连接也走异步反应器的话，
        // 与 PASV 同样存在事件丢失风险（见上面注释）。select + SO_ERROR
        // 不依赖反应器，各平台行为一致。
        error_code oe;
        tcp::endpoint ep = *port_ep_;
        shared_ptr<tcp::socket> s = make_shared<tcp::socket>(ioc_);
        s->open(ep.protocol(), oe);
        if (!oe) s->non_blocking(true, oe);
        if (oe) { port_ep_.reset(); return false; }
        auto deadline = chrono::steady_clock::now() + kConnectTimeout;
        error_code ce;
        s->connect(ep, ce);
        if (ce && ce != asio::error::in_progress && ce != asio::error::would_block) {
            port_ep_.reset();
            return false; // 连接被拒/网络不可达等明确失败
        }
        while (ce) {
            if (stop_wait() || chrono::steady_clock::now() >= deadline) {
                port_ep_.reset();
                return false;
            }
            if (!wait_socket_ready(*s, 100)) continue; // 还没结果，继续等
            int soerr = 0;
#ifdef _WIN32
            int len = sizeof(soerr);
#else
            socklen_t len = sizeof(soerr);
#endif
            ::getsockopt(s->native_handle(), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&soerr), &len);
            if (soerr == 0) {
                ce = error_code(); // 连接完成
            } else if (soerr != asio::error::in_progress &&
                       soerr != asio::error::would_block) {
                ce = error_code(soerr, asio::error::get_system_category());
                port_ep_.reset();
                return false;
            }
            // soerr 仍是 in_progress：继续等待（理论上不会走到）
        }
        port_ep_.reset();
        send_reply(150, "Opening data connection");
        out = s;
        return true;
    }
    return false; // 客户端未先发 PASV/EPSV/PORT/EPRT
}

// select 等待 socket 可写（用于非阻塞 connect 完成检测）。
// 返回 true 表示已有结果（可写/出错），false 表示超时。
static bool wait_socket_ready(tcp::socket& s, int timeout_ms) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s.native_handle(), &wset);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    int r = ::select(0, nullptr, &wset, nullptr, &tv);
#else
    int r = ::select(int(s.native_handle()) + 1, nullptr, &wset, nullptr, &tv);
#endif
    return r > 0;
}

bool Session::send_all(tcp::socket& s, const string& data) {
    return send_all(s, data.data(), data.size());
}

bool Session::send_all(tcp::socket& s, const char* data, size_t len) {
    size_t off = 0;
    auto last_progress = chrono::steady_clock::now();
    while (off < len) {
        error_code ec;
        size_t n = s.send(asio::buffer(data + off, len - off), 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            // Windows 上 asio 的 socket 底层可能是非阻塞的，忙等一小会儿重试
            if (closing_.load() || server_.stopping()) return false;
            if (chrono::steady_clock::now() - last_progress > stall_timeout_) {
                // 对端停滞（如客户端卡死/网络中断未挥手）：断开并释放文件句柄，
                // 否则线程会永远忙等，Windows 上被传的文件也会一直被占用无法删除
                log_error("Data connection send stall timeout, aborting transfer");
                return false;
            }
            this_thread::sleep_for(chrono::milliseconds(10));
            continue;
        }
        if (ec) return false;
        off += n;
        last_progress = chrono::steady_clock::now();
    }
    return true;
}

bool Session::send_file(tcp::socket& data, const fs::path& real) {
    ifstream f(real, ios::binary);
    if (!f) return false;
    f.seekg((streamoff)rest_);

    char in[65536];
    char out[131072];
    char carry = '\0'; // 上一个字节，用于跨块处理 CRLF
    while (true) {
        f.read(in, sizeof in);
        streamsize n = f.gcount();
        if (n <= 0) break;

        size_t m = 0;
        if (type_ == 'A') {
            // ASCII 模式：LF -> CRLF（CRLF 保持原样，避免 CRCRLF）
            for (streamsize i = 0; i < n; ++i) {
                char c = in[i];
                if (c == '\n' && carry != '\r') {
                    out[m++] = '\r';
                    out[m++] = '\n';
                } else {
                    out[m++] = c;
                }
                carry = c;
            }
        } else {
            memcpy(out, in, (size_t)n);
            m = (size_t)n;
        }
        if (!send_all(data, out, m)) return false;
    }
    return true;
}

bool Session::recv_file(tcp::socket& data, const fs::path& real, bool append) {
    ofstream f;
    if (append) {
        f.open(real, ios::binary | ios::app);
    } else if (rest_ > 0) {
        // REST 续传：不截断，从偏移处开始写
        f.open(real, ios::binary | ios::out);
        f.seekp((streamoff)rest_);
    } else {
        f.open(real, ios::binary | ios::out | ios::trunc);
    }
    if (!f) return false;

    bool pending_cr = false;
    char buf[65536];
    auto last_progress = chrono::steady_clock::now();
    while (true) {
        error_code ec;
        size_t n = data.receive(asio::buffer(buf), 0, ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            // Windows 上 asio 的 socket 底层可能是非阻塞的：没数据就等一会儿
            if (closing_.load() || server_.stopping()) return false;
            if (chrono::steady_clock::now() - last_progress > stall_timeout_) {
                // 对端停滞：断开并关闭文件（析构即释放句柄），
                // 避免线程永久占用导致服务器上的文件无法删除
                log_error("Data connection receive stall timeout, aborting transfer");
                return false;
            }
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }
        if (ec == asio::error::eof) break; // Windows IOCP 下客户端关闭连接以 eof 形式返回，视为正常结束
        if (ec) {
            log_error("recv_file receive failed: " + ec.message() + " (n=" + to_string(n) + ")");
            return false;
        }
        if (n == 0) break; // 客户端关闭数据连接 -> 传输正常结束
        last_progress = chrono::steady_clock::now();

        if (type_ == 'A') {
            // ASCII 模式：CRLF -> LF（CR 单独出现则保留）
            for (size_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (c == '\r') {
                    pending_cr = true;
                } else if (c == '\n') {
                    f.put('\n');
                    pending_cr = false;
                } else {
                    if (pending_cr) f.put('\r');
                    pending_cr = false;
                    f.put(c);
                }
            }
        } else {
            f.write(buf, (streamsize)n);
        }
    }
    if (pending_cr) f.put('\r');
    f.close();
    return true;
}

// ---------------- 命令分发 ----------------

void Session::handle_command(const string& line) {
    size_t sp = line.find(' ');
    string cmd = to_upper(sp == string::npos ? line : line.substr(0, sp));
    string arg = sp == string::npos ? "" : trim(line.substr(sp + 1));

    // REST 断点状态只对紧随其后的传输命令生效（与 vsftpd 等常见服务器一致），
    // 其他命令会清除它；RNFR 同理
    if (cmd != "REST" && cmd != "RETR" && cmd != "STOR" && cmd != "APPE")
        rest_ = 0;
    if (cmd != "RNFR" && cmd != "RNTO")
        rnfr_.reset();

    // 未登录时只允许会话类命令
    if (!logged_in_ && cmd != "USER" && cmd != "PASS" && cmd != "QUIT" &&
        cmd != "SYST" && cmd != "FEAT" && cmd != "HELP" && cmd != "OPTS" &&
        cmd != "NOOP" && cmd != "TYPE") {
        send_reply(530, "Not logged in");
        return;
    }

    const bool ro = server_.config().read_only;

    if (cmd == "USER") {
        user_ = arg;
        send_reply(331, "Password required");
    } else if (cmd == "PASS") {
        logged_in_ = true;
        send_reply(230, "Login successful");
    } else if (cmd == "QUIT") {
        want_quit_ = true;
        send_reply(221, "Goodbye");
    } else if (cmd == "NOOP") {
        send_reply(200, "OK");
    } else if (cmd == "SYST") {
        send_reply(215, "UNIX Type: L8"); // 与 unix 风格列表配套，客户端依赖此识别
    } else if (cmd == "FEAT") {
        send_multiline(211, "Features:", {"EPSV", "EPRT", "PASV", "MDTM", "SIZE",
                                          "REST STREAM", "UTF8"}, "End");
    } else if (cmd == "OPTS") {
        if (to_upper(arg).rfind("UTF8", 0) == 0)
            send_reply(200, "UTF8 mode always on");
        else
            send_reply(501, "Option not supported");
    } else if (cmd == "HELP") {
        send_reply(214, "Supported: USER PASS QUIT SYST FEAT TYPE STRU MODE PASV "
                        "EPSV PORT EPRT PWD CWD CDUP MKD RMD DELE RNFR RNTO LIST "
                        "NLST RETR STOR APPE REST SIZE MDTM ABOR NOOP");
    } else if (cmd == "TYPE") {
        if (arg == "A" || arg == "A N") {
            type_ = 'A';
            send_reply(200, "Type set to A");
        } else if (arg == "I" || arg == "L 8") {
            type_ = 'I';
            send_reply(200, "Type set to I");
        } else {
            send_reply(504, "Type not supported");
        }
    } else if (cmd == "STRU") {
        send_reply(arg == "F" ? 200 : 504,
                   arg == "F" ? "Structure set to F" : "Structure not supported");
    } else if (cmd == "MODE") {
        send_reply(arg == "S" ? 200 : 504,
                   arg == "S" ? "Mode set to S" : "Mode not supported");
    } else if (cmd == "PWD" || cmd == "XPWD") {
        string p = cwd_;
        // 路径中的引号按 RFC 959 加倍转义
        for (size_t i = p.find('"'); i != string::npos; i = p.find('"', i + 2))
            p.insert(i, 1, '"');
        send_reply(257, "\"" + p + "\"");
    } else if (cmd == "CWD" || cmd == "XCWD") {
        do_cwd(arg.empty() ? "/" : arg);
    } else if (cmd == "CDUP" || cmd == "XCUP") {
        do_cwd("..");
    } else if (cmd == "MKD" || cmd == "XMKD") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        if (!real) { send_reply(550, "Invalid path"); return; }
        error_code ec;
        if (fs::create_directory(*real, ec) && !ec) {
            send_reply(257, "\"" + vfs_.virtualize(*real) + "\" created");
        } else {
            send_reply(550, "Create directory failed: " + ec.message());
        }
    } else if (cmd == "RMD" || cmd == "XRMD") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real || !fs::is_directory(*real, ec) || ec) {
            send_reply(550, "No such directory");
            return;
        }
        if (fs::remove(*real, ec) && !ec) send_reply(250, "Directory removed");
        else send_reply(550, "Remove directory failed: " + ec.message());
    } else if (cmd == "DELE") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real || !fs::is_regular_file(*real, ec) || ec) {
            send_reply(550, "No such file");
            return;
        }
        if (fs::remove(*real, ec) && !ec) send_reply(250, "File deleted");
        else send_reply(550, "Delete failed: " + ec.message());
    } else if (cmd == "RNFR") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real || !fs::exists(*real, ec) || ec) {
            send_reply(550, "No such file or directory");
            return;
        }
        rnfr_ = vfs_.virtualize(*real);
        send_reply(350, "Ready for RNTO");
    } else if (cmd == "RNTO") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        if (!rnfr_) { send_reply(503, "RNFR required first"); return; }
        auto from = vfs_.resolve(cwd_, *rnfr_);
        auto to = vfs_.resolve(cwd_, arg);
        rnfr_.reset();
        if (!from || !to) { send_reply(550, "Invalid path"); return; }
        error_code ec;
        fs::rename(*from, *to, ec);
        if (ec) send_reply(550, "Rename failed: " + ec.message());
        else send_reply(250, "Renamed");
    } else if (cmd == "SIZE") {
        if (type_ != 'I') { send_reply(550, "Size not available in ASCII mode"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real || !fs::is_regular_file(*real, ec) || ec) {
            send_reply(550, "No such file");
            return;
        }
        auto sz = fs::file_size(*real, ec);
        if (ec) { send_reply(550, "Size not available"); return; }
        send_reply(213, to_string(sz));
    } else if (cmd == "MDTM") {
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real) { send_reply(550, "No such file"); return; }
        auto ft = fs::last_write_time(*real, ec);
        if (ec) { send_reply(550, "No such file"); return; }
        send_reply(213, mdtm_str(file_time_to_time_t(ft)));
    } else if (cmd == "PASV") {
        do_pasv(false);
    } else if (cmd == "EPSV") {
        do_pasv(true);
    } else if (cmd == "PORT") {
        do_port(arg);
    } else if (cmd == "EPRT") {
        do_eprt(arg);
    } else if (cmd == "LIST" || cmd == "NLST") {
        do_list(cmd == "LIST", arg);
    } else if (cmd == "RETR") {
        if (arg.empty()) { send_reply(501, "Missing file name"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        error_code ec;
        if (!real || !fs::is_regular_file(*real, ec) || ec) {
            send_reply(550, "No such file");
            return;
        }
        shared_ptr<tcp::socket> data;
        if (!open_data_conn(data)) { send_reply(425, "Can't open data connection"); return; }
        bool ok = send_file(*data, *real);
        rest_ = 0;
        send_reply(ok ? 226 : 426,
                   ok ? "Transfer complete" : "Connection closed; transfer aborted");
    } else if (cmd == "STOR" || cmd == "APPE") {
        if (ro) { send_reply(550, "Permission denied (server is read-only)"); return; }
        if (arg.empty()) { send_reply(501, "Missing file name"); return; }
        auto real = vfs_.resolve(cwd_, arg);
        if (!real) { send_reply(550, "Invalid path"); return; }
        error_code ec;
        if (fs::exists(*real, ec) && !ec && fs::is_directory(*real, ec)) {
            send_reply(550, "Is a directory");
            return;
        }
        if (!fs::is_directory(real->parent_path(), ec) || ec) {
            send_reply(550, "Parent directory does not exist");
            return;
        }
        shared_ptr<tcp::socket> data;
        if (!open_data_conn(data)) { send_reply(425, "Can't open data connection"); return; }
        bool ok = recv_file(*data, *real, cmd == "APPE");
        rest_ = 0;
        send_reply(ok ? 226 : 426,
                   ok ? "Transfer complete" : "Connection closed; transfer aborted");
    } else if (cmd == "REST") {
        char* end = nullptr;
        unsigned long long off = strtoull(arg.c_str(), &end, 10);
        if (!end || *end) { send_reply(501, "Bad REST parameter"); return; }
        rest_ = off;
        send_reply(350, "Restarting at " + to_string(off) + ". Send STORE or RETRIEVE");
    } else if (cmd == "ABOR") {
        send_reply(226, "ABOR command successful");
    } else {
        send_reply(502, "Command not implemented");
    }
}

// ---------------- 命令实现细节 ----------------

void Session::do_cwd(const string& path) {
    auto real = vfs_.resolve(cwd_, path);
    error_code ec;
    if (real && fs::is_directory(*real, ec) && !ec) {
        cwd_ = vfs_.virtualize(*real);
        send_reply(250, "Directory changed");
    } else {
        send_reply(550, "No such directory");
    }
}

void Session::do_pasv(bool extended) {
    port_ep_.reset();
    error_code ec;
    auto local = control_->local_endpoint(ec);
    if (ec) { send_reply(425, "No local address"); return; }

    // 绑定在与控制连接相同的本地地址上，端口自动分配
    pasv_ = make_shared<tcp::acceptor>(ioc_);
    tcp::endpoint bind_ep(local.address(), 0);
    pasv_->open(local.protocol(), ec);
    if (!ec) pasv_->set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec) pasv_->bind(bind_ep, ec);
    if (!ec) pasv_->listen(1, ec);
    if (ec) {
        pasv_.reset();
        send_reply(425, "Can't open passive connection: " + ec.message());
        return;
    }
    unsigned short port = pasv_->local_endpoint(ec).port();
    if (ec) { pasv_.reset(); send_reply(425, "Can't open passive connection"); return; }

    if (extended) {
        send_reply(229, "Entering Extended Passive Mode (|||" + to_string(port) + "|)");
        return;
    }
    // 经典 PASV：需要 IPv4 点分地址
    string ip;
    if (local.address().is_v4()) {
        ip = local.address().to_string();
    } else if (local.address().is_v6()) {
        if (local.address().to_v6().is_loopback())
            ip = "127.0.0.1";
        else {
            pasv_.reset();
            send_reply(425, "PASV not supported over IPv6, use EPSV");
            return;
        }
    } else {
        pasv_.reset();
        send_reply(425, "Unsupported address family");
        return;
    }
    // 227 (h1,h2,h3,h4,p1,p2)
    vector<string> octets;
    string cur;
    for (char c : ip) {
        if (c == '.') {
            octets.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    octets.push_back(cur);
    char buf[96];
    snprintf(buf, sizeof buf, "(%s,%s,%s,%s,%u,%u)", octets[0].c_str(),
             octets[1].c_str(), octets[2].c_str(), octets[3].c_str(),
             port >> 8, port & 0xFF);
    send_reply(227, "Entering Passive Mode " + string(buf));
}

void Session::do_port(const string& arg) {
    pasv_.reset();
    // h1,h2,h3,h4,p1,p2
    string a = arg;
    for (auto& c : a)
        if (c == ',') c = ' ';
    istringstream iss(a);
    int o[6];
    for (int i = 0; i < 6; ++i) {
        if (!(iss >> o[i]) || o[i] < 0 || o[i] > 255) {
            send_reply(501, "Bad PORT syntax");
            return;
        }
    }
    string ip = to_string(o[0]) + "." + to_string(o[1]) + "." + to_string(o[2]) +
                "." + to_string(o[3]);
    error_code ec;
    auto addr = asio::ip::make_address(ip, ec);
    if (ec) { send_reply(501, "Bad PORT address"); return; }
    // 防 FTP 弹跳攻击：数据连接目标必须就是控制连接的客户端本身，
    // 否则服务器会被滥用为对第三方主机/端口的扫描与攻击跳板
    if (!addr_equal(addr, peer_addr_)) {
        send_reply(501, "PORT address must match client address");
        return;
    }
    unsigned short port = (unsigned short)(o[4] * 256 + o[5]);
    if (port < 1024) { // 低端口目标是弹跳攻击的典型用法（SSH/SMTP 等），拒绝
        send_reply(501, "PORT target port must be >= 1024");
        return;
    }
    port_ep_ = tcp::endpoint(addr, port);
    send_reply(200, "PORT command successful");
}

void Session::do_eprt(const string& arg) {
    pasv_.reset();
    // |1|ip|port| 或 |2|ip|port|
    if (arg.size() < 7 || arg.front() != '|') {
        send_reply(501, "Bad EPRT syntax");
        return;
    }
    string s = arg.substr(1);
    size_t p1 = s.find('|');
    size_t p2 = p1 == string::npos ? string::npos : s.find('|', p1 + 1);
    size_t p3 = p2 == string::npos ? string::npos : s.find('|', p2 + 1);
    if (p1 == string::npos || p2 == string::npos || p3 == string::npos) {
        send_reply(501, "Bad EPRT syntax");
        return;
    }
    string ip = s.substr(p1 + 1, p2 - p1 - 1);
    string port_str = s.substr(p2 + 1, p3 - p2 - 1);
    error_code ec;
    auto addr = asio::ip::make_address(ip, ec);
    unsigned long port = 0;
    try {
        port = stoul(port_str);
    } catch (...) {
        ec = asio::error::invalid_argument;
    }
    if (ec || port == 0 || port > 65535) {
        send_reply(501, "Bad EPRT syntax");
        return;
    }
    // 防 FTP 弹跳攻击，同 PORT
    if (!addr_equal(addr, peer_addr_)) {
        send_reply(501, "EPRT address must match client address");
        return;
    }
    if (port < 1024) {
        send_reply(501, "EPRT target port must be >= 1024");
        return;
    }
    port_ep_ = tcp::endpoint(addr, (unsigned short)port);
    send_reply(200, "EPRT command successful");
}

void Session::do_list(bool with_details, const string& arg) {
    // 去掉开头的选项 token（如 -a、-la），剩余部分作为路径（可能含空格）
    string path = arg;
    if (!path.empty() && path[0] == '-') {
        size_t i = 0;
        while (true) {
            while (i < path.size() && path[i] == ' ') ++i;
            size_t start = i;
            while (i < path.size() && path[i] != ' ') ++i;
            string tok = path.substr(start, i - start);
            if (!tok.empty() && tok[0] != '-') {
                path = path.substr(start); // 从这里开始是路径
                break;
            }
            if (i >= path.size()) {
                path.clear();
                break;
            }
        }
    }

    // 不带参数时列出当前目录（cwd_）；带参数（含 "/"）时按参数解析。
    auto real = vfs_.resolve(cwd_, path);
    if (!real) { send_reply(550, "Invalid path"); return; }
    error_code ec;
    string payload;
    if (fs::is_directory(*real, ec) && !ec) {
        payload = vfs_.list_dir(*real, with_details);
    } else if (fs::exists(*real, ec) && !ec) {
        // 列表对象是单个文件：NLST 只给文件名，LIST 给详情
        string disp = path;
        size_t slash = disp.rfind('/');
        if (slash != string::npos) disp = disp.substr(slash + 1);
        payload = vfs_.list_file(*real, disp, with_details);
    } else {
        send_reply(550, "No such file or directory");
        return;
    }

    shared_ptr<tcp::socket> data;
    if (!open_data_conn(data)) { send_reply(425, "Can't open data connection"); return; }
    bool ok = send_all(*data, payload);
    send_reply(ok ? 226 : 426,
               ok ? "Transfer complete" : "Connection closed; transfer aborted");
}

void Session::run() {
    error_code ec;
    control_->non_blocking(true, ec);
    auto remote = control_->remote_endpoint(ec);
    string peer = "?";
    if (!ec) {
        peer = remote.address().to_string() + ":" + to_string(remote.port());
        peer_addr_ = remote.address(); // 供 PORT/EPRT/PASV 来源校验使用
    }
    last_activity_ = chrono::steady_clock::now();

    log_info("Connection: " + peer);
    send_reply(220, "ftp-server ready");

    string line;
    while (!closing_.load() && read_line(line)) {
        handle_command(line);
        // 命令可能携带长时间的文件传输，传输本身也是活动；
        // 否则传完一个大文件后会立刻被空闲超时误判踢掉
        last_activity_ = chrono::steady_clock::now();
        if (want_quit_) break;
    }

    log_info("Disconnected: " + peer);
    server_.remove_session(this);
}

} // namespace ftp
