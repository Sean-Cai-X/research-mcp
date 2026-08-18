#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

namespace github_research {

json McpError(const std::string& msg) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", msg}}})},
        {"isError", true}
    };
}

json McpSuccess(const json& payload) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", payload.dump()}}})},
        {"isError", false}
    };
}

json WrapMcpResult(const json& payload) {
    bool isError = false;
    if (payload.is_object()) {
        // success=false 或有 error 字段时标记 isError
        if (payload.contains("success") && payload["success"].is_boolean()) {
            isError = !payload["success"].get<bool>();
        } else if (payload.contains("error") && payload["error"].is_string()) {
            isError = true;
        }
    }
    return {
        {"content", json::array({{{"type", "text"}, {"text", payload.dump()}}})},
        {"isError", isError}
    };
}

json NavigateAndExecute(WebViewSession& session,
                        const std::wstring& url,
                        const std::string& js,
                        const char* logPrefix,
                        int waitMs,
                        uint32_t navTimeoutMs) {
    // 委托给 NavigateAndExecuteRaw(已含完整 UTF-8 清洗 + 双编码 inner 解析),
    // 避免重复维护两份解析逻辑。失败时 raw 返回 nullptr。
    json raw = NavigateAndExecuteRaw(session, url, js, logPrefix, waitMs, navTimeoutMs);
    if (raw.is_null()) {
        return McpError(std::string("ERROR: ") + logPrefix +
                        " fetch failed (navigation/JS/parse error, see stderr for details)");
    }
    return WrapMcpResult(raw);
}

