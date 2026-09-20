#include "github_research/semanticscholar_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/academic_resolver.hpp"
#include "github_research/query_preprocessor.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <chrono>

namespace github_research {

namespace {

// ─── S2 HTTP 客户端 + 429 指数退避 + 跨请求 300ms 间隔 ───
std::mutex g_s2_curl_mutex;
std::unique_ptr<CurlHttpClient> g_s2_curl;
std::mutex g_s2_rate_mutex;
std::chrono::steady_clock::time_point g_s2_last_request;

CurlHttpClient& get_s2_curl() {
    std::lock_guard<std::mutex> lk(g_s2_curl_mutex);
    if (!g_s2_curl) {
        g_s2_curl = std::make_unique<CurlHttpClient>("research-mcp-s2/1.0", 30);
        g_s2_curl->initialize();
    }
    return *g_s2_curl;
}

// 跨请求间隔保护 — S2 免费 API 限流非常严格,间隔 300ms 避免 429
static void s2_rate_limit_wait() {
    std::lock_guard<std::mutex> lk(g_s2_rate_mutex);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - g_s2_last_request;
    auto min_interval = std::chrono::milliseconds(300);
    if (elapsed < min_interval) {
        std::this_thread::sleep_for(min_interval - elapsed);
    }
    g_s2_last_request = std::chrono::steady_clock::now();
}

HttpResponse fetch_json(const std::string& url, int max_retries = 3) {
    s2_rate_limit_wait();

    CurlHttpClient& curl = get_s2_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";

    HttpResponse resp = curl.get(url, hdrs);

    // 429 指数退避重试 (S2 免费 API 限流非常紧)
    int attempt = 1;
    while (resp.status_code == 429 && attempt <= max_retries) {
        int backoff_ms = 1000 * (1 << (attempt - 1));  // 1000ms, 2000ms, 4000ms
        std::cerr << "[s2] 429 rate limited, retry " << attempt
                  << "/" << max_retries << " in " << backoff_ms << "ms" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        s2_rate_limit_wait();
        resp = curl.get(url, hdrs);
        ++attempt;
    }

    return resp;
}

// Parse authors array from S2 responses
json parse_authors(const json& obj) {
    json authors = json::array();
    if (obj.contains("authors") && obj["authors"].is_array()) {
        for (auto& a : obj["authors"]) {
            if (!a.is_object()) continue;
            json item;
            item["author_id"] = a.value("authorId", "");
            item["name"] = a.value("name", "");
            authors.push_back(std::move(item));
        }
    }
    return authors;
}

} // anonymous namespace

// ============================================================
// 1. s2_search_papers - api.semanticscholar.org/graph/v1/paper/search
// ============================================================
json ToolS2SearchPapers(const json& args) {
    std::string query;
    int count = 10;
    std::string year;
    if (args.contains("query") && args["query"].is_string()) query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (args.contains("year") && args["year"].is_string()) year = args["year"].get<std::string>();
    if (query.empty()) return McpError("ERROR: [s2] 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string fields = "title,authors,year,externalIds,url,abstract,citationCount";

    // Primary query
    std::string url = "https://api.semanticscholar.org/graph/v1/paper/search?query="
        + UrlEncodeComponent(query) + "&limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(fields);
    if (!year.empty()) url += "&year=" + UrlEncodeComponent(year);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] search HTTP " + std::to_string(resp.status_code));
    }
    json data;
    try { data = json::parse(resp.body); } catch (...) {
        return McpError("ERROR: [s2] search parse failed");
    }
    json results = json::array();
    auto items = data.value("data", json::array());
    for (auto& item : items) {
        json r;
        r["s2_paper_id"] = item.value("paperId", "");
        r["title"] = item.value("title", "");
        r["abstract"] = item.value("abstract", "");
        r["year"] = item.value("year", 0);
        r["citation_count"] = item.value("citationCount", 0);
        r["url"] = item.value("url", "");
        r["authors"] = parse_authors(item);
        r["external_ids"] = item.value("externalIds", json::object());
        results.push_back(std::move(r));
    }

