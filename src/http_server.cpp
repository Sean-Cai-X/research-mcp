#include "github_research/http_server.hpp"
#include "github_research/string_utils.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace github_research {

#ifdef _WIN32
std::mutex HttpServer::wsa_init_mutex_;
int HttpServer::wsa_init_count_ = 0;

bool HttpServer::init_wsa() {
    std::lock_guard<std::mutex> lock(wsa_init_mutex_);
    if (wsa_init_count_ > 0) {
        wsa_init_count_++;
        return true;
    }
    WSADATA wsaData;
    int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (err != 0) {
        std::cerr << "[http] WSAStartup failed: " << err << std::endl;
        return false;
    }
    wsa_init_count_ = 1;
    return true;
}

void HttpServer::fini_wsa() {
    std::lock_guard<std::mutex> lock(wsa_init_mutex_);
    if (wsa_init_count_ > 0) {
        wsa_init_count_--;
        if (wsa_init_count_ == 0) {
            WSACleanup();
        }
    }
}
#endif

HttpServer::HttpServer(int port, Handler handler)
    : port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
    stop();
#ifdef _WIN32
    fini_wsa();
#endif
}

void HttpServer::stop() {
    stopped_.store(true);
}

// 解析 HTTP 请求(简化版,假设一次 recv 能拿到完整 header)
static bool parse_http_request(const std::string& raw, HttpRequest& req) {
    // 找到 header/body 分隔
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::string header = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);

    // 解析请求行
    size_t first_space = header.find(' ');
    if (first_space == std::string::npos) return false;
    req.method = header.substr(0, first_space);

    size_t second_space = header.find(' ', first_space + 1);
    if (second_space == std::string::npos) return false;
    req.path = header.substr(first_space + 1, second_space - first_space - 1);

    // 解析 Content-Type / Content-Length
    std::string lower_header = to_lower(header);
    size_t ct_pos = lower_header.find("content-type:");
    if (ct_pos != std::string::npos) {
        size_t eol = header.find("\r\n", ct_pos);
        if (eol == std::string::npos) eol = header.size();
        std::string ct_line = header.substr(ct_pos + 13, eol - ct_pos - 13);
        // 去掉首尾空白
        size_t s = ct_line.find_first_not_of(" \t");
        size_t e = ct_line.find_last_not_of(" \t");
        if (s != std::string::npos) {
            req.content_type = ct_line.substr(s, e - s + 1);
        }
    }

    req.body = body;
    return true;
}

// HTTP 状态码 -> 原因短语
static const char* http_status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

static std::string build_http_response(const HttpServerResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << http_status_reason(resp.status) << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type\r\n";
    // 附加自定义 header(如 Allow: POST)
    if (!resp.extra_headers.empty()) {
        oss << resp.extra_headers;
    }
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

int HttpServer::run() {
#ifdef _WIN32
    if (!init_wsa()) return 1;
#endif

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "[http] socket() failed" << std::endl;
        return 2;
    }

    // 允许地址重用,避免 TIME_WAIT 卡住
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port_));

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[http] bind() failed on port " << port_ << std::endl;
        closesocket(listen_sock);
        return 3;
    }

    if (listen(listen_sock, 5) == SOCKET_ERROR) {
        std::cerr << "[http] listen() failed" << std::endl;
        closesocket(listen_sock);
        return 4;
    }

    std::cerr << "[http] MCP server listening on http://127.0.0.1:" << port_ << "/mcp"
              << " (Ctrl+C to stop)" << std::endl;

    while (!stopped_.load()) {
        // accept 带 select 超时,以便及时响应 stop()
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_sock, &fds);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(listen_sock) + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            // 超时或被中断,继续循环检查 stopped_
            continue;
        }

        sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);
        SOCKET client = accept(listen_sock,
                               reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client == INVALID_SOCKET) {
            continue;
        }

        // 读取请求数据(最多 1MB)
        std::vector<char> buf(65536, 0);
        std::string raw;
        while (true) {
            int n = recv(client, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) break;
            raw.append(buf.data(), n);
            if (raw.size() > 1024 * 1024) break;  // 1MB 上限
            // 收到完整 header+body 后退出
            if (raw.find("\r\n\r\n") != std::string::npos) {
                // 检查 Content-Length 是否已满足
                size_t h_end = raw.find("\r\n\r\n");
                std::string header = raw.substr(0, h_end);
                std::string lower = to_lower(header);
                size_t cl_pos = lower.find("content-length:");
                if (cl_pos != std::string::npos) {
                    size_t eol = header.find("\r\n", cl_pos);
                    std::string cl_str = header.substr(cl_pos + 15, eol - cl_pos - 15);
                    try {
                        size_t cl = static_cast<size_t>(std::stoul(cl_str));
                        if (raw.size() >= h_end + 4 + cl) break;
                    } catch (...) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }

        HttpRequest req;
        HttpServerResponse resp;

        if (parse_http_request(raw, req)) {
            // 处理 OPTIONS 预检
            if (req.method == "OPTIONS") {
                resp.status = 200;
                resp.body = "";
                resp.content_type = "text/plain";
            } else {
                try {
                    resp = handler_(req);
                } catch (const std::exception& e) {
                    resp.status = 500;
                    resp.body = std::string("{\"error\":\"internal error: ") + e.what() + "\"}";
                    resp.content_type = "application/json";
                }
            }
        } else {
            resp.status = 400;
            resp.body = "{\"error\":\"invalid http request\"}";
            resp.content_type = "application/json";
        }

        std::string resp_str = build_http_response(resp);
        send(client, resp_str.data(), static_cast<int>(resp_str.size()), 0);
        closesocket(client);
    }

    closesocket(listen_sock);
    std::cerr << "[http] server stopped" << std::endl;
    return 0;
}

} // namespace github_research
