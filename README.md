# ftp-server — 简单的跨平台 FTP 服务器（C++17）

用 C++17 重写的简单 FTP 文件服务器，复刻原 Java 版工具的命令行风格：

```
ftp-server -path ./ -port 3721        # 默认可读写
ftp-server -path ./ -port 3721 -ro    # 只读模式
```

- 登录不校验：任意用户名/密码均可登录（面向内网/本机简单使用）
- 支持标准常用 FTP 命令集，curl、Windows 资源管理器、FileZilla 均可连接
- 根目录锁定：`..` 越界、盘符、UNC、符号链接逃逸一律拒绝（550）
- 路径编码 UTF-8（RFC 2640），Windows 下中文文件名正常
- 跨平台：Windows / Linux / macOS（网络层使用 standalone Asio，仅头文件依赖）

## 构建

### 依赖

- C++17 编译器（GCC / MSVC / Clang）
- CMake ≥ 3.16（可选，也可直接 g++ 编译）
- Asio 头文件：已随项目提供（`third_party/asio`），无需联网

### Windows（MinGW）

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

生成 `build\ftp-server.exe`。CLion 用户直接打开本目录即可（自带工具链）。

### Windows（MSVC）

用 VS2022 打开文件夹，或：

```bat
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

### Linux / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

> 注：Linux 上绑定 1024 以下端口需要 root 权限；Windows 上无此限制（默认端口 21 可直接用）。

### 不用 CMake 直接编译

```bash
g++ -std=c++17 -Isrc -Ithird_party/asio/include src/*.cpp -o ftp-server -lws2_32 -lmswsock   # Windows
g++ -std=c++17 -Isrc -Ithird_party/asio/include src/*.cpp -o ftp-server -lpthread             # Linux/macOS
```

## 用法

```
用法: ftp-server [选项]
  -path <目录>   FTP 根目录（默认: 当前目录）
  -port <端口>   监听端口（默认: 21）
  -ro            只读模式（禁止上传/删除/改名等写操作）
  -h, -help      显示本帮助
```

示例：

```bash
# 把 D:\share 作为根目录，端口 3721，可读写
ftp-server -path D:\share -port 3721

# 只读共享
ftp-server -path ./ -port 3721 -ro
```

连接方式：

- 命令行：`curl ftp://127.0.0.1:3721/`（`curl -T 文件` 上传、`curl -o 文件` 下载、`curl -C -` 断点续传）
- Windows 资源管理器地址栏：`ftp://127.0.0.1:3721/`
- FileZilla 等任意 FTP 客户端

按 `Ctrl+C` 优雅退出（关闭监听、断开全部会话、等待线程结束后退出）。

## 支持的 FTP 命令

| 类别 | 命令 |
|------|------|
| 会话 | `USER` `PASS` `QUIT` `NOOP` `SYST` `FEAT` `OPTS UTF8` `HELP` |
| 传输模式 | `TYPE A/I` `STRU F` `MODE S` `PASV` `EPSV` `PORT` `EPRT` `REST` `ABOR` |
| 导航 | `PWD` `CWD` `CDUP`（含 `XPWD` `XCWD` `XCUP` 兼容名） |
| 目录 | `MKD` `RMD`（含 `XMKD` `XRMD`） |
| 文件 | `LIST` `NLST` `RETR` `STOR` `APPE` `DELE` `RNFR` `RNTO` `SIZE` `MDTM` |

- `-ro` 只读模式下，写命令（STOR/APPE/DELE/MKD/RMD/RNFR/RNTO）统一返回 `550`
- `TYPE A` 会做换行转换（LF ↔ CRLF），`TYPE I` 为原样二进制
- 空闲 10 分钟无命令自动断开；PASV 等待数据连接 30 秒超时

## 安全说明

- **无认证**：任何账号密码都能登录，请仅在可信网络/本机使用
- **根目录隔离**：所有路径解析后都会规范化并校验仍在根目录内，`..` 越界、绝对路径逃逸、符号链接逃逸均被拒绝
- 不做 FTPS/TLS 加密（与"简单工具"定位一致）

## 测试

```bash
python test/test_ftp.py            # 冒烟测试（自动启动/停止服务器）
```

覆盖：登录、全部命令、PASV/PORT 两种数据模式、断点续传、中文文件名、
-`ro` 只读拒绝、路径越界防护、并发下载（59 项断言）。

另有 `test/test_shutdown.cpp` 验证优雅退出与端口占用处理：

```bash
g++ -std=c++17 -Isrc -Ithird_party/asio/include test/test_shutdown.cpp \
    src/server.cpp src/session.cpp src/vfs.cpp src/util.cpp \
    -o build/test_shutdown.exe -lws2_32 -lmswsock && ./build/test_shutdown.exe
```

## 目录结构

```
├── CMakeLists.txt        CMake 构建脚本
├── README.md
├── third_party/asio      依赖：Asio 1.30.2 头文件（离线可用）
├── src/
│   ├── main.cpp          CLI 解析、参数校验、启动、Ctrl+C 退出
│   ├── server.h/.cpp     监听、accept 循环、会话管理、优雅关闭
│   ├── session.h/.cpp    每连接一个线程：命令循环、数据连接、文件传输
│   ├── vfs.h/.cpp        根目录锁定、防路径穿越、目录列表生成
│   └── util.h/.cpp       日志、时间格式化、UTF-8 转换
└── test/
    ├── test_ftp.py       冒烟测试（python ftplib）
    └── test_shutdown.cpp 优雅退出验证
```

## 常见问题

- **启动提示端口被占用**：换端口，或先结束占用进程
- **Linux 上 21 端口绑定失败**：`sudo` 运行或换高位端口
- **防火墙拦截**：放行监听端口；主动模式（PORT）还需要放行出站连接
- **mintty（Git Bash）里 Ctrl+C 可能无法优雅退出**：这是无控制台环境的限制，在
  cmd/PowerShell 中运行即可正常触发优雅退出
