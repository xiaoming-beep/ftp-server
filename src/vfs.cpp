#include "vfs.h"

#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace ftp {

using namespace std;
namespace fs = filesystem;

Vfs::Vfs(fs::path root) : root_(std::move(root)) {}

optional<fs::path> Vfs::resolve(const string& cwd, const string& ftp_path) const {
    if (ftp_path.find('\0') != string::npos) return nullopt;

    // 0. 去掉 RFC 959 路径名两端的双引号，并把内部成对的 "" 还原为单个 "。
    //    Windows 资源管理器等客户端在 CWD/LIST/MKD/RMD/DELE/RETR/STOR 等命令中
    //    会把路径参数用双引号包裹（如 "/"、"/src"），不剥离会把它当成文件名的一部分，
    //    导致全部解析失败（列目录报 550、删除报 550 No such directory）。
    string unquoted = ftp_path;
    if (unquoted.size() >= 2 && unquoted.front() == '"' && unquoted.back() == '"') {
        string inner = unquoted.substr(1, unquoted.size() - 2);
        unquoted.clear();
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i] == '"' && i + 1 < inner.size() && inner[i + 1] == '"') {
                unquoted.push_back('"'); // 成对引号还原为单个字面引号
                ++i;
            } else {
                unquoted.push_back(inner[i]);
            }
        }
    }

    // 1. 反斜杠当正斜杠，并拒绝盘符（C:）与 UNC（\\server 或 //server）
    string p = unquoted;
    for (auto& c : p)
        if (c == '\\') c = '/';
    if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':')
        return nullopt;
    if (p.rfind("//", 0) == 0) return nullopt;

    // 2. 组装虚拟路径：以 / 开头视为绝对，否则基于当前 cwd
    string vpath;
    if (!p.empty() && p[0] == '/')
        vpath = p;
    else if (cwd.empty() || cwd == "/")
        vpath = "/" + p;
    else
        vpath = cwd + "/" + p;

    // 3. 按分量处理 . 与 ..；.. 越过根目录即拒绝
    vector<string> parts;
    istringstream iss(vpath);
    string part;
    while (getline(iss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return nullopt;
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }

    // 4. 拼接到根目录下
    fs::path rel;
    for (auto& part : parts) rel /= utf8_to_path(part);
    fs::path real = root_ / rel;

    // 5. 规范化并二次确认仍在根目录内（防止符号链接/联接逃逸）。
    //    weakly_canonical 对不存在的路径也会尽量规范化，仅在权限等错误时失败
    error_code ec;
    fs::path canon = fs::weakly_canonical(real, ec);
    if (ec) return nullopt;
    if (!path_starts_with(canon, root_)) return nullopt;
    return canon;
}

string Vfs::virtualize(const fs::path& real) const {
    string rel = path_to_utf8(real.lexically_relative(root_));
    // lexically_relative 对相同路径（根目录）返回 "." 而非空串
    if (rel.empty() || rel == ".") return "/";
    string v;
    v.reserve(rel.size() + 1);
    v.push_back('/');
    for (auto& c : rel)
        v.push_back(c == '\\' ? '/' : c);
    return v;
}

string Vfs::list_dir(const fs::path& real, bool with_details) const {
    error_code ec;
    fs::directory_iterator it(real, ec), end;
    if (ec) return "";

    vector<fs::directory_entry> entries;
    for (auto& e : it) entries.push_back(e);
    sort(entries.begin(), entries.end(),
         [](const fs::directory_entry& a, const fs::directory_entry& b) {
             return a.path().filename().native() < b.path().filename().native();
         });

    string out;
    for (auto& e : entries) {
        string name = path_to_utf8(e.path().filename());
        out += format_line(e, name, with_details);
    }
    return out;
}

string Vfs::list_file(const fs::path& real, const string& name,
                      bool with_details) const {
    error_code ec;
    fs::directory_entry e(real, ec);
    if (ec) return "";
    string n = name.empty() ? path_to_utf8(real.filename()) : name;
    return format_line(e, n, with_details);
}

string Vfs::format_line(const fs::directory_entry& e, const string& name,
                        bool with_details) const {
    string safe = sanitize_name(name);
    if (!with_details) return safe + "\r\n";

    error_code ec;
    bool is_dir = e.is_directory(ec);
    const char* perm = is_dir ? "drwxr-xr-x" : "-rw-r--r--";
    uintmax_t size = 0;
    if (!is_dir) size = e.file_size(ec);
    auto ft = e.last_write_time(ec);
    string date = ec ? "Jan 01 00:00" : list_date_str(file_time_to_time_t(ft));
    char buf[512];
    snprintf(buf, sizeof buf, "%s   1 owner group %12llu %s %s\r\n", perm,
             (unsigned long long)size, date.c_str(), safe.c_str());
    return buf;
}

} // namespace ftp
