// util.h - 通用工具：日志、字符串、时间格式化、UTF-8 路径转换
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ftp {

// Console log with timestamp
void log_info(const std::string& msg);
void log_error(const std::string& msg);

// 去掉首尾空白
std::string trim(const std::string& s);
// 全部转大写
std::string to_upper(const std::string& s);

// 当前本地时间字符串 YYYY-MM-DD HH:MM:SS
std::string now_str();

// 文件时间 -> FTP MDTM 应答格式 YYYYMMDDHHMMSS
std::string mdtm_str(std::int64_t mtime);
// 文件时间 -> unix 风格列表日期 "Jun 30 10:00" / "Jun 30  2024"
std::string list_date_str(std::int64_t mtime);
// 文件时间 -> time_t（跨时钟换算，兼容 MinGW 等没有 file_clock::to_time_t 的实现）
std::time_t file_time_to_time_t(const std::filesystem::file_time_type& ft);

// UTF-8 字符串 <-> 文件系统路径（Windows 上处理宽字符转换）
std::filesystem::path utf8_to_path(const std::string& u8);
std::string path_to_utf8(const std::filesystem::path& p);

// 判断 path 是否以 root 为前缀（Windows 下大小写不敏感，且检查目录边界）
bool path_starts_with(const std::filesystem::path& p,
                      const std::filesystem::path& root);

// 把文件名中的控制字符替换为 '?'，防止伪造目录列表
std::string sanitize_name(const std::string& name);

} // namespace ftp
