// server.h - FTP 服务器：监听、accept 循环、会话管理、优雅关闭
#pragma once

#include "vfs.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ftp {

struct Config {
    std::filesystem::path root; // 已 canonical 化
    unsigned short port = 21;
    bool read_only = false;
    std::chrono::seconds stall_timeout{60}; // 数据传输停滞超时，超时断开并释放文件
};

class Session;

class Server {
public:
    explicit Server(Config cfg);

    // 绑定监听并进入 accept 循环；阻塞直到 request_stop() 被调用
    int run();
    // 线程安全：通知 accept 循环与所有会话退出
    void request_stop();

    const Config& config() const { return cfg_; }
    Vfs& vfs() { return vfs_; }
    bool stopping() const { return stop_.load(); }

    void add_session(std::shared_ptr<Session> s);
    void remove_session(const Session* s);

private:
    void do_accept();
    void wait_sessions();

    Config cfg_;
    Vfs vfs_;
    asio::io_context ioc_;
    asio::ip::tcp::acceptor acceptor_;
#ifndef _WIN32
    // POSIX 上不能在信号处理函数里加锁/操作 io_context（非 async-signal-safe，
    // 会死锁）；asio::signal_set 把信号转为 io_context 里的普通回调来处理
    asio::signal_set signals_;
#endif
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::shared_ptr<Session>> sessions_;
    int accept_errors_ = 0; // 连续 accept 失败计数，用于退避（如 fd 耗尽时避免热循环）
};

} // namespace ftp
