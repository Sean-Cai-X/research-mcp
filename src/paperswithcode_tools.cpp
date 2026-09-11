#include "github_research/paperswithcode_tools.hpp"
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
std::mutex g_pwc_curl_mutex;
std::unique_ptr<CurlHttpClient> g_pwc_curl;

CurlHttpClient& get_pwc_curl() {
    std::lock_guard<std::mutex> lk(g_pwc_curl_mutex);
    if (!g_pwc_curl) {
        g_pwc_curl = std::make_unique<CurlHttpClient>("research-mcp-pwc/1.0", 30);
        g_pwc_curl->initialize();
    }
    return *g_pwc_curl;
}

HttpResponse fetch_json(const std::string& url) {
    CurlHttpClient& curl = get_pwc_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";
    return curl.get(url, hdrs);
}

} // anonymous namespace

// ============================================================
// 1. pwc_search_papers - paperswithcode.com/api/v1/search/
// ============================================================
json ToolPwcSearchPapers(const json& args) {
    std::string query;
    int count = 10;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();
    if (query.empty()) return McpError("ERROR: [pwc] 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string url = "https://paperswithcode.com/api/v1/search/?q="
        + UrlEncodeComponent(query) + "&page_size=" + std::to_string(count);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pwc] search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json results = json::array();
        auto items = data.value("results", json::array());
        for (auto& item : items) {
            if (!item.is_object()) continue;
            json r;
            r["paper_id"] = item.value("paper_id", item.value("id", ""));
            r["title"] = item.value("title", "");
            r["abstract"] = item.value("abstract", "");
            r["url_abs"] = item.value("url_abs", "");
            r["published_date"] = item.value("published_date", "");
            r["conference"] = item.value("conference", "");
            // authors
            auto authors = item.value("authors", json::array());
            json authorNames = json::array();
            for (auto& a : authors) authorNames.push_back(a.value("name", ""));
            r["authors"] = authorNames;
            results.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "pwc_api"},
            {"query", query},
            {"total_returned", results.size()},
            {"results", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pwc] parse failed: ") + e.what());
    }
}

// ============================================================
// 2. pwc_get_paper_detail - api/v1/papers/{paper_id}/
// ============================================================
json ToolPwcGetPaperDetail(const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (paperId.empty()) return McpError("ERROR: [pwc] 'paper_id' parameter is required");

    std::string url = "https://paperswithcode.com/api/v1/papers/" + UrlEncodeComponent(paperId) + "/";
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pwc] paper not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json payload = {
            {"success", true},
            {"paper_id", paperId},
            {"title", data.value("title", "")},
            {"abstract", data.value("abstract", "")},
            {"url_pdf", data.value("url_pdf", "")},
            {"url_abs", data.value("url_abs", "")},
            {"published_date", data.value("published_date", "")},
            {"conference", data.value("conference", "")},
            {"tasks", data.value("tasks", json::array())},
            {"methods", data.value("methods", json::array())},
            {"results", data.value("results", json::array())}
        };
        // authors
        auto authors = data.value("authors", json::array());
        json authorNames = json::array();
        for (auto& a : authors) authorNames.push_back(a.value("name", ""));
        payload["authors"] = authorNames;
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pwc] parse failed: ") + e.what());
    }
}

