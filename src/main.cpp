// main.cpp - 入口：命令行解析、参数校验、启动、Ctrl+C 优雅退出
#include "server.h"
#include "util.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace ftp;

std::atomic<Server*> g_server{nullptr};

void request_stop() {
    if (auto* s = g_server.load()) s->request_stop();
}

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD /*type*/) {
    request_stop();
    return TRUE;
}
#else
void on_signal(int /*sig*/) { request_stop(); }
#endif

void usage() {
    std::printf(
        "Usage: ftp-server [options]\n"
        "  -path <dir>      FTP root directory (default: current directory)\n"
        "  -port <port>     Listen port (default: 21)\n"
        "  -ro              Read-only mode (disable upload/delete/rename etc.)\n"
        "  -h, --help       Show this help\n"
        "\n"
        "Examples:\n"
        "  ftp-server -path ./ -port 3721\n"
        "  ftp-server -path ./ -port 3721 -ro\n");
}

} // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;

    fs::path root = fs::current_path();
    unsigned long port = 21;
    bool read_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help" || a == "-help") {
            usage();
            return 0;
        }
        if (a == "-path" || a == "--path") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: -path missing directory argument\n");
                usage();
                return 2;
            }
            root = fs::path(argv[++i]);
        } else if (a == "-port" || a == "--port") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: -port missing port argument\n");
                usage();
                return 2;
            }
            char* end = nullptr;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v == 0 || v > 65535) {
                std::fprintf(stderr, "Error: invalid port \"%s\" (range 1-65535)\n", argv[i]);
                usage();
                return 2;
            }
            port = v;
        } else if (a == "-ro" || a == "--ro") {
            read_only = true;
        } else {
            std::fprintf(stderr, "Error: unknown argument \"%s\"\n", a.c_str());
            usage();
            return 2;
        }
    }

    // 根目录必须存在且为目录（canonical 同时解析相对路径并规范化）
    std::error_code ec;
    fs::path abs_root = fs::canonical(root, ec);
    if (ec || !fs::is_directory(abs_root)) {
        std::fprintf(stderr, "Error: root directory does not exist or is not a directory: %s\n",
                     ftp::path_to_utf8(root).c_str());
        return 1;
    }

    Config cfg{abs_root, static_cast<unsigned short>(port), read_only};
    Server server(cfg);
    g_server.store(&server);

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#endif

    int rc = server.run();
    g_server.store(nullptr);
    return rc;
}