    // ── 公共 query_preprocessor 模糊降级链 (联调 S2 300ms 间隔 + 429 退避) ──
    bool fuzzy_hit = false;
    std::string fuzzy_variant_used;
    std::string fuzzy_source_tag;
    if (results.empty()) {
        auto queue = buildQueryQueue(query, "");
        constexpr int kMaxExtraQueries = 3;
        int extra = 0;
        for (size_t i = 1; i < queue.size() && extra < kMaxExtraQueries; ++i) {
            const auto& qs = queue[i];
            if (qs.variant == query) continue;
            std::cerr << "[s2] trying variant [" << qs.source_tag << "]: " << qs.variant << std::endl;
            HttpResponse v_resp = fetch_json(
                "https://api.semanticscholar.org/graph/v1/paper/search?query="
                + UrlEncodeComponent(qs.variant) + "&limit=" + std::to_string(count)
                + "&fields=" + UrlEncodeComponent(fields)
                + (year.empty() ? "" : "&year=" + UrlEncodeComponent(year))
            );
            extra++;
            if (v_resp.status_code != 200) continue;
            try {
                json v_data = json::parse(v_resp.body);
                json v_results = json::array();
                auto v_items = v_data.value("data", json::array());
                for (auto& item : v_items) {
                    json r;
                    r["s2_paper_id"] = item.value("paperId", "");
                    r["title"] = item.value("title", "");
                    r["abstract"] = item.value("abstract", "");
                    r["year"] = item.value("year", 0);
                    r["citation_count"] = item.value("citationCount", 0);
                    r["url"] = item.value("url", "");
                    r["authors"] = parse_authors(item);
                    r["external_ids"] = item.value("externalIds", json::object());
                    v_results.push_back(std::move(r));
                }
                if (!v_results.empty()) {
                    results = std::move(v_results);
                    fuzzy_hit = true;
                    fuzzy_variant_used = qs.variant;
                    fuzzy_source_tag = qs.source_tag;
                    std::cerr << "[s2] variant hit (" << qs.source_tag << ") → " << results.size() << " results" << std::endl;
                    break;
                }
            } catch (...) { continue; }
        }
    }

    if (results.empty()) {
        return McpError(std::string("ERROR: [s2] search returned no results for query='") + query + "'");
    }

    json payload = {
        {"success", true},
        {"source", "s2_api"},
        {"query", query},
        {"total_returned", results.size()},
        {"results", results}
    };

    // ── 风险控制: 模糊命中 → 降置信度 + 溯源标记 ──
    if (fuzzy_hit) {
        bool is_fuzzy = isFuzzySource(fuzzy_source_tag);
        payload["confidence"] = is_fuzzy ? 0.8 : 0.9;
        payload["_source"] = json::object();
        payload["_source"]["match_type"] = fuzzy_source_tag;
        payload["_source"]["fuzzy_variant"] = fuzzy_variant_used;
        payload["_source"]["fuzzy_source_tag"] = fuzzy_source_tag;
        payload["_source"]["original_query"] = query;
    } else {
        payload["confidence"] = 0.9;
        payload["_source"] = json::object();
        payload["_source"]["match_type"] = "exact";
        payload["_source"]["original_query"] = query;
    }
    return WrapMcpResult(payload);
}

// ============================================================
// 2. s2_get_paper_detail - graph/v1/paper/{paper_id}
// ============================================================
json ToolS2GetPaperDetail(const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (paperId.empty()) return McpError("ERROR: [s2] 'paper_id' parameter is required");

    std::string fields = "title,authors,year,externalIds,url,abstract,citationCount,venue,publicationDate,fieldsOfStudy";
    std::string url = "https://api.semanticscholar.org/graph/v1/paper/"
        + UrlEncodeComponent(paperId) + "?fields=" + UrlEncodeComponent(fields);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] paper not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"paper_id", paperId},
            {"s2_paper_id", data.value("paperId", "")},
            {"title", data.value("title", "")},
            {"abstract", data.value("abstract", "")},
            {"year", data.value("year", 0)},
            {"venue", data.value("venue", "")},
            {"url", data.value("url", "")},
            {"publication_date", data.value("publicationDate", "")},
            {"citation_count", data.value("citationCount", 0)},
            {"authors", parse_authors(data)},
            {"external_ids", data.value("externalIds", json::object())},
            {"fields_of_study", data.value("fieldsOfStudy", json::array())}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] paper parse failed: ") + e.what());
    }
}

