#include "github_research/webview_session.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <codecvt>
#include <locale>
#include <thread>
#include <chrono>
#include <wrl.h>
#include <wrl/implements.h>
#include <wrl/client.h>

namespace github_research {

namespace {

// ============== 字符串转换 ==============
std::wstring Utf8ToWstr(const std::string& utf8) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(utf8);
    } catch (...) {
        return std::wstring(utf8.begin(), utf8.end());
    }
}

std::string WstrToUtf8(const std::wstring& wstr) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    } catch (...) {
        return std::string(wstr.begin(), wstr.end());
    }
}

// ============== 反自动化默认启动参数 ==============
constexpr const wchar_t* kAntiDetectArgs =
    L"--disable-blink-features=AutomationControlled "
    L"--exclude-switches=enable-automation "
    L"--disable-dev-shm-usage";

} // namespace

// ============================================================
// 构造/析构
// ============================================================

WebViewSession::WebViewSession() = default;

WebViewSession::~WebViewSession() {
    Destroy();
}

bool WebViewSession::IsReady() const {
    return ready_.load() && !destroyed_.load() && webview_ != nullptr;
}

// ============================================================
// COM + 窗口
// ============================================================

bool WebViewSession::init_com() {
    if (com_initialized_) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[session] CoInitializeEx failed: 0x"
                  << std::hex << hr << std::endl;
        return false;
    }
    com_initialized_ = true;
    return true;
}

bool WebViewSession::create_hidden_window() {
    if (hwnd_) return true;

    hinstance_ = GetModuleHandleW(nullptr);
    if (!hinstance_) return false;

    // 注册极简窗口类(一次即可,重复注册会失败,忽略即可)
    const wchar_t* kClassName = L"WebViewSessionHiddenHost";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hinstance_;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // 重复注册失败不影响

    hwnd_ = CreateWindowExW(
        0, kClassName, L"WebViewSessionHost", WS_OVERLAPPEDWINDOW,
        0, 0, 0, 0, nullptr, nullptr, hinstance_, nullptr);
    // 允许 nullptr: WebView2 CreateCoreWebView2Controller 支持 null HWND (无窗口模式)
    // 这里若窗口创建失败,就走无窗口模式(nullptr)
    return true;  // 即使失败,用 nullptr 继续
}

// ============================================================
// 构建 --proxy-server 参数
// ============================================================
std::wstring WebViewSession::build_proxy_arg(const std::string& proxy_url) {
    if (proxy_url.empty()) return L"";
    // Chromium --proxy-server 不接受带 scheme 前缀的 URL,需要剥离 http:// https://
    std::wstring p = Utf8ToWstr(proxy_url);
    const std::wstring schemes[] = { L"http://", L"https://", L"socks5://", L"socks4://" };
    for (const auto& s : schemes) {
        if (p.size() > s.size() &&
            _wcsnicmp(p.c_str(), s.c_str(), s.size()) == 0) {
            p = p.substr(s.size());
            break;
        }
    }
    return L" --proxy-server=" + p;
}

// ============================================================
// InitInternal: 原有 HRESULT/wstring 实现 (override 包装层)
// ============================================================
HRESULT WebViewSession::InitInternal(const std::wstring& userDataDir,
                              const std::wstring& extraArgs,
                              const std::string& proxy_url) {
    std::lock_guard<std::mutex> lock(op_mutex_);
    if (ready_.load()) return S_FALSE; // 已初始化

    profile_dir_ = userDataDir;

    if (!init_com()) return E_FAIL;
    create_hidden_window();  // 即使失败也继续(nullptr = 无窗口模式)

    return create_environment(userDataDir, extraArgs, proxy_url);
}

// ============================================================
// Init override (UTF-8 std::string → wstring 转码)
// ============================================================
bool WebViewSession::Init(const std::string& userDataDir,
                          const std::string& extraArgs,
                          const std::string& proxy_url) {
    HRESULT hr = InitInternal(Utf8ToWstr(userDataDir), Utf8ToWstr(extraArgs), proxy_url);
    return SUCCEEDED(hr);
}

