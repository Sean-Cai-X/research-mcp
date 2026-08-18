#pragma once

// WebViewSession 公共辅助函数
// 所有 API 数据源工具集共用:导航 + 等待 + 执行 JS + 解析结果
// 统一错误处理范式、统一日志前缀、统一 MCP 返回格式

#include <string>
#include <nlohmann/json.hpp>
#include "webview_session.hpp"

namespace github_research {

using json = nlohmann::json;

// 统一 MCP 返回格式封装
// payload: 已构造的 JSON 对象(含 success / data / error 等业务字段)
// 返回: {content:[{type:text,text:...}], isError:bool}
json WrapMcpResult(const json& payload);

// 导航 + 等待 + 执行 JS + 解析结果
// session:    已 Init 的 WebViewSession 引用
// url:        目标页面 URL
// js:         要执行的 JS 脚本(返回 JSON 字符串)
// logPrefix:  日志前缀(如 "[hn]" "[pwc]" "[hf]")
// waitMs:     导航完成后额外等待 DOM 渲染的毫秒数
// navTimeoutMs: 导航超时
json NavigateAndExecute(WebViewSession& session,
                        const std::wstring& url,
                        const std::string& js,
                        const char* logPrefix = "",
                        int waitMs = 2000,
                        uint32_t navTimeoutMs = 30000);

// 与 NavigateAndExecute 相同流程,但返回解析后的原始 JSON payload(不包装 MCP content)
// 用于需要多次导航后合并结构化结果的场景(如 hn_fetch_detailed_story)
// 失败时返回 null json(is_null() == true)
json NavigateAndExecuteRaw(WebViewSession& session,
                           const std::wstring& url,
                           const std::string& js,
                           const char* logPrefix = "",
                           int waitMs = 2000,
                           uint32_t navTimeoutMs = 30000);

// 快速返回错误(MCP 格式)
json McpError(const std::string& msg);

// 快速返回成功(MCP 格式,纯数据无需 WebView)
json McpSuccess(const json& payload);

// URL 编码
std::string UrlEncodeComponent(const std::string& str);

// ============== 统一原始文本提取 JS ==============
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 所有数据源搜索/详情类工具统一调用此 JS,返回原始页面文本
// 返回: {success:bool, url:string, title:string, text:string}
//   - text: document.body.innerText(去标签的纯文本,AI 可直接读)
//   - text 截断到 50000 字符避免超大返回
// 注: 2026-08-18 起移除 html 字段:
//   1) 其典型大小 50KB(约为 text 的 10~20 倍),通过 Streamable HTTP + 代理转发时
//      极易触发代理网关"Could not establish connection / payload too large / read timeout"
//      等与网络链路相关的偶发错误(HF 5 并发场景尤其明显)
//   2) 实际代码路径中没有任何一处消费 html 字段(全部工具都只用 text/title/url)
constexpr const char* kJsExtractRawPage = R"(
(function(){
    var text = "";
    try { text = document.body ? document.body.innerText : ""; } catch(e) { text = ""; }
    if(text && text.length > 50000) text = text.substring(0, 50000);
    return JSON.stringify({
        success: true,
        url: window.location.href,
        title: document.title || "",
        text: text
    });
})();
)";

} // namespace github_research
