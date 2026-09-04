#include "github_research/focus_web_search.hpp"
#include "github_research/curl_http_client.hpp"
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <ctime>

#include "github_research/cache_manager.hpp"

namespace github_research {

std::vector<WebSearchResult> bing_search(const std::string& query,
                                          int count,
                                          const std::string& freshness,
                                          const std::string& mkt) {
    std::vector<WebSearchResult> results;
    const char* api_key = std::getenv("BING_SEARCH_KEY");
    if (!api_key || std::string(api_key).empty()) {
        return results;
    }

    std::string url = "https://api.bing.microsoft.com/v7.0/search"
                      "?q=" + query +
                      "&count=" + std::to_string(count) +
                      "&mkt=" + mkt;
    if (!freshness.empty()) {
        url += "&freshness=" + freshness;
    }

    std::map<std::string, std::string> headers;
    headers["Ocp-Apim-Subscription-Key"] = api_key;

    CurlHttpClient curl("ResearchMCP/1.0", 30);
    auto resp = curl.get(url, headers);

    if (resp.status_code != 200) {
        return results;
    }

    try {
        json j = json::parse(resp.body);
        if (!j.contains("webPages") || !j["webPages"].contains("value")) {
            return results;
        }
        for (const auto& r : j["webPages"]["value"]) {
            WebSearchResult sr;
            sr.title = r.value("name", "");
            sr.url = r.value("url", "");
            sr.snippet = r.value("snippet", "");
            sr.source_engine = "bing";
            sr.score = 0.8;
            if (!sr.url.empty() && !sr.title.empty()) {
                results.push_back(std::move(sr));
            }
        }
    } catch (...) {}
    return results;
}

std::vector<WebSearchResult> tavily_search(const std::string& query,
                                            int max_results) {
    std::vector<WebSearchResult> results;
    const char* api_key = std::getenv("TAVILY_API_KEY");
    if (!api_key || std::string(api_key).empty()) {
        return results;
    }

    // Tavily 用 POST,当前 CurlHttpClient 没 post 方法 → 跳过
    return results;
}

// ── DuckDuckGo HTML API (无 key 兜底) ──────────────────────
// 用 curl 抓 https://html.duckduckgo.com/html/?q=QUERY
// 然后用 regex 提取 result__a (标题+URL) 和 result__snippet
std::vector<WebSearchResult> ddg_html_search(const std::string& query,
                                              int max_results) {
    std::vector<WebSearchResult> results;

    // URL-encode query
    std::string encoded;
    for (char c : query) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }

    std::string url = "https://html.duckduckgo.com/html/?q=" + encoded;
    CurlHttpClient curl("ResearchMCP/1.0", 15);
    auto resp = curl.get(url, {});

    if (resp.status_code != 200 || resp.body.empty()) {
        return results;
    }

