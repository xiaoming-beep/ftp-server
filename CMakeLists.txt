cmake_minimum_required(VERSION 3.16)
project(ftp-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Asio 依赖：优先使用项目内 vendored 头文件（离线可用）；
# 没有则退化为从 GitHub 下载（需要网络）。
find_path(ASIO_INCLUDE_DIR asio.hpp
  PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
  NO_DEFAULT_PATH)
if(NOT ASIO_INCLUDE_DIR)
  message(STATUS "未找到 vendored Asio，尝试从网络下载 asio-1-30-2 ...")
  include(FetchContent)
  FetchContent_Declare(asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz)
  FetchContent_MakeAvailable(asio)
  set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

add_executable(ftp-server
  src/main.cpp
  src/server.cpp
  src/session.cpp
  src/vfs.cpp
  src/util.cpp
)

target_include_directories(ftp-server PRIVATE src ${ASIO_INCLUDE_DIR})

if(WIN32)
  # standalone Asio 在 Windows 上依赖 Winsock；MinGW 下需要明确 Win32 API 版本
  target_compile_definitions(ftp-server PRIVATE _WIN32_WINNT=0x0601)
  # mswsock 提供 AcceptEx 等扩展，asio 的 async_accept 需要
  target_link_libraries(ftp-server ws2_32 mswsock)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(ftp-server Threads::Threads)
endif()
