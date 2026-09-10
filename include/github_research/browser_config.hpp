#pragma once

// ============================================================================
// BrowserConfig — CDP / WebView2 双后端统一配置
//
// 配置来源优先级 (高→低):
//   1. 命令行 --browser-config 指定路径
//   2. 环境变量 RESEARCH_MCP_BROWSER_CONFIG
//   3. ~/.research-mcp/browser_config.json
//   4. ./browser_config.json
//   5. 内置默认值
//
// JSON 支持 // 注释 (加载时自动剥离)
// ============================================================================

#include <string>
#include <vector>
#include <array>
#include <unordered_map>

namespace github_research {

// ============================================================================
// Chrome 段: 二进制与启动参数
// ============================================================================
struct ChromeConfig {
    std::string binary_path;                  // 强制路径, 非空时忽略 auto_detect
    std::unordered_map<std::string, std::string> binary_path_by_platform; // windows/linux/linux_wayland/macos
    bool auto_detect = true;
    std::string headless_mode = "new";        // "new" / "old" / "disabled"
    std::array<int, 2> window_size = {1280, 800};
    bool disable_gpu = true;
    bool no_sandbox = false;
    std::vector<std::string> extra_args;

    // 运行时解析后的绝对路径 (resolve_chrome_path 填充)
    std::string resolved_path;
};

// ============================================================================
// CDP 段: DevTools 协议连接参数
// ============================================================================
struct CdpConfig {
    int debug_port_range_start = 9222;
    int connect_timeout_ms = 15000;
    int command_timeout_ms = 90000;
    int navigate_timeout_ms = 30000;
    int ws_poll_interval_ms = 10;
    std::string endpoint_json = "http://127.0.0.1:%d/json";
    std::string endpoint_ws = "ws://127.0.0.1:%d/devtools/page/%s";
};

// ============================================================================
// Session 段: 会话隔离与缓存
// ============================================================================
struct SessionConfig {
    std::string user_data_dir_base = "./profiles";
    std::string default_profile = "default";
    bool isolate_per_profile = true;
    bool enable_cache = true;
    bool clear_cache_on_start = false;
};

// ============================================================================
// Proxy 段
// ============================================================================
struct ProxyConfig {
    std::string mode = "direct";              // "direct" / "system" / "fixed"
    std::string server;                       // fixed 模式下:  "http://ip:port" 或 "socks5://ip:port"
    std::vector<std::string> bypass_list = {"localhost", "127.0.0.1", "::1"};
};

// ============================================================================
// Extensions 段
// ============================================================================
struct ExtensionsConfig {
    bool enabled = false;
    std::vector<std::string> load_paths;      // Chrome --load-extension= 目录列表
    std::vector<std::string> inject_scripts;  // CDP 注入 JS 文件路径列表
};

// ============================================================================
// BrowserConfig 单例
// ============================================================================
class BrowserConfig {
public:
    static BrowserConfig& instance();

    // 从 JSON 文件加载 (加载成功返回 true, 失败保留当前值)
    bool load(const std::string& path);

    // 按默认查找链自动加载 (命令行 > 环境变量 > ~/.research-mcp > ./browser_config.json)
    // cmdline_arg 可为空, 为空时跳过命令行步骤
    bool load_defaults(const std::string& cmdline_arg = "");

    // 仅用内置硬编码默认值填充 (不查文件)
    void reset_to_builtin_defaults();

    // ---- 运行时辅助 ----

    // 解析 Chrome 绝对路径 (按 binary_path → binary_path_by_platform → auto_detect 顺序)
    std::string resolve_chrome_path();

    // 构造 Chrome 启动命令行 (完整: "chrome.exe --arg1 --arg2 ... about:blank")
    std::string build_chrome_command(
        const std::string& chrome_bin,
        uint16_t port,
        const std::string& user_data_dir,
        const std::string& extra_proxy = "",
        const std::string& extra_args_append = "");

    // 平台名: "windows" / "linux" / "macos" (用于 binary_path_by_platform 查表)
    static std::string current_platform();

    // 环境探测: 是否 Wayland 环境
    static bool is_wayland();

    // ---- 数据成员 ----
    ChromeConfig    chrome;
    CdpConfig       cdp;
    SessionConfig   session;
    ProxyConfig     proxy;
    ExtensionsConfig extensions;

private:
    BrowserConfig();
    BrowserConfig(const BrowserConfig&) = delete;
    BrowserConfig& operator=(const BrowserConfig&) = delete;
};

} // namespace github_research