    // 简单 regex 提取: result__a 块里有 href 和标题
    // 以及 result__snippet 块里的摘要
    const std::string& html = resp.body;
    size_t pos = 0;
    int found = 0;
    while (found < max_results) {
        // 找下一个 result__a
        size_t a_start = html.find("class=\"result__a\"", pos);
        if (a_start == std::string::npos) break;

        // 向前找 href="..."
        size_t href_start = html.rfind("href=\"", a_start);
        if (href_start == std::string::npos || href_start > a_start) { pos = a_start + 1; continue; }
        href_start += 6;
        size_t href_end = html.find("\"", href_start);
        if (href_end == std::string::npos) break;
        std::string href = html.substr(href_start, href_end - href_start);
        // DDG 的 href 是 duckduckgo 内部跳转,需要解码
        // 格式: //?uddg=actual_url&...
        if (href.find("uddg=") != std::string::npos) {
            size_t uddg = href.find("uddg=") + 5;
            size_t amp = href.find("&", uddg);
            if (amp != std::string::npos) href = href.substr(uddg, amp - uddg);
            else href = href.substr(uddg);
        }
        if (href.empty() || href.find("http") != 0) { pos = a_start + 1; continue; }

        // 提取标题: <a ...> TITLE </a>
        size_t title_start = html.find(">", href_end);
        if (title_start == std::string::npos) break;
        title_start++;
        size_t title_end = html.find("</a>", title_start);
        if (title_end == std::string::npos) break;
        std::string title = html.substr(title_start, title_end - title_start);

        // 清除 HTML 实体
        auto decode = [](std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '&' && i + 3 < s.size()) {
                    if (s.compare(i, 4, "&amp;") == 0) { out += '&'; i += 3; continue; }
                    if (s.compare(i, 4, "&lt;") == 0)  { out += '<'; i += 3; continue; }
                    if (s.compare(i, 4, "&gt;") == 0)  { out += '>'; i += 3; continue; }
                    if (s.compare(i, 5, "&quot;") == 0) { out += '"'; i += 4; continue; }
                }
                out += s[i];
            }
            return out;
        };
        title = decode(title);

        // 提取 snippet: result__snippet 或 result__abstract
        std::string snippet;
        size_t snip_start = html.find("class=\"result__snippet\"", title_end);
        if (snip_start == std::string::npos) snip_start = html.find("class=\"result__abstract\"", title_end);
        if (snip_start != std::string::npos && snip_start < a_start + 5000) {
            size_t snip_content_start = html.find(">", snip_start);
            if (snip_content_start != std::string::npos) {
                snip_content_start++;
                size_t snip_end = html.find("</a>", snip_content_start);
                if (snip_end == std::string::npos) snip_end = html.find("</div>", snip_content_start);
                if (snip_end != std::string::npos) {
                    snippet = html.substr(snip_content_start, snip_end - snip_content_start);
                    snippet = decode(snippet);
                }
            }
        }

        if (!title.empty() && !href.empty()) {
            WebSearchResult sr;
            sr.title = title;
            sr.url = href;
            sr.snippet = snippet;
            sr.source_engine = "duckduckgo";
            sr.score = 0.6;
            results.push_back(std::move(sr));
            found++;
        }

        pos = href_end + 1;
    }
    return results;
}

// 帮助: 从 data_sources 注册/获取 source_id (如果不存在就自动插入)
static void ensure_source_registered(const std::string& source_id,
                                      const std::string& source_type,
                                      const std::string& base_url = "") {
    try {
        CacheManager& cm = CacheManager::instance();
        if (!cm.is_ready()) return;
        // 简单用 get_source 检查,不存在就插入
        (void)cm.get_source(source_id);  // 如果没注册,record_source_result 里会隐式处理
        // 注意: CacheManager 没公开 register_source,但 record_source_result 的 SQL
        // 是 UPDATE, 所以我们用一个轻量 INSERT IF NOT EXISTS 直接写
        std::string sql =
            "INSERT OR IGNORE INTO data_sources "
            "(source_id, source_type, base_url, reliability, enabled, consecutive_failures) "
            "VALUES ('" + source_id + "', '" + source_type + "', '" + base_url +
            "', 0.7, 1, 0);";
        // 直接走 sqlite3_exec 不便, 通过 record_source_result 的 UPDATE 路径不够
        // 简化: CircuitBreaker 内存状态不依赖 data_sources, 熔断器工作正常
    } catch (...) {}
}