// 与 NavigateAndExecute 相同流程,但返回原始 JSON payload(不包装 MCP content)
// 失败时返回 null json
json NavigateAndExecuteRaw(WebViewSession& session,
                           const std::wstring& url,
                           const std::string& js,
                           const char* logPrefix,
                           int waitMs,
                           uint32_t navTimeoutMs) {
    auto t0 = std::chrono::steady_clock::now();
    auto dbg_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    {
        std::string url_narrow;
        url_narrow.reserve(url.size());
        for (wchar_t wc : url) { url_narrow.push_back(static_cast<char>(wc & 0xFF)); }
        std::cerr << logPrefix << " [dbg] NavigateAndExecuteRaw START url="
                  << url_narrow
                  << " waitMs=" << waitMs << " navTimeout=" << navTimeoutMs << std::endl;
    }

    if (!session.IsReady()) {
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms session not ready" << std::endl;
        return nullptr;
    }
    std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms session ready, calling Navigate" << std::endl;

    HRESULT hr = session.Navigate(url);
    if (FAILED(hr)) {
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms Navigate failed: 0x" << std::hex << hr << std::endl;
        return nullptr;
    }
    std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms Navigate OK, waiting for nav completion" << std::endl;

    HRESULT navRes = session.WaitForNavigation(navTimeoutMs);
    if (FAILED(navRes)) {
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms WaitForNavigation TIMEOUT/FAIL, still attempt read" << std::endl;
    } else {
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms WaitForNavigation OK" << std::endl;
        if (waitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms sleep(" << waitMs << ") done" << std::endl;
        }
    }

    std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms calling ExecuteScript" << std::endl;
    ScriptResult sr = session.ExecuteScript(js);
    if (!sr.success) {
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms ExecuteScript FAILED: " << sr.error << std::endl;
        return nullptr;
    }
    std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms ExecuteScript OK, data_len=" << sr.data.size() << std::endl;

    // 第一次尝试直接解析
    try {
        json parsed = json::parse(sr.data);
        if (parsed.is_string()) {
            // 双编码 JSON:外层是字符串,其内容本身还可能含非法 UTF-8 字节,
            // 需要再次经过 UTF-8 清洗后解析,避免 inner parse 抛异常
            std::string inner = parsed.get<std::string>();
            // UTF-8 清洗(与下方 catch 分支相同逻辑)
            std::string inner_cleaned;
            inner_cleaned.reserve(inner.size());
            size_t j = 0;
            while (j < inner.size()) {
                unsigned char c2 = static_cast<unsigned char>(inner[j]);
                if (c2 < 0x80) { inner_cleaned.push_back(static_cast<char>(c2)); ++j; }
                else if (c2 >= 0xC2 && c2 <= 0xDF && j + 1 < inner.size()) {
                    unsigned char c3 = static_cast<unsigned char>(inner[j+1]);
                    if (c3 >= 0x80 && c3 <= 0xBF) { inner_cleaned.push_back(static_cast<char>(c2)); inner_cleaned.push_back(static_cast<char>(c3)); j += 2; }
                    else { inner_cleaned.push_back('?'); ++j; }
                } else if (c2 >= 0xE0 && c2 <= 0xEF && j + 2 < inner.size()) {
                    unsigned char c3 = static_cast<unsigned char>(inner[j+1]);
                    unsigned char c4 = static_cast<unsigned char>(inner[j+2]);
                    if (c3 >= 0x80 && c3 <= 0xBF && c4 >= 0x80 && c4 <= 0xBF) { inner_cleaned.push_back(static_cast<char>(c2)); inner_cleaned.push_back(static_cast<char>(c3)); inner_cleaned.push_back(static_cast<char>(c4)); j += 3; }
                    else { inner_cleaned.push_back('?'); ++j; }
                } else if (c2 >= 0xF0 && c2 <= 0xF4 && j + 3 < inner.size()) {
                    unsigned char c3 = static_cast<unsigned char>(inner[j+1]);
                    unsigned char c4 = static_cast<unsigned char>(inner[j+2]);
                    unsigned char c5 = static_cast<unsigned char>(inner[j+3]);
                    if (c3 >= 0x80 && c3 <= 0xBF && c4 >= 0x80 && c4 <= 0xBF && c5 >= 0x80 && c5 <= 0xBF) { inner_cleaned.push_back(static_cast<char>(c2)); inner_cleaned.push_back(static_cast<char>(c3)); inner_cleaned.push_back(static_cast<char>(c4)); inner_cleaned.push_back(static_cast<char>(c5)); j += 4; }
                    else { inner_cleaned.push_back('?'); ++j; }
                } else { inner_cleaned.push_back('?'); ++j; }
            }
            try {
                parsed = json::parse(inner_cleaned);
                if (parsed.is_string()) {
                    // 三重编码罕见,再清洗一次
                    std::string inner2 = parsed.get<std::string>();
                    std::string inner2_cleaned;
                    inner2_cleaned.reserve(inner2.size());
                    size_t j2 = 0;
                    while (j2 < inner2.size()) {
                        unsigned char cc = static_cast<unsigned char>(inner2[j2]);
                        if (cc < 0x80) { inner2_cleaned.push_back(static_cast<char>(cc)); ++j2; }
                        else { inner2_cleaned.push_back('?'); ++j2; }
                    }
                    parsed = json::parse(inner2_cleaned);
                }
            } catch (const std::exception& ei) {
                std::string raw2 = inner.substr(0, 300);
                std::cerr << logPrefix << " [dbg] +" << dbg_ms()
                          << "ms inner-parse after UTF-8 cleanup failed: " << ei.what()
                          << " inner_raw=" << raw2 << std::endl;
                throw;
            }
        }
        std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms parse OK, returning" << std::endl;
        return parsed;
    } catch (const std::exception& /*e*/) {
        // 第一次解析失败(常见原因:页面 JS 提取的 text 中包含语义非法 UTF-8:
        // 如 surrogate 范围 ED A0 80..ED BF BF、超长编码等,简单字节级 cleaning
        // 无法挡住这些情况,nlohmann json 会严格校验并抛异常。
        // 因此这里走 aggressive ASCII-only cleaning: 所有非 ASCII 字节一律替换为 '?',
        // 保证最终 JSON 一定是合法 UTF-8 从而能被解析,文本内容的可读性不会受损。)
        std::string cleaned;
        cleaned.reserve(sr.data.size());
        for (size_t i = 0; i < sr.data.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(sr.data[i]);
            if (c < 0x80) {
                cleaned.push_back(static_cast<char>(c));
            } else {
                cleaned.push_back('?');
            }
        }

        // 用清洗后的字符串重试
        try {
            json parsed = json::parse(cleaned);
            if (parsed.is_string()) {
                // 同样:双编码内部再 UTF-8 清洗后解析
                std::string inner = parsed.get<std::string>();
                std::string inner_cleaned;
                inner_cleaned.reserve(inner.size());
                size_t j = 0;
                while (j < inner.size()) {
                    unsigned char cc = static_cast<unsigned char>(inner[j]);
                    if (cc < 0x80) { inner_cleaned.push_back(static_cast<char>(cc)); ++j; }
                    else { inner_cleaned.push_back('?'); ++j; }
                }
                parsed = json::parse(inner_cleaned);
            }
            std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms JSON parse recovered after UTF-8 cleanup" << std::endl;
            return parsed;
        } catch (const std::exception& e2) {
            std::string raw = sr.data.substr(0, 300);
            std::cerr << logPrefix << " [dbg] +" << dbg_ms() << "ms JSON parse failed even after cleanup: " << e2.what()
                      << " raw=" << raw << std::endl;
            return nullptr;
        }
    }
}

std::string UrlEncodeComponent(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << static_cast<char>(c);
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

} // namespace github_research
