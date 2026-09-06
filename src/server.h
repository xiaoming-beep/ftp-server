// server.h - FTP 服务器：监听、accept 循环、会话管理、优雅关闭
#pragma once

#include "vfs.h"

#include <asio.hpp>

#include <atomic>
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
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<std::shared_ptr<Session>> sessions_;
};

} // namespace ftp
