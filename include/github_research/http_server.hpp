#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <mutex>

namespace github_research {

// HTTP 请求结构
struct HttpRequest {
    std::string method;       // "POST" / "GET"
    std::string path;         // "/mcp" 等
    std::string body;         // 请求体
    std::string content_type; // Content-Type 头
};

// HTTP 响应结构(注意:与 webview_client.hpp 中的 HttpResponse 区分,后者是 fetch 响应)
struct HttpServerResponse {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::string extra_headers;  // 附加 HTTP header(如 "Allow: POST\r\n"),已含 CRLF
};

// 简单的同步 HTTP/1.1 服务器(单线程串行处理)
// 用于 MCP HTTP 传输模式,监听指定端口
// - POST /mcp  -> body 为 JSON-RPC 请求,返回 JSON-RPC 响应
// - GET  /     -> 返回服务状态
// - GET  /sse  -> 返回简单提示(占位,当前未实现完整 SSE)
class HttpServer {
public:
    using Handler = std::function<HttpServerResponse(const HttpRequest&)>;

    HttpServer(int port, Handler handler);
    ~HttpServer();

    // 启动监听,阻塞直到 stop() 被调用或出错
    // 返回非 0 表示出错码
    int run();

    // 请求停止 server(可从其他线程调用)
    void stop();

private:
    int port_;
    Handler handler_;
    std::atomic<bool> stopped_{false};

#ifdef _WIN32
    // WinSock 初始化保护
    static std::mutex wsa_init_mutex_;
    static int wsa_init_count_;
    static bool init_wsa();
    static void fini_wsa();
#endif
};

} // namespace github_research
