#include "github_research/semanticscholar_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

// ─── Curl HTTP client singleton ───
std::mutex g_s2_curl_mutex;
std::unique_ptr<CurlHttpClient> g_s2_curl;

CurlHttpClient& get_s2_curl() {
    std::lock_guard<std::mutex> lk(g_s2_curl_mutex);
    if (!g_s2_curl) {
        g_s2_curl = std::make_unique<CurlHttpClient>("research-mcp-s2/1.0", 30);
        g_s2_curl->initialize();
    }
    return *g_s2_curl;
}

HttpResponse fetch_json(const std::string& url) {
    CurlHttpClient& curl = get_s2_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";
    return curl.get(url, hdrs);
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
    std::string url = "https://api.semanticscholar.org/graph/v1/paper/search?query="
        + UrlEncodeComponent(query) + "&limit=" + std::to_string(count)
        + "&fields=" + UrlEncodeComponent(fields);
    if (!year.empty()) url += "&year=" + UrlEncodeComponent(year);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [s2] search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
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
        json payload = {
            {"success", true},
            {"source", "s2_api"},
            {"query", query},
            {"total_returned", results.size()},
            {"results", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [s2] search parse failed: ") + e.what());
    }
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
// 7. ToolS2FetchPaperDetail - layered with cache + entity_mapper
// ============================================================
json ToolS2FetchPaperDetail(const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (paperId.empty()) return McpError("ERROR: [s2] 'paper_id' parameter is required");

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "s2:" + paperId;
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

    json rawArgs = json::object();
    rawArgs["paper_id"] = paperId;
    json detail = ToolS2GetPaperDetail(rawArgs);

    json inner;
    if (detail.contains("content") && detail["content"].is_array() && !detail["content"].empty()) {
        auto& c = detail["content"][0];
        if (c.contains("text")) {
            try { inner = json::parse(c["text"].get<std::string>()); }
            catch (...) {}
        }
    }
    if (!inner.is_object()) inner = {{"success", false}};

    if (inner.value("success", false)) {
        if (cm.is_ready()) cm.put("s2", cache_key, inner.dump(), "json", 72, "", "ok", "");
        if (cm.is_ready()) {
            std::string title = inner.value("title", "");
            int cites = inner.value("citation_count", 0);
            std::string pid = inner.value("s2_paper_id", paperId);
            std::string eid = cm.register_entity(
                "paper", "s2:" + pid.empty() ? paperId : pid, {title}, {"semanticscholar"},
                {{"paper_id", paperId}, {"citation_count", cites}}, title);
            cm.register_entity_source(eid, "s2_api", paperId,
                                      {"title", "abstract", "citation_count"}, 0.95);
            cm.record_metric(eid, "s2_citations", (double)cites, "s2");
        }
    }
    return WrapMcpResult(inner);
}

} // namespace github_research
