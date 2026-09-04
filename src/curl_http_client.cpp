#include "github_research/curl_http_client.hpp"

#include <curl/curl.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <mutex>

namespace github_research {

// 静态成员定义
std::once_flag CurlHttpClient::global_init_flag_;
bool CurlHttpClient::global_init_done_ = false;

namespace {

// libcurl 写回调:追加到 std::string
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// libcurl 头回调:解析响应头到 map(小写键)
size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    size_t total = size * nitems;
    std::string line(buffer, total);
    // 去除尾随 \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.empty()) return total;
    auto colon = line.find(':');
    if (colon == std::string::npos) return total;
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    // 去除值前导空格
    size_t start = val.find_first_not_of(" \t");
    if (start != std::string::npos) val.erase(0, start);
    // 键转小写
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    (*headers)[key] = val;
    return total;
}

// 获取 exe 同目录下的 curl-ca-bundle.crt 路径(运行时由 CMake 复制)
std::string get_ca_bundle_path() {
    wchar_t exe_path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) return "";
    std::wstring ws(exe_path);
    size_t pos = ws.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return "";
    std::wstring dir = ws.substr(0, pos + 1);
    dir += L"curl-ca-bundle.crt";
    // 宽字符转 UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1,
                        &out[0], len, nullptr, nullptr);
    return out;
}

} // namespace

CurlHttpClient::CurlHttpClient(const std::string& user_agent, int timeout_seconds)
    : user_agent_(user_agent), timeout_seconds_(timeout_seconds) {
    // 自动从环境变量读代理(优先级: HTTPS_PROXY > HTTP_PROXY > ALL_PROXY)
    // 这样所有 CurlHttpClient 实例(包括 web_search、GitHub API 等)
    // 都能直接拿到与 WebView2 相同的代理配置
    auto read_env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? std::string(v) : "";
    };
    std::string proxy = read_env("HTTPS_PROXY");
    if (proxy.empty()) proxy = read_env("https_proxy");
    if (proxy.empty()) proxy = read_env("HTTP_PROXY");
    if (proxy.empty()) proxy = read_env("http_proxy");
    if (proxy.empty()) proxy = read_env("ALL_PROXY");
    if (proxy.empty()) proxy = read_env("all_proxy");
    if (!proxy.empty()) {
        // 去掉可能的 "http://" 前缀(libcurl 自动处理协议)
        proxy_url_ = proxy;
    }

    // 进程级 curl 全局初始化(线程安全,仅一次)
    std::call_once(global_init_flag_, []() {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        global_init_done_ = (rc == CURLE_OK);
        if (!global_init_done_) {
            std::cerr << "[curl] global_init failed: " << curl_easy_strerror(rc) << std::endl;
        }
    });
}

CurlHttpClient::~CurlHttpClient() {
    // 不在此调用 curl_global_cleanup:进程内可能有其他 curl 实例,
    // 全局清理应由进程退出时由 CRT 完成。libcurl 文档建议仅初始化一次,
    // 不显式清理(操作系统会回收资源)。
}

bool CurlHttpClient::initialize() {
    if (ready_) return true;
    if (!global_init_done_) {
        std::cerr << "[curl] ERROR: curl_global_init not done" << std::endl;
        return false;
    }
    // 探测一次 easy handle 是否可用
    CURL* probe = curl_easy_init();
    if (!probe) {
        std::cerr << "[curl] ERROR: curl_easy_init failed" << std::endl;
        return false;
    }
    curl_easy_cleanup(probe);
    ready_ = true;
    std::cerr << "[curl] libcurl backend ready (" << curl_version() << ")" << std::endl;
    return true;
}

HttpResponse CurlHttpClient::get(const std::string& url,
                                  const std::map<std::string, std::string>& headers) {
    HttpResponse resp;
    if (!ready_) {
        std::cerr << "[curl] ERROR: backend not initialized" << std::endl;
        return resp;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[curl] ERROR: curl_easy_init failed in get()" << std::endl;
        return resp;
    }

    // 构建请求头 slist
    struct curl_slist* hdr_slist = nullptr;
    for (const auto& kv : headers) {
        std::string h = kv.first + ": " + kv.second;
        hdr_slist = curl_slist_append(hdr_slist, h.c_str());
    }

    // 设置选项
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds_));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    if (!user_agent_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
    }
    if (hdr_slist) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_slist);
    }
    // 代理
    if (!proxy_url_.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url_.c_str());
    }
    // CA 证书:优先 exe 同目录的 curl-ca-bundle.crt,失败回退系统证书
    std::string ca_path = get_ca_bundle_path();
    if (!ca_path.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path.c_str());
    }
    // 启用 TCP keepalive,避免长连接挂死
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        std::cerr << "[curl] GET failed: " << curl_easy_strerror(rc)
                  << " (url=" << url << ")" << std::endl;
        // 仍返回,调用方通过 status_code=0 判断失败
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status_code = static_cast<int>(code);
    }

    if (hdr_slist) curl_slist_free_all(hdr_slist);
    curl_easy_cleanup(curl);
    return resp;
}

} // namespace github_research
