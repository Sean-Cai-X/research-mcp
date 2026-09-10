// ============================================================================
// BrowserConfig 实现 — JSON 配置 + Chrome 路径探测 + 命令构造
// ============================================================================

#include "github_research/browser_config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace github_research {

using json = nlohmann::json;

// ============================================================================
// 单例
// ============================================================================
BrowserConfig& BrowserConfig::instance() {
    static BrowserConfig inst;
    return inst;
}

BrowserConfig::BrowserConfig() {
    reset_to_builtin_defaults();
}

// ============================================================================
// 默认值
// ============================================================================
void BrowserConfig::reset_to_builtin_defaults() {
    chrome = ChromeConfig();
    cdp = CdpConfig();
    session = SessionConfig();
    proxy = ProxyConfig();
    extensions = ExtensionsConfig();

    // binary_path_by_platform 内置默认值
#ifdef _WIN32
    chrome.binary_path_by_platform["windows"] =
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
#endif
    chrome.binary_path_by_platform["linux"] = "/usr/bin/google-chrome";
    chrome.binary_path_by_platform["linux_wayland"] = "/usr/bin/google-chrome";
    chrome.binary_path_by_platform["macos"] =
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
}

// ============================================================================
// 平台探测
// ============================================================================
std::string BrowserConfig::current_platform() {
#ifdef _WIN32
    return "windows";
#elif __APPLE__
    return "macos";
#else
    return "linux";
#endif
}

bool BrowserConfig::is_wayland() {
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    return wd != nullptr && wd[0] != '\0';
}

