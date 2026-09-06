// session.h - FTP 会话：控制连接命令循环、数据连接与文件传输
#pragma once

#include "server.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ftp {

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(Server& server, std::shared_ptr<asio::ip::tcp::socket> control);

    // 会话主循环（在独立线程中调用）
    void run();
    // 线程安全：通知本会话尽快退出
    void close();

private:
    // 控制连接
    bool read_line(std::string& out);
    bool send_raw(const std::string& data);
    bool send_reply(int code, const std::string& text);
    bool send_multiline(int code, const std::string& first,
                        const std::vector<std::string>& lines,
                        const std::string& last);
    void handle_command(const std::string& line);

    // 数据连接
    // 异步等待数据连接就绪的共享状态。回调通过 shared_ptr 持有它，
    // 即使回调在函数返回后才被投递，也不会触碰已销毁的栈变量。
    struct DataWait {
        std::shared_ptr<asio::ip::tcp::socket> sock;
        std::shared_ptr<asio::ip::tcp::acceptor> acc;
        std::unique_ptr<asio::steady_timer> timer;
        bool done = false;
        bool ok = false;
    };
    bool open_data_conn(std::shared_ptr<asio::ip::tcp::socket>& out);
    void arm_pasv_accept(std::shared_ptr<DataWait> st);
    bool send_all(asio::ip::tcp::socket& s, const std::string& data);
    bool send_all(asio::ip::tcp::socket& s, const char* data, std::size_t len);
    bool send_file(asio::ip::tcp::socket& data, const std::filesystem::path& real);
    bool recv_file(asio::ip::tcp::socket& data, const std::filesystem::path& real,
                   bool append);

    // 命令实现细节
    void do_cwd(const std::string& path);
    void do_pasv(bool extended);
    void do_port(const std::string& arg);
    void do_eprt(const std::string& arg);
    void do_list(bool with_details, const std::string& arg);

    Server& server_;
    Vfs& vfs_;
    asio::io_context ioc_;
    std::shared_ptr<asio::ip::tcp::socket> control_;
    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::seconds stall_timeout_; // 数据连接停滞超时（无字节进展即断开）
    asio::ip::address peer_addr_;        // 控制连接对端地址（PORT/PASV 来源校验用）
    std::string recv_buf_;  // 控制连接的行缓冲

    std::string cwd_ = "/";
    std::string user_;
    bool logged_in_ = false;
    bool want_quit_ = false;
    char type_ = 'A'; // 'A' ASCII / 'I' 二进制
    std::uint64_t rest_ = 0;
    std::optional<std::string> rnfr_; // RNFR 暂存的虚拟路径
    std::shared_ptr<asio::ip::tcp::acceptor> pasv_;
    std::optional<asio::ip::tcp::endpoint> port_ep_; // PORT/EPRT 目标
    std::atomic<bool> closing_{false};
};

} // namespace ftp