// ============================================================
// 3. pwc_get_sota - api/v1/sota/{task_id}/results/
// ============================================================
json ToolPwcGetSota(const json& args) {
    std::string task;
    int count = 20;
    if (args.contains("task") && args["task"].is_string())
        task = args["task"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();
    if (task.empty()) return McpError("ERROR: [pwc] 'task' parameter is required");
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    std::string taskId = task;
    // Convert "Image Classification" → "image-classification" slug-like
    for (char& c : taskId) if (c == ' ') c = '-';
    std::string url = "https://paperswithcode.com/api/v1/sota/" + UrlEncodeComponent(taskId) + "/results/";
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pwc] sota not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        json results = json::array();
        auto items = data.value("results", json::array());
        int n = 0;
        for (auto& item : items) {
            if (n >= count) break;
            json r;
            r["rank"] = item.value("rank", 0);
            r["model"] = item.value("model", "");
            r["paper_id"] = item.value("paper", json::object()).value("id", "");
            r["metrics"] = item.value("metrics", json::object());
            results.push_back(std::move(r));
            ++n;
        }
        json payload = {
            {"success", true},
            {"source", "pwc_api"},
            {"task", task},
            {"total_returned", results.size()},
            {"results", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pwc] sota parse failed: ") + e.what());
    }
}

// ============================================================
// 4. pwc_search_tasks - api/v1/tasks/?search={q}
// ============================================================
json ToolPwcSearchTasks(const json& args) {
    std::string query;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (query.empty()) return McpError("ERROR: [pwc] 'query' parameter is required");

    std::string url = "https://paperswithcode.com/api/v1/tasks/?search=" + UrlEncodeComponent(query);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pwc] tasks search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json results = json::array();
        auto items = data.value("results", json::array());
        for (auto& item : items) {
            json r;
            r["task_id"] = item.value("id", "");
            r["name"] = item.value("name", "");
            r["description"] = item.value("description", "");
            r["paper_count"] = item.value("paper_count", 0);
            results.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "pwc_api"},
            {"query", query},
            {"total_returned", results.size()},
            {"results", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pwc] tasks parse failed: ") + e.what());
    }
}

// ============================================================
// 5. pwc_search_datasets - api/v1/datasets/?search={q}
// ============================================================
json ToolPwcSearchDatasets(const json& args) {
    std::string query;
    int count = 10;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("count") && args["count"].is_number_integer())
        count = args["count"].get<int>();
    if (query.empty()) return McpError("ERROR: [pwc] 'query' parameter is required");
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string url = "https://paperswithcode.com/api/v1/datasets/?search="
        + UrlEncodeComponent(query) + "&page_size=" + std::to_string(count);
    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [pwc] datasets search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json results = json::array();
        auto items = data.value("results", json::array());
        for (auto& item : items) {
            json r;
            r["dataset_id"] = item.value("id", "");
            r["name"] = item.value("name", "");
            r["description"] = item.value("description", "");
            results.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "pwc_api"},
            {"query", query},
            {"total_returned", results.size()},
            {"results", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [pwc] datasets parse failed: ") + e.what());
    }
}

// ============================================================
// 6. ToolPwcFetchPaperDetail - layered tool with cache
// ============================================================
json ToolPwcFetchPaperDetail(const json& args) {
    std::string paperId;
    if (args.contains("paper_id") && args["paper_id"].is_string())
        paperId = args["paper_id"].get<std::string>();
    if (paperId.empty()) return McpError("ERROR: [pwc] 'paper_id' parameter is required");

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "pwc:" + paperId;
    if (cm.is_ready()) {
        auto cached = cm.get("pwc", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("pwc", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("pwc", cache_key); }
        }
    }

    // Use API for detail
    json rawArgs = json::object();
    rawArgs["paper_id"] = paperId;
    json detail = ToolPwcGetPaperDetail(rawArgs);

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
        if (cm.is_ready()) cm.put("pwc", cache_key, inner.dump(), "json", 72, "", "ok", "");
        if (cm.is_ready()) {
            std::string title = inner.value("title", "");
            std::string paper_eid = cm.register_entity(
                "paper", "pwc:" + paperId, {title}, {"paperswithcode"},
                {{"paper_id", paperId}}, title);
            cm.register_entity_source(paper_eid, "pwc_api", paperId,
                                      {"title", "abstract", "tasks"}, 0.9);
            cm.record_metric(paper_eid, "pwc_observed", 1.0, "pwc");
        }
    }
    return WrapMcpResult(inner);
}

} // namespace github_research
