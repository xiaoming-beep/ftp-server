#include "server.h"

#include "session.h"
#include "util.h"

#include <chrono>
#include <thread>

namespace ftp {

using namespace std;

Server::Server(Config cfg) : cfg_(std::move(cfg)), vfs_(cfg_.root), acceptor_(ioc_) {}

void Server::add_session(shared_ptr<Session> s) {
    lock_guard<mutex> lk(mu_);
    sessions_.push_back(std::move(s));
}

void Server::remove_session(const Session* s) {
    lock_guard<mutex> lk(mu_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->get() == s) {
            sessions_.erase(it);
            break;
        }
    }
}

void Server::request_stop() {
    if (stop_.exchange(true)) return;
    ioc_.stop(); // 线程安全：让 ioc_.run() 返回
    lock_guard<mutex> lk(mu_);
    for (auto& s : sessions_) s->close();
}

void Server::do_accept() {
    acceptor_.async_accept([this](error_code ec, asio::ip::tcp::socket sock) {
        if (!ec) {
            auto s = make_shared<Session>(*this,
                                          make_shared<asio::ip::tcp::socket>(std::move(sock)));
            add_session(s);
            std::thread([s] { s->run(); }).detach();
            do_accept();
        } else if (ec != asio::error::operation_aborted) {
            log_error("accept failed: " + ec.message());
            do_accept();
        }
    });
}

int Server::run() {
    error_code ec;
    asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), cfg_.port);
    acceptor_.open(ep.protocol(), ec);
    if (!ec) acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec) acceptor_.bind(ep, ec);
    if (ec) {
        log_error("bind port " + to_string(cfg_.port) + " failed: " + ec.message() +
                  " (port may be in use)");
        return 1;
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        log_error("listen failed: " + ec.message());
        return 1;
    }

    log_info("FTP server started: root = " + path_to_utf8(cfg_.root) +
             ", port = " + to_string(cfg_.port) +
             ", mode = " + (cfg_.read_only ? "read-only (-ro)" : "read-write"));
    log_info("Waiting for connections...");

    do_accept();
    ioc_.run(); // 阻塞直到 request_stop() 调用 ioc_.stop()

    acceptor_.close(ec);
    {
        lock_guard<mutex> lk(mu_);
        for (auto& s : sessions_) s->close();
    }
    log_info("Shutting down, waiting for sessions to exit...");
    wait_sessions();
    log_info("Server exited");
    return 0;
}

void Server::wait_sessions() {
    auto deadline = chrono::steady_clock::now() + chrono::seconds(40);
    while (true) {
        {
            lock_guard<mutex> lk(mu_);
            if (sessions_.empty()) return;
        }
        if (chrono::steady_clock::now() >= deadline) {
            log_error("Some sessions did not exit within timeout");
            return;
        }
        this_thread::sleep_for(chrono::milliseconds(200));
    }
}

} // namespace ftp
