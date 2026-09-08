#include "github_research/semanticscholar_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <string>

namespace github_research {

namespace {

// ==================== 内置 JS 脚本 ====================
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 所有工具统一使用 webview_helpers.hpp 中的 kJsExtractRawPage

} // anonymous namespace

// ============================================================
// 1. s2_search_papers - 论文检索
// args: query (string) / count (int, default 10) / year (string, optional, e.g. "2020-2024")
// ============================================================
json ToolS2SearchPapers(IBrowserSession& session, const json& args) {
    std::string query;
    int count = 10;
    std::string year;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();
    if (args.contains("year") && args["year"].is_string())
        year = args["year"].get<std::string>();

    if (query.empty()) {
        return McpError("ERROR: [s2] 'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string encoded = UrlEncodeComponent(query);
    std::string url = "https://www.semanticscholar.org/search?q=" + encoded + "&sort=relevance";
    if (!year.empty()) {
        url += "&year=" + UrlEncodeComponent(year);
    }
    // count 仅作为 JS 内截断上限提示,JS 默认截断 30,这里不强制
    (void)count;

    std::wcout << L"[s2] search papers: " << to_wstring(query) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 2500, 45000);
}

// ============================================================
// 2. s2_get_paper_detail - 论文详情
// args: paper_id (string) - 支持 DOI / corpus ID / arXiv ID
// ============================================================
json ToolS2GetPaperDetail(IBrowserSession& session, const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();

    if (paperId.empty()) {
        return McpError("ERROR: [s2] 'paper_id' parameter is required");
    }

    std::string url = "https://www.semanticscholar.org/paper/" + UrlEncodeComponent(paperId);
    std::wcout << L"[s2] get paper detail: " << to_wstring(paperId) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 2500, 45000);
}

// ============================================================
// 3. s2_get_citations - 获取引用该论文的论文列表
// args: paper_id (string) / count (int, default 20)
// ============================================================
json ToolS2GetCitations(IBrowserSession& session, const json& args) {
    std::string paperId;
    int count = 20;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (paperId.empty()) {
        return McpError("ERROR: [s2] 'paper_id' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;
    (void)count;

    std::string url = "https://www.semanticscholar.org/paper/" +
                      UrlEncodeComponent(paperId) + "#cited-papers";
    std::wcout << L"[s2] get citations: " << to_wstring(paperId) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 3000, 45000);
}

// ============================================================
// 4. s2_get_references - 获取该论文引用的参考文献列表
// args: paper_id (string) / count (int, default 20)
// ============================================================
json ToolS2GetReferences(IBrowserSession& session, const json& args) {
    std::string paperId;
    int count = 20;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (paperId.empty()) {
        return McpError("ERROR: [s2] 'paper_id' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;
    (void)count;

    std::string url = "https://www.semanticscholar.org/paper/" +
                      UrlEncodeComponent(paperId) + "#references";
    std::wcout << L"[s2] get references: " << to_wstring(paperId) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 3000, 45000);
}

// ============================================================
// 5. s2_get_author_papers - 获取作者论文列表及作者元信息
// args: author_id (string) / count (int, default 20)
// ============================================================
json ToolS2GetAuthorPapers(IBrowserSession& session, const json& args) {
    std::string authorId;
    int count = 20;
    if (args.contains("author_id") && args["author_id"].is_string())
        authorId = args["author_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (authorId.empty()) {
        return McpError("ERROR: [s2] 'author_id' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;
    (void)count;

    std::string url = "https://www.semanticscholar.org/author/" + UrlEncodeComponent(authorId);
    std::wcout << L"[s2] get author papers: " << to_wstring(authorId) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 3000, 45000);
}

// ============================================================
// 6. s2_search_author - 作者检索
// args: name (string) / count (int, default 5)
// ============================================================
json ToolS2SearchAuthor(IBrowserSession& session, const json& args) {
    std::string name;
    int count = 5;
    if (args.contains("name") && args["name"].is_string())
        name = args["name"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();

    if (name.empty()) {
        return McpError("ERROR: [s2] 'name' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 20) count = 20;
    (void)count;

    std::string encoded = UrlEncodeComponent(name);
    std::string url = "https://www.semanticscholar.org/search?q=" + encoded +
                      "&sort=relevance&type=author";
    std::wcout << L"[s2] search author: " << to_wstring(name) << std::endl;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[s2]", 2500, 45000);
}

// ============================================================
// 7. ToolS2FetchPaperDetail - 分层工具: 缓存 + entity_mapper
//    args: paper_id (string) - 支持 DOI / corpus ID / arXiv ID
//    cache_key: s2:{paper_id}, TTL=72h
//    entity: paper 实体 + cites/reference 关系 + citations 时间快照
// ============================================================
json ToolS2FetchPaperDetail(IBrowserSession& session, const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string()) {
        paperId = args["paper_id"].get<std::string>();
    }
    if (paperId.empty()) {
        return McpError("ERROR: [s2] 'paper_id' parameter is required");
    }

    // ── 缓存查询: s2:{paper_id} (TTL=72h) ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "s2:" + paperId;
    if (cm.is_ready()) {
        auto cached = cm.get("s2", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("s2", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("s2", cache_key);
            }
        }
    }

    std::string url = "https://www.semanticscholar.org/paper/" + UrlEncodeComponent(paperId);
    std::wcout << L"[s2] fetch paper detail: " << to_wstring(paperId) << std::endl;
    json raw = NavigateAndExecuteRaw(session, url, kJsExtractRawPage,
                                      "[s2]", 2500, 45000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("s2", cache_key, "", "json", 1, "", "failed", "s2 paper page fetch failed");
        }
        return McpError(std::string("ERROR: [s2] failed to fetch paper=") + paperId);
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

    std::string title = pageTitle;
    {
        size_t pos = title.find(" | Semantic Scholar");
        if (pos != std::string::npos) title = title.substr(0, pos);
    }

    json payload = {
        {"success", true},
        {"paper_id", paperId},
        {"title", title},
        {"page_url", url},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    if (cm.is_ready()) {
        cm.put("s2", cache_key, payload.dump(), "json", 72, "", "ok", "");
    }

    // entity_mapper: paper 实体(canonical_name 加 s2: 前缀,与 arxiv/pwc 区分)
    if (cm.is_ready() && !paperId.empty()) {
        std::string paper_eid = cm.register_entity(
            "paper",
            "s2:" + paperId,
            {title},
            {"semanticscholar"},
            {{"paper_id", paperId},
             {"page_url", url}},
            title
        );
        cm.register_entity_source(paper_eid, "s2_web", paperId,
                                  {"title", "abstract", "citations", "references"}, 0.9);
        cm.record_metric(paper_eid, "s2_citations_observed", 1.0, "s2");
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