// ============================================================
// 3. s2_get_citations - graph/v1/paper/{paper_id}/citations
// ============================================================
json ToolS2GetCitations(const json& args) {
    std::string paperId;
    int count = 20;
    if (args.contains("paper_id") && args["paper_id"].is_string()) paperId = args["paper_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (paperId.empty()) return McpError("ERROR: [s2] 'paper_id' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string fields = "title,year,citationCount,url,authors";
    std::string url = "https://api.semanticscholar.org/graph/v1/paper/"
        + UrlEncodeComponent(paperId) + "/citations?limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(fields);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] citations HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json citations = json::array();
        auto items = data.value("data", json::array());
        for (auto& item : items) {
            json citing = item.value("citingPaper", json::object());
            json r;
            r["title"] = citing.value("title", "");
            r["year"] = citing.value("year", 0);
            r["citation_count"] = citing.value("citationCount", 0);
            r["url"] = citing.value("url", "");
            r["authors"] = parse_authors(citing);
            citations.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"paper_id", paperId},
            {"count", citations.size()},
            {"citations", citations}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] citations parse failed: ") + e.what());
    }
}

// ============================================================
// 4. s2_get_references - graph/v1/paper/{paper_id}/references
// ============================================================
json ToolS2GetReferences(const json& args) {
    std::string paperId;
    int count = 20;
    if (args.contains("paper_id") && args["paper_id"].is_string()) paperId = args["paper_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (paperId.empty()) return McpError("ERROR: [s2] 'paper_id' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string fields = "title,year,url,authors";
    std::string url = "https://api.semanticscholar.org/graph/v1/paper/"
        + UrlEncodeComponent(paperId) + "/references?limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(fields);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] references HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json references = json::array();
        auto items = data.value("data", json::array());
        for (auto& item : items) {
            json cited = item.value("citedPaper", json::object());
            json r;
            r["title"] = cited.value("title", "");
            r["year"] = cited.value("year", 0);
            r["url"] = cited.value("url", "");
            r["authors"] = parse_authors(cited);
            references.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"paper_id", paperId},
            {"count", references.size()},
            {"references", references}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] references parse failed: ") + e.what());
    }
}

// ============================================================
// 5. s2_get_author_papers - graph/v1/author/{author_id}/papers
// ============================================================
json ToolS2GetAuthorPapers(const json& args) {
    std::string authorId;
    int count = 20;
    if (args.contains("author_id") && args["author_id"].is_string()) authorId = args["author_id"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (authorId.empty()) return McpError("ERROR: [s2] 'author_id' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string fields = "name,paperCount,citationCount,hIndex";
    std::string authorUrl = "https://api.semanticscholar.org/graph/v1/author/"
        + UrlEncodeComponent(authorId) + "?fields=" + UrlEncodeComponent(fields);

    HttpResponse aresp = fetch_json(authorUrl);
    json authorData;
    if (aresp.status_code == 200) {
        try { authorData = json::parse(aresp.body); } catch (...) {}
    }

    std::string paperFields = "title,year,citationCount,url";
    std::string papersUrl = "https://api.semanticscholar.org/graph/v1/author/"
        + UrlEncodeComponent(authorId) + "/papers?limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(paperFields);

    HttpResponse resp = fetch_json(papersUrl);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] author papers HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json papers = json::array();
        auto items = data.value("data", json::array());
        for (auto& item : items) {
            json p;
            p["title"] = item.value("title", "");
            p["year"] = item.value("year", 0);
            p["citation_count"] = item.value("citationCount", 0);
            p["url"] = item.value("url", "");
            papers.push_back(std::move(p));
        }
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"author_id", authorId},
            {"author_name", authorData.value("name", "")},
            {"paper_count", authorData.value("paperCount", 0)},
            {"citation_count", authorData.value("citationCount", 0)},
            {"h_index", authorData.value("hIndex", 0)},
            {"count", papers.size()},
            {"papers", papers}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] author papers parse failed: ") + e.what());
    }
}