// ============================================================
// SDK 内置 CoreWebView2EnvironmentOptions(单一技术栈,与 WebViewClient 一致)
// ============================================================
namespace {
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>
    MakeSimpleOptions(const std::wstring& extraArgs) {
        // 使用 SDK 内置 CoreWebView2EnvironmentOptions(与 WebViewClient 保持一致)
        // 避免自定义 COM 实现导致 CreateCoreWebView2EnvironmentWithOptions 返回 0x80070057
        auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (!opts) return nullptr;
        if (!extraArgs.empty()) {
            opts->put_AdditionalBrowserArguments(extraArgs.c_str());
        }
        Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> options;
        opts.As(&options);
        return options;
    }
}  // namespace

// ============================================================
// 异步 Environment + Controller + WebView 创建
// ============================================================
HRESULT WebViewSession::create_environment(const std::wstring& userDataDir,
                                            const std::wstring& extraArgs,
                                            const std::string& proxy_url) {
    // 合并启动参数:反自动化 + 代理 + 用户额外参数
    std::wstring all_args = kAntiDetectArgs;
    std::wstring proxy_arg = build_proxy_arg(proxy_url);
    if (!proxy_arg.empty()) all_args += proxy_arg;
    if (!extraArgs.empty()) {
        all_args += L" ";
        all_args += extraArgs;
    }
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> options =
        MakeSimpleOptions(all_args);
    if (!options) {
        std::cerr << "[session] MakeSimpleOptions failed" << std::endl;
        return E_FAIL;
    }

    // 重置 promise(每次创建重新初始化)
    init_promise_ = std::promise<HRESULT>();
    auto fut = init_promise_.get_future();

    auto envHandler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [this, userDataDir](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
                init_promise_.set_value(result);
                return result;
            }
            environment_ = env;

            // 创建 Controller(无窗口:hwnd_ 可为 nullptr)
            env->CreateCoreWebView2Controller(
                hwnd_,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                        if (SUCCEEDED(res)) {
                            controller_ = controller;
                            if (controller_) {
                                controller_->get_CoreWebView2(&webview_);
                            }
                            // 订阅 NavigationCompleted 事件
                            if (webview_) {
                                webview_->add_NavigationCompleted(
                                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL success = FALSE;
                                            COREWEBVIEW2_WEB_ERROR_STATUS webErr =
                                                COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                            args->get_IsSuccess(&success);
                                            args->get_WebErrorStatus(&webErr);
                                            {
                                                std::lock_guard<std::mutex> lk(nav_mtx_);
                                                nav_result_ = success
                                                    ? S_OK
                                                    : static_cast<HRESULT>(webErr);
                                                nav_completed_.store(true);
                                            }
                                            nav_cv_.notify_all();
                                            return S_OK;
                                        }).Get(),
                                    nullptr);
                            }
                        }
                        init_promise_.set_value(res);
                        return res;
                    }).Get()
            );
            return S_OK;
        });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,                 // browser exe dir(系统 Edge)
        userDataDir.c_str(),     // user data folder
        options.Get(),           // options
        envHandler.Get()         // completion handler
    );

    if (FAILED(hr)) {
        std::cerr << "[session] CreateCoreWebView2EnvironmentWithOptions failed: 0x"
                  << std::hex << hr << std::endl;
        return hr;
    }

    // 等待异步创建(最多 30s)
    // 关键:必须 pump 消息循环,否则 WebView2 COM 回调无法触发(与 WebViewClient 一致)
    auto start = std::chrono::steady_clock::now();
    const int init_timeout = 30;  // 初始化最长等 30s
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        auto status = fut.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) break;
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count() > init_timeout) {
            std::cerr << "[session] WebView2 init timeout (>30s)" << std::endl;
            return E_FAIL;
        }
        Sleep(10);
    }
    HRESULT result = fut.get();
    if (SUCCEEDED(result) && webview_ != nullptr) {
        ready_.store(true);
        std::cerr << "[session] WebView2 ready, profile: " << WstrToUtf8(userDataDir) << std::endl;
    }
    return result;
}

