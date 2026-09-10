#pragma once

// ============================================================================
// IBrowserSession — 跨平台浏览器会话纯虚接口
//
// 设计约束:
//   - 零 Windows/GTK/Qt 污染: 只用 C++ 标准类型 (std::string, bool, uint32_t)
//   - UTF-8 字符串: URL / JS / 路径全 UTF-8, 由各后端内部做平台编码转换
//   - 同步语义: Init / Navigate / WaitForNavigation / ExecuteScript 全部阻塞返回
//   - ScriptResult 类型独立定义在本文件 (被 WebViewSession 和 CdpBrowserSession 共用)
//
// 后端实现:
//   - WebViewSession     : 当前 Windows WebView2 实现 (已有, 继承后 override)
//   - CdpBrowserSession  : 新增 CDP Chrome 外控实现 (跨平台)
// ============================================================================

#include <string>
#include <cstdint>

namespace github_research {

// JS 脚本执行返回结构 (与现有 webview_session.hpp 保持同字同序, 避免业务层大改)
struct ScriptResult {
    bool success{false};
    std::string data;   // ExecuteScript 返回的 JSON 字符串 (UTF-8)
    std::string error;  // 错误信息
};

// 跨平台浏览器会话纯虚接口
// 所有方法返回 bool = 成功, 参数全 std::string (UTF-8)
class IBrowserSession {
public:
    virtual ~IBrowserSession() = default;

    // 禁止拷贝
    IBrowserSession(const IBrowserSession&) = delete;
    IBrowserSession& operator=(const IBrowserSession&) = delete;

    // 初始化独立浏览器环境
    // userDataDir : 独立缓存/Cookie 目录 (UTF-8, 相对路径或绝对路径)
    // extraArgs   : 额外浏览器启动参数 (UTF-8, 空格分隔), 可为空
    // proxy_url   : 显式代理 URL (如 "http://127.0.0.1:7897"), 空 = 不设置
    virtual bool Init(const std::string& userDataDir,
                      const std::string& extraArgs = "",
                      const std::string& proxy_url = "") = 0;

    // 释放所有资源, 关闭浏览器
    virtual void Destroy() = 0;

    // 页面跳转 (异步导航, 不等加载完成)
    virtual bool Navigate(const std::string& url) = 0;

    // 等待最近一次导航完成
    // timeoutMs : 超时毫秒
    virtual bool WaitForNavigation(uint32_t timeoutMs = 30000) = 0;

    // 同步执行 JS 脚本, 阻塞直到返回结果
    virtual ScriptResult ExecuteScript(const std::string& jsCode,
                                       uint32_t timeoutMs = 90000) = 0;

    // 检测登录/页面状态 (通过注入检测 JS)
    // loginDetectJs 应返回 JSON 字符串或 true/false 可解析文本
    virtual bool CheckLogin(const std::string& loginDetectJs) = 0;

    // 是否已初始化完成且未销毁
    virtual bool IsReady() const = 0;

protected:
    IBrowserSession() = default;
};

} // namespace github_research