// ============================================================
// 6. s2_search_author - graph/v1/author/search
// ============================================================
json ToolS2SearchAuthor(const json& args) {
    std::string name;
    int count = 5;
    if (args.contains("name") && args["name"].is_string()) name = args["name"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (name.empty()) return McpError("ERROR: [s2] 'name' parameter is required");
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    std::string fields = "name,paperCount,citationCount,hIndex,affiliations";
    std::string url = "https://api.semanticscholar.org/graph/v1/author/search?query="
        + UrlEncodeComponent(name) + "&limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(fields);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] author search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json authors = json::array();
        auto items = data.value("data", json::array());
        for (auto& a : items) {
            json r;
            r["author_id"] = a.value("authorId", "");
            r["name"] = a.value("name", "");
            r["paper_count"] = a.value("paperCount", 0);
            r["citation_count"] = a.value("citationCount", 0);
            r["h_index"] = a.value("hIndex", 0);
            r["affiliations"] = a.value("affiliations", json::array());
            authors.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"query", name},
            {"count", authors.size()},
            {"authors", authors}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] author search parse failed: ") + e.what());
    }
}

// ============================================================
// 7. ToolS2FetchPaperDetail - 统一五级降级调度器
// ============================================================
json ToolS2FetchPaperDetail(const json& args) {
    std::string paperId = args.value("paper_id", args.value("s2_paper_id", ""));
    std::string title   = args.value("title", "");
    if (paperId.empty() && title.empty())
        return McpError("ERROR: [s2] 'paper_id' or 'title' parameter is required");

    CacheManager& cm = CacheManager::instance();

    // Cache lookup (by paper_id first, then by title)
    std::string cache_key = paperId.empty() ? "s2:title:" + title : "s2:" + paperId;
    if (cm.is_ready()) {
        auto cached = cm.get("s2", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("s2", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("s2", cache_key); }
        }
    }

    // ── 五级降级链路 ──
    PaperResult result = resolvePaper(AcademicSource::S2, args);

    // Build unified output
    json payload = json::object();
    payload["success"] = result.found;
    payload["source"] = "s2_api";
    payload["confidence"] = result.confidence;
    payload["degradation_level"] = degradationLabel(result.level);
    payload["fallback_chain"] = result.fallback_chain;
    payload["is_formal_entity"] = result.found && result.confidence >= 0.8;
    payload["is_candidate"]     = result.found && result.confidence >= 0.6 && result.confidence < 0.8;
    payload["is_clue_only"]     = !result.found || result.confidence < 0.6;
    payload["hard_anchors_ok"]  = hasHardAnchors(result);

    if (result.found) {
        payload["title"] = result.title;
        payload["authors"] = result.authors;
        payload["year"] = result.year;
        payload["abstract"] = result.abstract;
        payload["venue"] = result.venue;
        payload["s2_paper_id"] = result.external_id;
        payload["url"] = result.url;
        payload["citation_count"] = result.citation_count;
        if (!result.fields_of_study.empty())
            payload["fields_of_study"] = result.fields_of_study;

        // ── Cache write ──
        int ttl = result.confidence >= 0.8 ? 72 : 24;
        std::string status = result.confidence >= 0.6 ? "ok" : "partial";
        if (cm.is_ready()) cm.put("s2", cache_key, payload.dump(), "json", ttl, "", status, "");

        // ── Entity write (only for formal + hard anchors ok) ──
        if (cm.is_ready() && result.confidence >= 0.8 && hasHardAnchors(result)) {
            std::string pid = result.external_id.empty() ? paperId : result.external_id;
            std::string eid = cm.register_entity(
                "paper", "s2:" + pid, {result.title}, {"semanticscholar"},
                {{"paper_id", pid}, {"citation_count", result.citation_count},
                 {"year", result.year}, {"venue", result.venue}},
                result.title);
            cm.register_entity_source(eid, "s2_api", pid,
                                      {"title", "abstract", "citation_count"}, result.confidence);
            if (result.citation_count > 0)
                cm.record_metric(eid, "s2_citations", (double)result.citation_count, "s2");
        }
    } else {
        // L5: 线索留存 — 仅记录查询意图
        payload["clue_title"] = title;
        payload["clue_paper_id"] = paperId;
        payload["message"] = "All S2 fallback levels exhausted; paper not found";
        if (cm.is_ready()) cm.put("s2", cache_key, payload.dump(), "json", 6, "", "not_found", "all_levels_exhausted");
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