// ============================================================================
// 注释剥离: 把 // line comment 去掉 (// 忽略字符串内的, 简单处理足够)
// ============================================================================
static std::string strip_comments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool in_str = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '"' && (i == 0 || in[i - 1] != '\\')) {
            in_str = !in_str;
            out.push_back(c);
        } else if (!in_str && c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            // // comment: 跳到行尾
            while (i < in.size() && in[i] != '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// ============================================================================
// 文件存在检查
// ============================================================================
static bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ============================================================================
// JSON 解析辅助: 安全 get_or_default
// ============================================================================
template <typename T>
static T get_or(const json& j, const char* key, T def) {
    if (j.contains(key) && !j[key].is_null()) {
        try { return j[key].get<T>(); } catch (...) {}
    }
    return def;
}

// ============================================================================
// 加载单个 JSON 文件
// ============================================================================
bool BrowserConfig::load(const std::string& path) {
    if (!file_exists(path)) return false;

    std::ifstream f(path);
    if (!f) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();

    try {
        json j = json::parse(strip_comments(raw));

        // ---- chrome 段 ----
        if (auto it = j.find("chrome"); it != j.end()) {
            const auto& c = *it;
            chrome.binary_path       = get_or<std::string>(c, "binary_path", chrome.binary_path);
            chrome.auto_detect       = get_or<bool>(c, "auto_detect", chrome.auto_detect);
            chrome.headless_mode     = get_or<std::string>(c, "headless_mode", chrome.headless_mode);
            chrome.disable_gpu       = get_or<bool>(c, "disable_gpu", chrome.disable_gpu);
            chrome.no_sandbox        = get_or<bool>(c, "no_sandbox", chrome.no_sandbox);

            if (c.contains("window_size") && c["window_size"].is_array() && c["window_size"].size() == 2) {
                chrome.window_size[0] = c["window_size"][0].get<int>();
                chrome.window_size[1] = c["window_size"][1].get<int>();
            }

            if (c.contains("extra_args") && c["extra_args"].is_array()) {
                chrome.extra_args.clear();
                for (const auto& a : c["extra_args"]) {
                    if (a.is_string()) chrome.extra_args.push_back(a.get<std::string>());
                }
            }

            if (c.contains("binary_path_by_platform") && c["binary_path_by_platform"].is_object()) {
                for (const auto& [k, v] : c["binary_path_by_platform"].items()) {
                    if (v.is_string()) chrome.binary_path_by_platform[k] = v.get<std::string>();
                }
            }
        }

        // ---- cdp 段 ----
        if (auto it = j.find("cdp"); it != j.end()) {
            const auto& c = *it;
            cdp.debug_port_range_start = get_or<int>(c, "debug_port_range_start", cdp.debug_port_range_start);
            cdp.connect_timeout_ms     = get_or<int>(c, "connect_timeout_ms", cdp.connect_timeout_ms);
            cdp.command_timeout_ms     = get_or<int>(c, "command_timeout_ms", cdp.command_timeout_ms);
            cdp.navigate_timeout_ms    = get_or<int>(c, "navigate_timeout_ms", cdp.navigate_timeout_ms);
            cdp.ws_poll_interval_ms    = get_or<int>(c, "ws_poll_interval_ms", cdp.ws_poll_interval_ms);
            cdp.endpoint_json          = get_or<std::string>(c, "endpoint_json", cdp.endpoint_json);
            cdp.endpoint_ws            = get_or<std::string>(c, "endpoint_ws", cdp.endpoint_ws);
        }

        // ---- session 段 ----
        if (auto it = j.find("session"); it != j.end()) {
            const auto& c = *it;
            session.user_data_dir_base = get_or<std::string>(c, "user_data_dir_base", session.user_data_dir_base);
            session.default_profile    = get_or<std::string>(c, "default_profile", session.default_profile);
            session.isolate_per_profile= get_or<bool>(c, "isolate_per_profile", session.isolate_per_profile);
            session.enable_cache       = get_or<bool>(c, "enable_cache", session.enable_cache);
            session.clear_cache_on_start = get_or<bool>(c, "clear_cache_on_start", session.clear_cache_on_start);
        }

        // ---- proxy 段 ----
        if (auto it = j.find("proxy"); it != j.end()) {
            const auto& c = *it;
            proxy.mode   = get_or<std::string>(c, "mode", proxy.mode);
            proxy.server = get_or<std::string>(c, "server", proxy.server);

            if (c.contains("bypass_list") && c["bypass_list"].is_array()) {
                proxy.bypass_list.clear();
                for (const auto& a : c["bypass_list"]) {
                    if (a.is_string()) proxy.bypass_list.push_back(a.get<std::string>());
                }
            }
        }

        // ---- extensions 段 ----
        if (auto it = j.find("extensions"); it != j.end()) {
            const auto& c = *it;
            extensions.enabled = get_or<bool>(c, "enabled", extensions.enabled);

            if (c.contains("load_paths") && c["load_paths"].is_array()) {
                extensions.load_paths.clear();
                for (const auto& a : c["load_paths"]) {
                    if (a.is_string()) extensions.load_paths.push_back(a.get<std::string>());
                }
            }
            if (c.contains("inject_scripts") && c["inject_scripts"].is_array()) {
                extensions.inject_scripts.clear();
                for (const auto& a : c["inject_scripts"]) {
                    if (a.is_string()) extensions.inject_scripts.push_back(a.get<std::string>());
                }
            }
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BrowserConfig] parse failed: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================================
// 默认查找链
// ============================================================================
bool BrowserConfig::load_defaults(const std::string& cmdline_arg) {
    // 1. 命令行
    if (!cmdline_arg.empty() && load(cmdline_arg)) {
        std::cerr << "[BrowserConfig] loaded from --browser-config: " << cmdline_arg << std::endl;
        return true;
    }

    // 2. 环境变量
    const char* env = std::getenv("RESEARCH_MCP_BROWSER_CONFIG");
    if (env && env[0] != '\0' && load(env)) {
        std::cerr << "[BrowserConfig] loaded from RESEARCH_MCP_BROWSER_CONFIG: " << env << std::endl;
        return true;
    }

    // 3. ~/.research-mcp/browser_config.json
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home && home[0] != '\0') {
        std::string p = std::string(home) + "/.research-mcp/browser_config.json";
        if (load(p)) {
            std::cerr << "[BrowserConfig] loaded from " << p << std::endl;
            return true;
        }
    }

    // 4. 当前目录
    if (load("./browser_config.json")) {
        std::cerr << "[BrowserConfig] loaded from ./browser_config.json" << std::endl;
        return true;
    }

    std::cerr << "[BrowserConfig] no config file found, using built-in defaults" << std::endl;
    return false;
}

// ============================================================================
// Chrome 路径探测
// ============================================================================
std::string BrowserConfig::resolve_chrome_path() {
    // 1. 强制路径
    if (!chrome.binary_path.empty() && file_exists(chrome.binary_path)) {
        chrome.resolved_path = chrome.binary_path;
        return chrome.resolved_path;
    }

    // 2. 平台配置路径 (考虑 Wayland 优先)
    std::string plat = current_platform();
    if (plat == "linux" && is_wayland()) {
        auto it = chrome.binary_path_by_platform.find("linux_wayland");
        if (it != chrome.binary_path_by_platform.end() && file_exists(it->second)) {
            chrome.resolved_path = it->second;
            return chrome.resolved_path;
        }
    }
    auto it = chrome.binary_path_by_platform.find(plat);
    if (it != chrome.binary_path_by_platform.end() && file_exists(it->second)) {
        chrome.resolved_path = it->second;
        return chrome.resolved_path;
    }

    // 3. 自动探测 (硬编码候选)
    if (chrome.auto_detect) {
#ifdef _WIN32
        const char* candidates[] = {
            "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
            "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        };
        for (auto p : candidates) {
            if (file_exists(p)) { chrome.resolved_path = p; return chrome.resolved_path; }
        }
        // %LOCALAPPDATA%
        char localappdata[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", localappdata, sizeof(localappdata));
        if (len > 0 && len < sizeof(localappdata)) {
            std::string p = std::string(localappdata) + "\\Google\\Chrome\\Application\\chrome.exe";
            if (file_exists(p)) { chrome.resolved_path = p; return chrome.resolved_path; }
        }
#else
        const char* candidates[] = {
            "/usr/bin/google-chrome",
            "/usr/bin/google-chrome-stable",
            "/usr/bin/chromium",
            "/usr/bin/chromium-browser",
            "/snap/bin/chromium",
        };
        for (auto p : candidates) {
            if (file_exists(p)) { chrome.resolved_path = p; return chrome.resolved_path; }
        }
#endif
        // 环境变量 CHROME_PATH
        const char* env = std::getenv("CHROME_PATH");
        if (env && env[0] != '\0' && file_exists(env)) {
            chrome.resolved_path = env;
            return chrome.resolved_path;
        }
    }

    // 探测失败: 还是返回 binary_path 让调用方报错
    chrome.resolved_path = chrome.binary_path;
    return chrome.resolved_path;
}

// ============================================================================
// Chrome 启动命令行构造
// ============================================================================
std::string BrowserConfig::build_chrome_command(
    const std::string& chrome_bin,
    uint16_t port,
    const std::string& user_data_dir,
    const std::string& extra_proxy,
    const std::string& extra_args_append)
{
    std::string args;

    // ---- 核心 CDP 参数 ----
    args += " --remote-debugging-port=" + std::to_string(port);
    args += " --user-data-dir=\"" + user_data_dir + "\"";

    // ---- 无头模式 ----
    if (chrome.headless_mode == "new") {
        args += " --headless=new";
    } else if (chrome.headless_mode == "old") {
        args += " --headless";
    }
    // "disabled" = 不加 headless 参数

    // ---- 窗口尺寸 ----
    args += " --window-size=" + std::to_string(chrome.window_size[0])
          + "," + std::to_string(chrome.window_size[1]);

    // ---- 稳定性参数 (总是加, 无害) ----
    args += " --no-first-run";
    args += " --no-default-browser-check";
    args += " --disable-blink-features=AutomationControlled";
    args += " --disable-dev-shm-usage";

    if (chrome.disable_gpu) args += " --disable-gpu";
    if (chrome.no_sandbox)  args += " --no-sandbox";

    // ---- Wayland ----
    if (is_wayland() && chrome.headless_mode == "disabled") {
        args += " --ozone-platform=wayland";
    }

    // ---- 缓存 ----
    if (!session.enable_cache) args += " --disk-cache-dir=/dev/null";
    if (session.clear_cache_on_start) args += " --disk-cache-dir=/dev/null";

    // ---- 代理 (优先级: extra_proxy > proxy.server) ----
    std::string effective_proxy = extra_proxy.empty() ? proxy.server : extra_proxy;
    if (!effective_proxy.empty()) {
        args += " --proxy-server=\"" + effective_proxy + "\"";
    }

    // ---- 扩展加载 ----
    for (const auto& ep : extensions.load_paths) {
        args += " --load-extension=\"" + ep + "\"";
    }

    // ---- extra_args (用户配置) ----
    for (const auto& a : chrome.extra_args) {
        args += " " + a;
    }

    // ---- 调用方额外追加 ----
    if (!extra_args_append.empty()) args += " " + extra_args_append;

    args += " about:blank";

    return chrome_bin + args;
}

} // namespace github_research
