// test_shutdown.cpp - 验证 Server::run() / request_stop() 的优雅退出路径
// （Ctrl+C 控制台回调调用的正是 request_stop()，此测试直接验证该路径）
//
// 编译（不加入主构建）:
//   g++ -std=c++17 -Isrc -Ithird_party/asio/include test/test_shutdown.cpp \
//       src/server.cpp src/session.cpp src/vfs.cpp src/util.cpp \
//       -o build/test_shutdown.exe -lws2_32 -lmswsock
#include "server.h"
#include "util.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>

int main() {
    namespace fs = std::filesystem;
    using namespace ftp;

    // 场景 1：正常启动后 request_stop()，应优雅退出且返回 0
    {
        fs::path root = fs::canonical(fs::current_path());
        Config cfg{root, 3730, false};
        Server server(cfg);
        std::atomic<int> rc{999};
        std::thread t([&] { rc = server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        server.request_stop();
        t.join();
        std::printf("场景1 (正常退出): run() 返回 %d\n", rc.load());
        if (rc.load() != 0) return 1;
    }

    // 场景 2：端口被占用时应返回 1 而不是崩溃
    {
        asio::io_context ioc;
        asio::ip::tcp::acceptor blocker(ioc);
        blocker.open(asio::ip::tcp::v4());
        blocker.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 3731));
        blocker.listen(1);

        fs::path root = fs::canonical(fs::current_path());
        Config cfg{root, 3731, false};
        Server server(cfg);
        int rc = server.run();
        std::printf("场景2 (端口占用): run() 返回 %d\n", rc);
        if (rc != 1) return 1;
    }

    std::printf("优雅退出测试全部通过\n");
    return 0;
}
