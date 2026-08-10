#include "github_research/string_utils.hpp"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <set>

namespace github_research {

std::wstring to_wstring(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                   static_cast<int>(utf8.size()),
                                   nullptr, 0);
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        wide.data(), wlen);
    return wide;
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                   static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()),
                        utf8.data(), ulen, nullptr, nullptr);
    return utf8;
}

std::string url_encode(const std::string& value) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            oss << static_cast<char>(c);
        } else {
            oss << '%' << std::setw(2) << std::setfill('0')
                << static_cast<int>(c);
        }
    }
    return oss.str();
}

std::string to_lower(const std::string& s) {
    std::string result = s;
    for (auto& c : result) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return result;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string build_query(const std::map<std::string, std::string>& params) {
    std::string q;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) q += '&';
        q += url_encode(k) + '=' + url_encode(v);
        first = false;
    }
    return q;
}

std::string base64_decode(const std::string& encoded) {
    static const int decode_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int d = decode_table[c];
        if (d == -1) break;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ============================================================
// URL 提取 / 归一化 / 过滤
// ============================================================

std::vector<std::string> extract_urls(const std::string& text) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    if (text.empty()) return result;

    // 基于常见边界字符的简化扫描(不依赖 <regex>,避免 MSVC STL 栈开销)
    // URL 起始签名: http:// 或 https:// 或 ftp://
    const char* patterns[] = { "https://", "http://", "ftp://" };
    size_t plen[] = { 8, 7, 6 };
    const size_t n = text.size();

    for (size_t i = 0; i < n; ) {
        size_t match_idx = (size_t)-1;
        for (size_t p = 0; p < 3; ++p) {
            if (i + plen[p] <= n &&
                ::memcmp(text.data() + i, patterns[p], plen[p]) == 0) {
                match_idx = p;
                break;
            }
        }
        if (match_idx == (size_t)-1) {
            ++i;
            continue;
        }
        // 向右找 URL 结束位置
        size_t start = i;
        size_t j = i + plen[match_idx];
        // URL 允许字符:字母数字 + !*'();:@&=+$,/?#[]-_.~%
        auto is_url_char = [](unsigned char c) -> bool {
            if (c >= 'a' && c <= 'z') return true;
            if (c >= 'A' && c <= 'Z') return true;
            if (c >= '0' && c <= '9') return true;
            const char* allowed = "!*'();:@&=+$,/?#[]-_.~%";
            for (const char* p = allowed; *p; ++p) if (*p == c) return true;
            return false;
        };
        while (j < n && is_url_char(static_cast<unsigned char>(text[j]))) ++j;
        // 清除尾部标点(.,;) —— 它们很可能是句子标点而非 URL 一部分
        while (j > start + plen[match_idx]) {
            char c = text[j - 1];
            if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
                --j;
            } else {
                break;
            }
        }
        if (j > start + plen[match_idx] + 3) {   // 至少有 host
            std::string url = text.substr(start, j - start);
            if (seen.insert(url).second) {
                result.push_back(url);
            }
        }
        i = j;
    }
    return result;
}

std::string normalize_url(const std::string& url) {
    if (url.empty()) return url;
    // 1. 去 fragment (# 之后)
    std::string s = url;
    size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    if (s.empty()) return s;
    // 2. 小写 scheme + host (path 保留大小写)
    size_t scheme_end = s.find("://");
    if (scheme_end == std::string::npos) return s;
    std::string scheme = s.substr(0, scheme_end);
    for (auto& c : scheme) c = (char)::tolower((unsigned char)c);
    std::string rest = s.substr(scheme_end + 3);
    size_t slash = rest.find('/');
    std::string host, path;
    if (slash == std::string::npos) {
        host = rest;
        path = "";
    } else {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    for (auto& c : host) c = (char)::tolower((unsigned char)c);
    // 去 host 尾点(有时出现多余点)
    while (!host.empty() && host.back() == '.') host.pop_back();
    // 去 path 尾斜杠(只去一个,避免把根路径清掉)
    if (path.size() >= 2 && path.back() == '/') path.pop_back();
    if (path.empty()) path = "/";
    return scheme + "://" + host + path;
}

bool is_low_value_url(const std::string& url) {
    if (url.empty()) return true;
    std::string nu = normalize_url(url);
    std::string lower = to_lower(nu);

    // HN 站内(外部正文已经单独抓了,不要重复)
    if (lower.find("news.ycombinator.com") != std::string::npos) return true;

    // 登录 / 认证 / 注册 页
    static const char* auth_tokens[] = {
        "/login", "/signin", "/auth", "/signup", "/register",
        "/oauth", "/sso", "/account/login", "/user/sign_in"
    };
    for (auto t : auth_tokens) {
        if (lower.find(t) != std::string::npos) return true;
    }

    // 常见低价值/无正文资源
    static const char* low_ext[] = {
        ".pdf", ".zip", ".tar", ".gz", ".rar", ".7z",
        ".jpg", ".jpeg", ".png", ".gif", ".mp4", ".mp3",
        ".avi", ".mov", ".svg", ".exe", ".dmg", ".apk",
        ".iso", ".bin", ".css", ".js", ".woff", ".ttf"
    };
    for (auto e : low_ext) {
        if (lower.size() >= std::strlen(e) &&
            lower.compare(lower.size() - std::strlen(e), std::strlen(e), e) == 0) {
            return true;
        }
    }

    // 广告 / 追踪 域名前缀
    static const char* ad_domains[] = {
        "ads.", "track.", "analytics.", "pixel.", "doubleclick.net",
        "googletagmanager", "googlesyndication", "facebook.com/tr",
        "twitter.com/i/redirect", "t.co/", "bit.ly/", "tinyurl.com/",
        "lnkd.in/", "buff.ly/", "amzn.to/", "a.co/", "fb.me/",
        "news.google.com/__i/rss", "feedproxy.google.com"
    };
    for (auto d : ad_domains) {
        if (lower.find(d) != std::string::npos) return true;
    }

    // 空路径
    // (http://example.com 不算低价值,排除)
    return false;
}

} // namespace github_research