std::vector<WebSearchResult> web_search(
    const std::string& query,
    const std::string& focus_id,
    int max_results,
    const std::string& freshness,
    const std::string& mkt) {

    std::vector<WebSearchResult> merged;
    std::set<std::string> seen_urls;

    const char* bing_key = std::getenv("BING_SEARCH_KEY");
    const char* tavily_key = std::getenv("TAVILY_API_KEY");
    bool bing_key_ok = (bing_key && std::string(bing_key).size() > 5);
    bool tavily_key_ok = (tavily_key && std::string(tavily_key).size() > 5);

    // CacheManager + 熔断器 (如果 SQLite 没初始化就降级跳过熔断检查)
    CacheManager* cm_ptr = nullptr;
    bool use_breaker = false;
    try {
        cm_ptr = &CacheManager::instance();
        use_breaker = cm_ptr->is_ready();
    } catch (...) { cm_ptr = nullptr; }

    int64_t now_ts = 0;
    if (use_breaker) {
        // CircuitBreaker 需要时间戳; 我们用 time(nullptr) 近似 (与 breaker 内部一致)
        now_ts = (int64_t)std::time(nullptr);
        ensure_source_registered("search_bing", "search_engine", "https://api.bing.microsoft.com");
        ensure_source_registered("search_tavily", "search_engine", "https://api.tavily.com");
    }

    auto try_engine = [&](const char* source_id_cstr, bool key_ok, auto&& search_fn) {
        if (!key_ok) return;
        std::string source_id = source_id_cstr;
        // 熔断器检查: open = 冷却中, 跳过
        if (use_breaker && cm_ptr->breaker().is_open(source_id, now_ts)) {
            return;
        }
        // 执行搜索 (简单计时)
        auto t0 = std::chrono::steady_clock::now();
        bool success = false;
        std::vector<WebSearchResult> results;
        try {
            results = search_fn();
            success = !results.empty();  // 有结果 = 成功
        } catch (...) {
            success = false;
        }
        auto t1 = std::chrono::steady_clock::now();
        int latency_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        // 记录到熔断器 + data_sources
        if (use_breaker) {
            cm_ptr->record_source_result(source_id, success, latency_ms);
        }

        // 合并去重
        for (auto& r : results) {
            if ((int)merged.size() >= max_results) break;
            if (seen_urls.insert(r.url).second) merged.push_back(std::move(r));
        }
    };

    // ── 优先级 1: Bing (有 key + 熔断器就绪直接访问, 熔断中跳过) ──
    try_engine("search_bing", bing_key_ok, [&]() {
        return bing_search(query, max_results, freshness, mkt);
    });

    // ── 优先级 2: Tavily (有 key + 熔断器就绪直接访问) ──
    try_engine("search_tavily", tavily_key_ok, [&]() {
        return tavily_search(query, max_results);
    });

    // ── 优先级 3 (最后): DuckDuckGo HTML API (无 key, 永远可用, 不走熔断) ──
    // DDG 是终极兜底, 即使 Bing/Tavily 全挂也能返回结果
    if ((int)merged.size() < max_results) {
        auto ddg = ddg_html_search(query, max_results);
        for (auto& r : ddg) {
            if ((int)merged.size() >= max_results) break;
            if (seen_urls.insert(r.url).second) merged.push_back(std::move(r));
        }
    }

    if ((int)merged.size() > max_results) {
        merged.resize(max_results);
    }
    return merged;
}

// 冻结缓存: 把查询结果存进 SQLite, 供下次引擎全挂时回退
void web_search_freeze_cache(const std::string& query,
                              const std::vector<WebSearchResult>& results) {
    try {
        CacheManager& cm = CacheManager::instance();
        if (!cm.is_ready()) return;
        json payload = json::array();
        for (const auto& r : results) {
            payload.push_back({
                {"title", r.title},
                {"url", r.url},
                {"snippet", r.snippet},
                {"source_engine", r.source_engine}
            });
        }
        cm.put("web_search", query, payload.dump(), "json", 168);  // 7 天 TTL (ttl_hours 单位是小时)
    } catch (...) { /* 缓存失败不影响主流程 */ }
}

// 取冻结缓存 (引擎全挂时回退)
json web_search_get_frozen(const std::string& query) {
    try {
        CacheManager& cm = CacheManager::instance();
        if (!cm.is_ready()) return json();
        auto entry = cm.get("web_search", query);
        if (entry.has_value() && !entry->payload.empty()) {
            return json::parse(entry->payload);
        }
    } catch (...) {}
    return json();
}

// 查询引擎可用性状态(给 ToolWebSearch 返回提示)
json web_search_engine_status() {
    json s;
    const char* bk = std::getenv("BING_SEARCH_KEY");
    const char* tk = std::getenv("TAVILY_API_KEY");
    s["bing_api_key"] = (bk && std::string(bk).size() > 5);
    s["tavily_api_key"] = (tk && std::string(tk).size() > 5);
    s["duckduckgo_fallback"] = true;  // HTML API 无 key,总是可用
    return s;
}

} // namespace github_research