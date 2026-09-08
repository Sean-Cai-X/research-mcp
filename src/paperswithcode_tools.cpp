#include "github_research/paperswithcode_tools.hpp"
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

} // anonymous namespace

// ============================================================
// 1. ToolPwcSearchPapers
// args: query (string), count (int, default 10)
// ============================================================
json ToolPwcSearchPapers(IBrowserSession& session, const json& args) {
    std::string query;
    int count = 10;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (query.empty()) {
        return McpError("ERROR: [pwc] 'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://paperswithcode.com/search?q=" + encoded;
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pwc]", 2500);
}

// ============================================================
// 2. ToolPwcGetPaperDetail
// args: paper_id (string)
// ============================================================
json ToolPwcGetPaperDetail(IBrowserSession& session, const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();

    if (paperId.empty()) {
        return McpError("ERROR: [pwc] 'paper_id' parameter is required");
    }

    std::string url = "https://paperswithcode.com/paper/" + paperId;
    // 统一返回原始页面文本,解析交给 AI
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pwc]", 2500);
}

// ============================================================
// 3. ToolPwcGetSota
// args: task (string), count (int, default 20)
// ============================================================
json ToolPwcGetSota(IBrowserSession& session, const json& args) {
    std::string task;
    int count = 20;

    if (args.contains("task") && args["task"].is_string())
        task = args["task"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (task.empty()) {
        return McpError("ERROR: [pwc] 'task' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    // 任务 slug: 空格替换为连字符, 再 URL 编码(连字符属于 unreserved, 保留)
    std::string slug = task;
    for (char& c : slug) {
        if (c == ' ') c = '-';
    }
    slug = UrlEncodeComponent(slug);

    std::string url = "https://paperswithcode.com/sota/task/" + slug;
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pwc]", 2500);
}

// ============================================================
// 4. ToolPwcSearchTasks
// args: query (string)
// ============================================================
json ToolPwcSearchTasks(IBrowserSession& session, const json& args) {
    std::string query;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();

    if (query.empty()) {
        return McpError("ERROR: [pwc] 'query' parameter is required");
    }

    std::string encoded = UrlEncodeComponent(query);
    // 任务检索: 直接访问 /task/ENCODED(slug 形式)
    std::string url = "https://paperswithcode.com/task/" + encoded;
    // 统一返回原始页面文本,解析交给 AI
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pwc]", 2500);
}

// ============================================================
// 5. ToolPwcSearchDatasets
// args: query (string), count (int, default 10)
// ============================================================
json ToolPwcSearchDatasets(IBrowserSession& session, const json& args) {
    std::string query;
    int count = 10;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (query.empty()) {
        return McpError("ERROR: [pwc] 'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://paperswithcode.com/datasets?q=" + encoded;
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[pwc]", 2500);
}

// ============================================================
// 6. ToolPwcFetchPaperDetail - 分层工具: 缓存 + entity_mapper
//    args: paper_id (string)
//    cache_key: pwc:{paper_id}, TTL=72h
//    entity: paper 实体 + evaluated_on(task) 关系 + stars 时间快照
// ============================================================
json ToolPwcFetchPaperDetail(IBrowserSession& session, const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string()) {
        paperId = args["paper_id"].get<std::string>();
    }
    if (paperId.empty()) {
        return McpError("ERROR: [pwc] 'paper_id' parameter is required");
    }

    // ── 缓存查询: pwc:{paper_id} (TTL=72h) ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "pwc:" + paperId;
    if (cm.is_ready()) {
        auto cached = cm.get("pwc", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("pwc", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("pwc", cache_key);
            }
        }
    }

    std::string url = "https://paperswithcode.com/paper/" + paperId;
    json raw = NavigateAndExecuteRaw(session, url, kJsExtractRawPage, "[pwc]", 2500, 30000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("pwc", cache_key, "", "json", 1, "", "failed", "pwc paper page fetch failed");
        }
        return McpError(std::string("ERROR: [pwc] failed to fetch paper=") + paperId);
    }

    std::string pageText, pageTitle;
    if (raw.is_object()) {
        if (raw.contains("text") && raw["text"].is_string()) {
            pageText = raw["text"].get<std::string>();
        }
        if (raw.contains("title") && raw["title"].is_string()) {
            pageTitle = raw["title"].get<std::string>();
        }
    }
    if (pageText.size() > 50000) pageText = pageText.substr(0, 50000);

    // 简单提取:从 title 去掉 " | Papers With Code"
    std::string title = pageTitle;
    {
        size_t pos = title.find(" | Papers With Code");
        if (pos != std::string::npos) title = title.substr(0, pos);
    }

    json payload = {
        {"success", true},
        {"paper_id", paperId},
        {"title", title},
        {"page_url", "https://paperswithcode.com/paper/" + paperId},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    if (cm.is_ready()) {
        cm.put("pwc", cache_key, payload.dump(), "json", 72, "", "ok", "");
    }

    // entity_mapper: paper 实体(与 arxiv paper 共用 paper 类型,canonical_name 加 pwc: 前缀)
    if (cm.is_ready() && !paperId.empty()) {
        std::string paper_eid = cm.register_entity(
            "paper",
            "pwc:" + paperId,  // canonical_name,带 pwc: 前缀避免与 arxiv 冲突
            {title},            // aliases
            {"paperswithcode"}, // tags
            {{"paper_id", paperId},
             {"page_url", "https://paperswithcode.com/paper/" + paperId}},
            title
        );
        cm.register_entity_source(paper_eid, "pwc_web", paperId,
                                  {"title", "abstract", "code_link"}, 0.85);
        // 时间快照:记录 paper 被观测到一次(数值化为 1.0)
        cm.record_metric(paper_eid, "pwc_stars", 1.0, "pwc");
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
