#include "util.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cwctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ftp {

using namespace std;

void log_info(const string& msg) {
    fprintf(stdout, "[%s] %s\n", now_str().c_str(), msg.c_str());
    fflush(stdout);
}

void log_error(const string& msg) {
    fprintf(stderr, "[%s] [ERROR] %s\n", now_str().c_str(), msg.c_str());
    fflush(stderr);
}

string trim(const string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

string to_upper(const string& s) {
    string r = s;
    transform(r.begin(), r.end(), r.begin(),
              [](unsigned char c) { return (char)toupper(c); });
    return r;
}

string now_str() {
    time_t t = time(nullptr);
    tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

string mdtm_str(int64_t mtime) {
    time_t t = (time_t)mtime;
    tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tmv);
    return buf;
}

string list_date_str(int64_t mtime) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = (time_t)mtime;
    tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    // 半年内显示时分，更早显示年份
    time_t now = time(nullptr);
    bool recent = (now - t) < 183LL * 24 * 3600 && t <= now + 3600;
    char buf[64];
    if (recent) {
        snprintf(buf, sizeof buf, "%s %02d %02d:%02d", months[tmv.tm_mon],
                 tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(buf, sizeof buf, "%s %02d  %04d", months[tmv.tm_mon],
                 tmv.tm_mday, tmv.tm_year + 1900);
    }
    return buf;
}

time_t file_time_to_time_t(const filesystem::file_time_type& ft) {
    // file_clock 与 system_clock 不能直接相减，改用"相对当前时刻"的偏移量换算：
    // 文件时间相对现在的偏移 + 系统时钟的当前时间
    auto since_now = ft - filesystem::file_time_type::clock::now();
    auto sys = chrono::time_point_cast<chrono::system_clock::duration>(
        chrono::system_clock::now() +
        chrono::duration_cast<chrono::system_clock::duration>(since_now));
    return chrono::system_clock::to_time_t(sys);
}

filesystem::path utf8_to_path(const string& u8) {
#ifdef _WIN32
    if (u8.empty()) return filesystem::path();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.c_str(), -1, nullptr, 0);
    if (len <= 0) return filesystem::path();
    wstring wpath(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.c_str(), -1, wpath.data(), len);
    return filesystem::path(wpath);
#else
    return filesystem::path(u8);
#endif
}

string path_to_utf8(const filesystem::path& p) {
#ifdef _WIN32
    wstring wpath = p.wstring();
    if (wpath.empty()) return string();
    int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return string();
    string u8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wpath.c_str(), -1, u8.data(), len, nullptr, nullptr);
    return u8;
#else
    return p.string();
#endif
}

bool path_starts_with(const filesystem::path& p, const filesystem::path& root) {
    auto ps = p.native();
    auto rs = root.native();
    if (rs.size() > ps.size()) return false;
#ifdef _WIN32
    // Windows 路径大小写不敏感
    wstring lp = ps, lr = rs;
    for (auto& c : lp) c = (wchar_t)towlower(c);
    for (auto& c : lr) c = (wchar_t)towlower(c);
    if (lr.size() < lp.size()) {
        wchar_t next = lp[lr.size()];
        if (next != L'\\' && next != L'/') return false; // 根=C:\foo 而路径=C:\foobar
    }
    return lp.compare(0, lr.size(), lr) == 0;
#else
    if (rs.size() < ps.size() && ps[rs.size()] != '/') return false;
    return ps.compare(0, rs.size(), rs) == 0;
#endif
}

string sanitize_name(const string& name) {
    string out = name;
    for (auto& c : out) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20 || u == 0x7f) c = '?';
    }
    return out;
}

} // namespace ftp
