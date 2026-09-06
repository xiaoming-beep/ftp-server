// vfs.h - 虚拟文件系统：把 FTP 的 / 风格路径映射到根目录之下，并阻止任何越界访问
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ftp {

class Vfs {
public:
    // root 必须是已 canonical 化的绝对路径（启动时校验过存在）
    explicit Vfs(std::filesystem::path root);
    const std::filesystem::path& root() const { return root_; }

    // 把 FTP 路径（相对当前虚拟 cwd）解析为根目录下的真实路径。
    // 越界（.. 越过根目录）、盘符、UNC、非法字符一律返回 std::nullopt
    std::optional<std::filesystem::path> resolve(const std::string& cwd,
                                                 const std::string& ftp_path) const;

    // 真实路径 -> FTP 虚拟路径（以 / 开头，用于 PWD / MKD 应答）
    std::string virtualize(const std::filesystem::path& real) const;

    // 生成目录列表内容（每行以 \r\n 结尾）：with_details=true 为 unix 风格 LIST，
    // false 为仅名字的 NLST
    std::string list_dir(const std::filesystem::path& real, bool with_details) const;
    std::string list_file(const std::filesystem::path& real, const std::string& name,
                          bool with_details) const;

private:
    std::string format_line(const std::filesystem::directory_entry& e,
                            const std::string& name, bool with_details) const;

    std::filesystem::path root_;
};

} // namespace ftp