// ============================================================
// 销毁
// ============================================================
void WebViewSession::Destroy() {
    std::lock_guard<std::mutex> lock(op_mutex_);
    if (destroyed_.load()) return;
    destroyed_.store(true);
    ready_.store(false);

    webview_.Reset();
    controller_.Reset();
    environment_.Reset();

    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

// ============================================================
// 导航
// ============================================================
// ============================================================
// NavigateInternal: 原有 HRESULT/wstring 实现 (override 包装层)
// ============================================================
HRESULT WebViewSession::NavigateInternal(const std::wstring& url) {
    std::lock_guard<std::mutex> lock(op_mutex_);
    if (!ready_.load() || !webview_) return E_FAIL;

    // 重置导航完成标志
    {
        std::lock_guard<std::mutex> lk(nav_mtx_);
        nav_completed_.store(false);
        nav_result_ = S_OK;
    }

    HRESULT hr = webview_->Navigate(url.c_str());
    if (FAILED(hr)) {
        std::cerr << "[session] Navigate failed: 0x" << std::hex << hr << std::endl;
    }
    return hr;
}

// ============================================================
// Navigate override (UTF-8 std::string → wstring 转码)
// ============================================================
bool WebViewSession::Navigate(const std::string& url) {
    HRESULT hr = NavigateInternal(Utf8ToWstr(url));
    return SUCCEEDED(hr);
}

bool WebViewSession::WaitForNavigation(uint32_t timeoutMs) {
    if (!ready_.load()) return false;
    // 关键:必须 pump 消息循环,否则 NavigationCompleted 事件回调无法触发
    auto start = std::chrono::steady_clock::now();
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        {
            std::lock_guard<std::mutex> lk(nav_mtx_);
            if (nav_completed_.load()) return SUCCEEDED(nav_result_);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > static_cast<int64_t>(timeoutMs)) {
            std::cerr << "[session] WaitForNavigation timeout (>=" << timeoutMs << "ms)" << std::endl;
            return false;
        }
        Sleep(5);
    }
}

// ============================================================
// JS 执行
// ============================================================
ScriptResult WebViewSession::ExecuteScript(const std::string& jsCode, uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(op_mutex_);
    ScriptResult ret{};
    if (!ready_.load() || !webview_) {
        ret.error = "WebViewSession not ready";
        return ret;
    }

    std::promise<ScriptResult> promise;
    auto fut = promise.get_future();

    webview_->ExecuteScript(
        Utf8ToWstr(jsCode).c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [&promise](HRESULT hr, LPCWSTR jsonResult) -> HRESULT {
                ScriptResult res;
                if (SUCCEEDED(hr)) {
                    res.success = true;
                    res.data = WstrToUtf8(jsonResult ? jsonResult : L"");
                } else {
                    res.success = false;
                    std::wostringstream ss;
                    ss << L"ExecuteScript failed, hr=0x" << std::hex << hr;
                    res.error = WstrToUtf8(ss.str());
                }
                promise.set_value(res);
                return S_OK;
            }).Get()
    );

    // 关键:必须 pump 消息循环,否则 ExecuteScript 回调无法触发
    auto start = std::chrono::steady_clock::now();
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        auto status = fut.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) return fut.get();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > static_cast<int64_t>(timeoutMs)) {
            ret.error = "ExecuteScript timeout (>=" + std::to_string(timeoutMs) + "ms)";
            return ret;
        }
        Sleep(5);
    }
}

// ============================================================
// CheckLogin(便捷包装)
// ============================================================
bool WebViewSession::CheckLogin(const std::string& loginDetectJs) {
    auto res = ExecuteScript(loginDetectJs);
    if (!res.success) return false;
    // 简单规则:JS 返回 JSON 中包含 true 或非空字符串视为已登录
    return res.data.find("true") != std::string::npos ||
           (res.data.size() > 2 && res.data != "null" && res.data != "false" && res.data != "{}");
}

// ============================================================
// wait_for_ready (当前未用到,保持接口)
// ============================================================
HRESULT WebViewSession::wait_for_ready(uint32_t /*timeoutMs*/) {
    return ready_.load() ? S_OK : E_FAIL;
}

} // namespace github_research
