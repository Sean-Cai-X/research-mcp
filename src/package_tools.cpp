#include "github_research/package_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <string>

namespace github_research {

namespace {

// ============== 内置 JS 脚本 ==============
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 所有工具统一使用 webview_helpers.hpp 中的 kJsExtractRawPage

// ============== 参数读取辅助 ==============
std::string read_string(const json& args, const std::string& key) {
    if (args.contains(key) && args[key].is_string()) {
        return args[key].get<std::string>();
    }
    return "";
}

int read_count(const json& args, const std::string& key, int default_val) {
    if (args.contains(key) && args[key].is_number_integer()) {
        int n = args[key].get<int>();
        if (n < 1) return 1;
        if (n > 50) return 50;
        return n;
    }
    return default_val;
}

} // anonymous namespace

// ============================================================
// 1. pkg_search_npm
// ============================================================
json ToolPkgSearchNpm(IBrowserSession& session, const json& args) {
    std::string query = read_string(args, "query");
    if (query.empty()) {
        return McpError("ERROR: [pkg] 'query' parameter is required");
    }
    int count = read_count(args, "count", 10);
    (void)count;  // 统一返回原始页面文本,解析交给 AI

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://www.npmjs.com/search?q=" + encoded;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pkg]", 2500, 30000);
}

// ============================================================
// 2. pkg_get_npm_detail
// ============================================================
json ToolPkgGetNpmDetail(IBrowserSession& session, const json& args) {
    std::string name = read_string(args, "name");
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    std::string url = "https://www.npmjs.com/package/" + name;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pkg]", 2500, 30000);
}

// ============================================================
// 3. pkg_search_pypi
// ============================================================
json ToolPkgSearchPypi(IBrowserSession& session, const json& args) {
    std::string query = read_string(args, "query");
    if (query.empty()) {
        return McpError("ERROR: [pkg] 'query' parameter is required");
    }
    int count = read_count(args, "count", 10);
    (void)count;  // 统一返回原始页面文本,解析交给 AI

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://pypi.org/search/?q=" + encoded;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pkg]", 2500, 30000);
}

// ============================================================
// 4. pkg_get_pypi_detail
// ============================================================
json ToolPkgGetPypiDetail(IBrowserSession& session, const json& args) {
    std::string name = read_string(args, "name");
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    std::string url = "https://pypi.org/project/" + name + "/";
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pkg]", 2500, 30000);
}

// ============================================================
// 5. ToolPkgFetchDetail - 分层工具: 缓存 + entity_mapper
//    args: registry (string, "npm"|"pypi"), name (string)
//    cache_key: pkg:{registry}:{name}, TTL=24h
//    entity: package 实体 + authored_by(owner) 关系 + weekly_downloads 时间快照
// ============================================================
json ToolPkgFetchDetail(IBrowserSession& session, const json& args) {
    std::string registry = read_string(args, "registry");
    std::string name = read_string(args, "name");
    if (registry.empty()) {
        registry = "npm";  // 默认 npm
    }
    if (registry != "npm" && registry != "pypi") {
        return McpError("ERROR: [pkg] 'registry' must be 'npm' or 'pypi'");
    }
    if (name.empty()) {
        return McpError("ERROR: [pkg] 'name' parameter is required");
    }

    // ── 缓存查询: pkg:{registry}:{name} (TTL=24h) ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "pkg:" + registry + ":" + name;
    if (cm.is_ready()) {
        auto cached = cm.get("pkg", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("pkg", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("pkg", cache_key);
            }
        }
    }

    // ── 网络抓取 ──
    std::string url_str;
    if (registry == "npm") {
        url_str = "https://www.npmjs.com/package/" + name;
    } else {
        url_str = "https://pypi.org/project/" + name + "/";
    }
    json raw = NavigateAndExecuteRaw(session, url_str,
                                      kJsExtractRawPage, "[pkg]", 2500, 30000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("pkg", cache_key, "", "json", 1, "", "failed", "package page fetch failed");
        }
        return McpError(std::string("ERROR: [pkg] failed to fetch ") + registry + " package=" + name);
    }

    std::string pageText;
    std::string pageTitle;
    if (raw.is_object()) {
        if (raw.contains("text") && raw["text"].is_string()) {
            pageText = raw["text"].get<std::string>();
        }
        if (raw.contains("title") && raw["title"].is_string()) {
            pageTitle = raw["title"].get<std::string>();
        }
    }
    if (pageText.size() > 50000) pageText = pageText.substr(0, 50000);

    // ── 简单字段提取(启发式,从页面文本中抽取) ──
    // 原始文本保留给 AI,这里只抽取易识别的字段
    std::string version;
    std::string description;
    std::string license;
    std::string homepage;

    // version: 查找 "version" 关键字后的数字
    {
        size_t pos = pageText.find("version");
        if (pos != std::string::npos) {
            size_t numStart = pageText.find_first_of("0123456789", pos);
            if (numStart != std::string::npos) {
                size_t numEnd = numStart;
                while (numEnd < pageText.size() &&
                       (pageText[numEnd] == '.' ||
                        (pageText[numEnd] >= '0' && pageText[numEnd] <= '9'))) {
                    ++numEnd;
                }
                if (numEnd > numStart && numEnd - numStart <= 30) {
                    version = pageText.substr(numStart, numEnd - numStart);
                }
            }
        }
    }
    // description: page title 去掉 " - npm" / " - PyPI"
    if (!pageTitle.empty()) {
        description = pageTitle;
        size_t dashPos = description.find(" - npm");
        if (dashPos != std::string::npos) description = description.substr(0, dashPos);
        dashPos = description.find(" - PyPI");
        if (dashPos != std::string::npos) description = description.substr(0, dashPos);
    }

    json payload = {
        {"success", true},
        {"registry", registry},
        {"name", name},
        {"version", version},
        {"description", description},
        {"license", license},
        {"homepage", homepage},
        {"page_url", url_str},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    // ── 缓存写入 ──
    if (cm.is_ready()) {
        cm.put("pkg", cache_key, payload.dump(), "json", 24, "", "ok", "");
    }

    // ── entity_mapper: package 实体 ──
    if (cm.is_ready() && !name.empty()) {
        std::string pkg_eid = cm.register_entity(
            "package",
            registry + ":" + name,  // canonical_name,带 registry 前缀
            {name, pageTitle},       // aliases
            {registry, "package"},   // tags
            {{"registry", registry},
             {"version", version},
             {"license", license},
             {"homepage", homepage},
             {"page_url", url_str}},
            description
        );
        // 注册实体来源(供多源融合使用)
        cm.register_entity_source(pkg_eid, registry == "npm" ? "npm_registry" : "pypi_registry",
                                  name,
                                  {"name", "version", "description", "license"}, 0.85);
        // 时间快照:记录版本号(数值化,失败不影响)
        if (!version.empty()) {
            // version 通常非纯数字,用字符串记录到 attrs,不强行 record_metric
            cm.record_metric(pkg_eid, "version_observed", 1.0, registry);
        }
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
