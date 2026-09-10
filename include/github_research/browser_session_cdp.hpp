#pragma once

// ============================================================================
// CdpBrowserSession — CDP Chrome 远程控制后端
//
// 架构: 独立 Chrome 进程 + CDP WebSocket (curl v8 内置 WebSocket API)
// 平台依赖: 仅 Windows.h (CreateProcess/TerminateProcess), Linux 用 fork+exec
// GUI 依赖: 零 — Chrome 自处理窗口/X11/Wayland,本程序全程网络协议
//
// 核心机制:
//   1. Init() 启动 Chrome --remote-debugging-port=<port>, 轮询 /json/version 就绪
//   2. ConnectWebSocket() curl_easy_perform(CONNECT_ONLY) + 取 webSocketDebuggerUrl
//   3. SendCdpCommand() 封 JSON {id, method, params}, curl_ws_send
//   4. RecvLoop() curl_ws_recv 阻塞循环, 匹配 id 后返回, 无匹配时继续收/超时
//
// 继承: IBrowserSession (UTF-8 std::string, bool 返回值, ScriptResult)
// ============================================================================

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // 必须在 windows.h 之前,否则 curl 内部引 winsock.h 冲突
#include <windows.h>
#endif

#include "github_research/browser_session.hpp"
#include "github_research/browser_config.hpp"

#include <string>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <curl/curl.h>

namespace github_research {

class CdpBrowserSession : public IBrowserSession {
public:
    CdpBrowserSession();
    ~CdpBrowserSession() override;

    CdpBrowserSession(const CdpBrowserSession&) = delete;
    CdpBrowserSession& operator=(const CdpBrowserSession&) = delete;

    // ============ IBrowserSession 接口实现 ============
    bool Init(const std::string& userDataDir,
              const std::string& extraArgs = "",
              const std::string& proxy_url = "") override;

    void Destroy() override;

    bool Navigate(const std::string& url) override;

    bool WaitForNavigation(uint32_t timeoutMs = 30000) override;

    ScriptResult ExecuteScript(const std::string& jsCode,
                               uint32_t timeoutMs = 90000) override;

    bool CheckLogin(const std::string& loginDetectJs) override;

    bool IsReady() const override { return ready_.load(); }

private:
    // ---------- 跨平台进程管理 ----------
    // 启动 Chrome 子进程 (阻塞)
    // cmdlineUtf8 是完整命令行 (含 chrome.exe + 所有参数), UTF-8 编码
    // 返回 true 表示进程创建成功, 句柄/信息存入 platform 成员
    bool launch_chrome(const std::string& cmdlineUtf8);

    // 终止 Chrome 进程 (阻塞直到退出或超时)
    void terminate_chrome();

    // ---------- CDP 连接 ----------
    // 等待 CDP 端口就绪 (HTTP /json/version 轮询)
    bool wait_cdp_ready(uint16_t port, uint32_t timeoutMs);

    // 从 /json 接口取第一个页面的 webSocketDebuggerUrl
    std::string get_page_ws_url(uint16_t port);

    // curl WebSocket 握手 (CURLOPT_CONNECT_ONLY)
    bool connect_websocket(const std::string& wsUrl);

    void close_websocket();

    // ---------- CDP 命令 ----------
    // 发送 CDP 命令 (自动分配 id), 返回匹配 id 的完整响应 JSON
    // timeoutMs: 单次命令超时
    // 返回空字符串 = 超时/错误
    std::string send_cdp_command(const std::string& method,
                                const std::string& paramsJson,
                                uint32_t timeoutMs);

    // 启动浏览器级别的 Browser.close (优雅关闭)
    bool send_browser_close();

private:
    // ---------- 状态 ----------
    std::atomic<bool> ready_{false};
    std::atomic<bool> destroyed_{false};
    uint16_t debug_port_{0};
    std::string chrome_path_;
    std::string user_data_dir_;
    std::string proxy_url_;
    std::atomic<int> next_msg_id_{0};

    // ---------- WebSocket (原生 socket, 绕开 curl WS API) ----------
#ifdef _WIN32
    SOCKET ws_sock_{INVALID_SOCKET};
#else
    int ws_sock_{-1};
#endif
    std::mutex ws_mutex_;

    // ---------- Windows 进程句柄 ----------
#ifdef _WIN32
    PROCESS_INFORMATION proc_info_{};
    HANDLE              process_handle_{nullptr};
#endif

    // ---------- 跨平台通用 ----------
    // 保存 pid 便于跨平台一致访问 (Windows 同时有 process_handle_)
    uint64_t pid_{0};
};

} // namespace github_research
