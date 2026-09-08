#pragma once

// 通用独立 WebView2 会话封装
// 用于创建完全隔离的浏览器环境:独立 UserData、独立 Environment、独立 Cookie
// 每个实例内置 std::mutex,保证同一会话内自动化操作串行执行
// 不同 WebViewSession 实例之间操作可以并行

// 继承 IBrowserSession 跨平台接口,ScriptResult 定义在 browser_session.hpp
#include "github_research/browser_session.hpp"

#include <mutex>
#include <atomic>
#include <future>
#include <condition_variable>
#include <windows.h>
#include <unknwn.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include <wrl.h>  // Microsoft::WRL::ComPtr / Callback

namespace github_research {

class WebViewSession : public IBrowserSession {
public:
    WebViewSession();
    ~WebViewSession() override;

    // 禁止拷贝
    WebViewSession(const WebViewSession&) = delete;
    WebViewSession& operator=(const WebViewSession&) = delete;

    // ============ IBrowserSession 接口实现 (UTF-8, bool 返回值) ============

    bool Init(const std::string& userDataDir,
              const std::string& extraArgs = "",
              const std::string& proxy_url = "") override;

    void Destroy() override;

    bool Navigate(const std::string& url) override;

    bool WaitForNavigation(uint32_t timeoutMs = 30000) override;

    ScriptResult ExecuteScript(const std::string& jsCode, uint32_t timeoutMs = 90000) override;

    bool CheckLogin(const std::string& loginDetectJs) override;

    bool IsReady() const override;

private:
    // ============ 内部 HRESULT/wstring 实现 (供 override 包装) ============
    HRESULT InitInternal(const std::wstring& userDataDir,
                         const std::wstring& extraArgs,
                         const std::string& proxy_url);

    HRESULT NavigateInternal(const std::wstring& url);

    // 初始化 COM
    bool init_com();
    // 创建隐藏宿主窗口(WebView2 Controller 需要 HWND,无窗口模式传 nullptr 即可)
    bool create_hidden_window();
    // 创建 WebView2 Environment + Controller + WebView
    HRESULT create_environment(const std::wstring& userDataDir,
                               const std::wstring& extraArgs,
                               const std::string& proxy_url);

    // 内部:等待异步 ready
    HRESULT wait_for_ready(uint32_t timeoutMs);

    // 内部:转换代理 URL
    static std::wstring build_proxy_arg(const std::string& proxy_url);

private:
    // 操作互斥锁:同一会话内所有操作串行执行
    std::mutex              op_mutex_;

    // WebView2 配置
    std::wstring            profile_dir_;

    // COM + 窗口
    HWND                    hwnd_{nullptr};
    HINSTANCE               hinstance_{nullptr};
    bool                    com_initialized_{false};

    // WebView2 COM 对象(使用 WRL::ComPtr RAII)
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2>            webview_;

    // 状态
    std::atomic<bool>       ready_{false};
    std::atomic<bool>       destroyed_{false};

    // 异步同步原语
    std::promise<HRESULT>   init_promise_;
    std::mutex              nav_mtx_;
    std::condition_variable nav_cv_;
    std::atomic<bool>       nav_completed_{false};
    HRESULT                 nav_result_{S_OK};
};

} // namespace github_research
