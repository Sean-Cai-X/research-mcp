#pragma once

#include <string>
#include <optional>
#include <memory>
#include <nlohmann/json.hpp>
#include "github_client.hpp"
#include "webview_session.hpp"

namespace github_research {

using json = nlohmann::json;

// MCP Server over stdio / HTTP(JSON-RPC 2.0)
// 8 源统一架构:全部基于 WebViewSession(独立 UserData 目录隔离)
//   GitHub(arxiv_session_ 之前的内部 WebViewClient)
//   arXiv / Hacker News / npm+PyPI / Papers with Code /
//   Hugging Face / Semantic Scholar / Stack Overflow
class McpServer {
public:
    McpServer(std::optional<std::string> token,
              int timeout_seconds = 30);
    ~McpServer();

    // 设置代理(应用到所有 WebView 会话)
    void set_proxy(const std::string& proxy_url);

    // ============ GitHub 后端独立 user data dir ============
    // 必须在 run()/run_http() 之前调用(确保首次请求前生效)
    // 用于 8 源会话隔离,避免与默认 LOCALAPPDATA 路径冲突
    void init_github_profile(const std::string& userDataDir) {
        github_profile_dir_ = userDataDir;
        client_.set_user_data_dir(userDataDir);
    }

    // ============ 各源 profile 路径(懒加载用) ============
    // 启动时不初始化任何 WebView2 会话,首次 tool 调用时按需创建
    struct ProfilePaths {
        std::string arxiv, hn, pkg, pwc, hf, s2, so;
    };
    void set_profiles(const ProfilePaths& paths) { profile_paths_ = paths; }

    // ============ 各源 WebView 会话初始化(可选,按需启动) ============
    // 每个源使用独立 UserData 目录,Cookie/缓存完全隔离

    bool init_arxiv(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_arxiv();
    bool has_arxiv() const { return arxiv_session_ != nullptr; }

    bool init_hackernews(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_hackernews();
    bool has_hackernews() const { return hn_session_ != nullptr; }

    bool init_package(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_package();
    bool has_package() const { return pkg_session_ != nullptr; }

    bool init_paperswithcode(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_paperswithcode();
    bool has_paperswithcode() const { return pwc_session_ != nullptr; }

    bool init_huggingface(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_huggingface();
    bool has_huggingface() const { return hf_session_ != nullptr; }

    bool init_semanticscholar(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_semanticscholar();
    bool has_semanticscholar() const { return s2_session_ != nullptr; }

    bool init_stackoverflow(const std::wstring& userDataDir, const std::string& proxy_url = "");
    void shutdown_stackoverflow();
    bool has_stackoverflow() const { return so_session_ != nullptr; }

    // 运行 server(标准输入输出模式)
    int run();

    // 运行 HTTP 模式
    int run_http(int port);

    struct HttpResult {
        int status = 200;
        std::string body;
        std::string content_type = "application/json";
        std::string extra_headers;  // 附加 HTTP header(如 "Allow: POST\r\n"),已含 CRLF
    };
    HttpResult handle_http_request(const std::string& method,
                                   const std::string& path,
                                   const std::string& body);

private:
    std::string handle_request(const json& request);
    json handle_initialize(const json& params);
    json handle_tools_list();
    json handle_tools_call(const json& params);

    // 各源工具分发
    json dispatch_arxiv_tool(const std::string& tool_name, const json& args);
    json dispatch_hn_tool(const std::string& tool_name, const json& args);
    json dispatch_pkg_tool(const std::string& tool_name, const json& args);
    json dispatch_pwc_tool(const std::string& tool_name, const json& args);
    json dispatch_hf_tool(const std::string& tool_name, const json& args);
    json dispatch_s2_tool(const std::string& tool_name, const json& args);
    json dispatch_so_tool(const std::string& tool_name, const json& args);
    json dispatch_research_tool(const std::string& tool_name, const json& args);

    // 通用 init/shutdown 辅助
    bool init_session(std::unique_ptr<WebViewSession>& session,
                      const std::wstring& userDataDir,
                      const std::string& proxy_url,
                      const char* logName);
    void shutdown_session(std::unique_ptr<WebViewSession>& session, const char* logName);

    // 懒加载:首次 tool 调用时按 profile 路径初始化对应会话
    // 返回 false 表示该源未配置 profile 或初始化失败
    bool ensure_arxiv_session();
    bool ensure_hn_session();
    bool ensure_pkg_session();
    bool ensure_pwc_session();
    bool ensure_hf_session();
    bool ensure_s2_session();
    bool ensure_so_session();

    bool read_line(std::string& line);
    void write_line(const std::string& line);
    void log(const std::string& msg);

    GitHubClient client_;

    // 8 源独立 WebView 会话(按需初始化,未 init 时为 nullptr)
    std::unique_ptr<WebViewSession> arxiv_session_;
    std::unique_ptr<WebViewSession> hn_session_;
    std::unique_ptr<WebViewSession> pkg_session_;
    std::unique_ptr<WebViewSession> pwc_session_;
    std::unique_ptr<WebViewSession> hf_session_;
    std::unique_ptr<WebViewSession> s2_session_;
    std::unique_ptr<WebViewSession> so_session_;

    std::string proxy_url_;
    std::string github_profile_dir_;  // GitHub 后端独立 user data dir
    ProfilePaths profile_paths_;     // 各源 profile 路径(懒加载用)
    bool initialized_ = false;
};

} // namespace github_research
