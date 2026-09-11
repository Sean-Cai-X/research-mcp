#include "github_research/stackoverflow_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[so]";

// ─── Curl HTTP client singleton ───
std::mutex g_so_curl_mutex;
std::unique_ptr<CurlHttpClient> g_so_curl;

CurlHttpClient& get_so_curl() {
    std::lock_guard<std::mutex> lk(g_so_curl_mutex);
    if (!g_so_curl) {
        g_so_curl = std::make_unique<CurlHttpClient>("research-mcp-so/1.0", 30);
        g_so_curl->initialize();
    }
    return *g_so_curl;
}

HttpResponse fetch_json(const std::string& url) {
    CurlHttpClient& curl = get_so_curl();
    std::map<std::string, std::string> hdrs;
    hdrs["User-Agent"] = "ResearchMCP/1.0";
    hdrs["Accept"] = "application/json";
    return curl.get(url, hdrs);
}

std::string normalize_sort(const std::string& s) {
    if (s == "relevance" || s == "newest" || s == "active" || s == "votes" || s == "creation" || s == "views") return s;
    return "relevance";
}

} // anonymous namespace

// ============================================================
// 1. so_search_questions - api.stackexchange.com/2.3/search
// ============================================================
json ToolSoSearchQuestions(const json& args) {
    if (!args.contains("query") || !args["query"].is_string() || args["query"].get<std::string>().empty()) {
        return McpError("ERROR: 'query' parameter is required");
    }
    std::string query = args["query"].get<std::string>();

    std::string tag;
    if (args.contains("tag") && args["tag"].is_string()) tag = args["tag"].get<std::string>();

    int count = 10;
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string sort = "relevance";
    if (args.contains("sort") && args["sort"].is_string()) sort = normalize_sort(args["sort"].get<std::string>());

    // Stack Exchange filter that includes body + tags + owner
    std::string filter = "!9_bDDx5I0";

    std::string url = "https://api.stackexchange.com/2.3/search?order=desc&sort="
        + sort + "&q=" + UrlEncodeComponent(query)
        + "&site=stackoverflow&page_size=" + std::to_string(count)
        + "&filter=" + filter;
    if (!tag.empty()) url += "&tagged=" + UrlEncodeComponent(tag);

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [so] search HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json questions = json::array();
        auto items = data.value("items", json::array());
        for (auto& item : items) {
            json q;
            q["question_id"] = item.value("question_id", 0);
            q["title"] = item.value("title", "");
            q["score"] = item.value("score", 0);
            q["answer_count"] = item.value("answer_count", 0);
            q["view_count"] = item.value("view_count", 0);
            q["is_answered"] = item.value("is_answered", false);
            q["tags"] = item.value("tags", json::array());
            q["creation_date"] = item.value("creation_date", 0);
            q["link"] = item.value("link", "");
            questions.push_back(std::move(q));
        }
        json payload = {
            {"success", true},
            {"source", "stackexchange_api"},
            {"query", query},
            {"total_returned", questions.size()},
            {"questions", questions}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [so] search parse failed: ") + e.what());
    }
}

// ============================================================
// 2. so_get_question_detail - api.stackexchange.com/2.3/questions/{id}
// ============================================================
json ToolSoGetQuestionDetail(const json& args) {
    long long qid = 0;
    if (args.contains("question_id")) {
        if (args["question_id"].is_number_integer()) qid = args["question_id"].get<long long>();
        else if (args["question_id"].is_string()) {
            try { qid = std::stoll(args["question_id"].get<std::string>()); } catch (...) {}
        }
    }
    if (qid <= 0) return McpError("ERROR: 'question_id' parameter is required (positive integer)");

    std::string filter = "!9_bDDx5I0"; // includes body
    std::string url = "https://api.stackexchange.com/2.3/questions/" + std::to_string(qid)
        + "?site=stackoverflow&filter=" + filter;

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [so] question not found (HTTP " + std::to_string(resp.status_code) + ")");
    }
    try {
        json data = json::parse(resp.body);
        auto items = data.value("items", json::array());
        if (items.empty()) return McpError("ERROR: [so] question " + std::to_string(qid) + " not found");
        auto& item = items[0];
        json payload = {
            {"success", true},
            {"source", "stackexchange_api"},
            {"question_id", item.value("question_id", qid)},
            {"title", item.value("title", "")},
            {"body", item.value("body", "")},
            {"score", item.value("score", 0)},
            {"answer_count", item.value("answer_count", 0)},
            {"view_count", item.value("view_count", 0)},
            {"is_answered", item.value("is_answered", false)},
            {"tags", item.value("tags", json::array())},
            {"creation_date", item.value("creation_date", 0)},
            {"link", item.value("link", "")},
            {"owner", item.value("owner", json::object())}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [so] detail parse failed: ") + e.what());
    }
}

// ============================================================
// 3. so_get_top_answers - api.stackexchange.com/2.3/questions/{id}/answers
// ============================================================
json ToolSoGetTopAnswers(const json& args) {
    long long qid = 0;
    if (args.contains("question_id")) {
        if (args["question_id"].is_number_integer()) qid = args["question_id"].get<long long>();
        else if (args["question_id"].is_string()) {
            try { qid = std::stoll(args["question_id"].get<std::string>()); } catch (...) {}
        }
    }
    if (qid <= 0) return McpError("ERROR: 'question_id' parameter is required");

    int count = 3;
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    std::string filter = "!9_bDDx5I0"; // includes body
    std::string url = "https://api.stackexchange.com/2.3/questions/" + std::to_string(qid)
        + "/answers?order=desc&sort=votes&site=stackoverflow&page_size="
        + std::to_string(count) + "&filter=" + filter;

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [so] answers HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json answers = json::array();
        auto items = data.value("items", json::array());
        for (auto& item : items) {
            json a;
            a["answer_id"] = item.value("answer_id", 0);
            a["body"] = item.value("body", "");
            a["score"] = item.value("score", 0);
            a["is_accepted"] = item.value("is_accepted", false);
            a["creation_date"] = item.value("creation_date", 0);
            a["owner"] = item.value("owner", json::object());
            answers.push_back(std::move(a));
        }
        json payload = {
            {"success", true},
            {"source", "stackexchange_api"},
            {"question_id", qid},
            {"count", answers.size()},
            {"answers", answers}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [so] answers parse failed: ") + e.what());
    }
}

// ============================================================
// 4. so_search_by_tags - api.stackexchange.com/2.3/questions?tagged=
// ============================================================
json ToolSoSearchByTags(const json& args) {
    std::string tags;
    if (args.contains("tags") && args["tags"].is_string()) tags = args["tags"].get<std::string>();
    if (tags.empty()) return McpError("ERROR: 'tags' parameter is required (semicolon-separated)");

    int count = 10;
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    // Convert "python;pandas" → "python;pandas" (SE uses ; or space)
    std::string url = std::string("https://api.stackexchange.com/2.3/questions?order=desc&sort=votes&site=stackoverflow")
        + "&tagged=" + UrlEncodeComponent(tags)
        + "&page_size=" + std::to_string(count)
        + "&filter=!9_bDDx5I0";

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [so] tags HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json questions = json::array();
        auto items = data.value("items", json::array());
        for (auto& item : items) {
            json q;
            q["question_id"] = item.value("question_id", 0);
            q["title"] = item.value("title", "");
            q["score"] = item.value("score", 0);
            q["answer_count"] = item.value("answer_count", 0);
            q["view_count"] = item.value("view_count", 0);
            q["tags"] = item.value("tags", json::array());
            q["link"] = item.value("link", "");
            questions.push_back(std::move(q));
        }
        json payload = {
            {"success", true},
            {"source", "stackexchange_api"},
            {"tags", tags},
            {"count", questions.size()},
            {"questions", questions}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [so] tags parse failed: ") + e.what());
    }
}

// ============================================================
// 5. so_get_similar - api.stackexchange.com/2.3/questions/{id}/similar
// ============================================================
json ToolSoGetSimilar(const json& args) {
    std::string title;
    if (args.contains("title") && args["title"].is_string()) title = args["title"].get<std::string>();
    if (title.empty()) return McpError("ERROR: 'title' parameter is required");

    int count = 5;
    if (args.contains("count") && args["count"].is_number_integer()) count = args["count"].get<int>();
    if (count < 1) count = 1;
    if (count > 30) count = 30;

    // Use search with title as query (SE doesn't have a true "similar by title" API)
    std::string url = "https://api.stackexchange.com/2.3/search?order=desc&sort=relevance&q="
        + UrlEncodeComponent(title) + "&site=stackoverflow&page_size=" + std::to_string(count)
        + "&filter=!9_bDDx5I0";

    HttpResponse resp = fetch_json(url);
    if (resp.status_code != 200) {
        return McpError("ERROR: [so] similar HTTP " + std::to_string(resp.status_code));
    }
    try {
        json data = json::parse(resp.body);
        json results = json::array();
        auto items = data.value("items", json::array());
        for (auto& item : items) {
            json r;
            r["question_id"] = item.value("question_id", 0);
            r["title"] = item.value("title", "");
            r["score"] = item.value("score", 0);
            r["answer_count"] = item.value("answer_count", 0);
            r["link"] = item.value("link", "");
            results.push_back(std::move(r));
        }
        json payload = {
            {"success", true},
            {"source", "stackexchange_api"},
            {"query", title},
            {"count", results.size()},
            {"questions", results}
        };
        return WrapMcpResult(payload);
    } catch (const std::exception& e) {
        return McpError(std::string("ERROR: [so] similar parse failed: ") + e.what());
    }
}

// ============================================================
// 6. ToolSoFetchQuestionDetail - layered with cache + entity_mapper
// ============================================================
json ToolSoFetchQuestionDetail(const json& args) {
    std::string questionId;
    if (args.contains("question_id")) {
        if (args["question_id"].is_string()) questionId = args["question_id"].get<std::string>();
        else if (args["question_id"].is_number_integer()) questionId = std::to_string(args["question_id"].get<int>());
    }
    if (questionId.empty()) return McpError("ERROR: [so] 'question_id' parameter is required");
    for (char c : questionId) if (c < '0' || c > '9') return McpError("ERROR: [so] 'question_id' must be numeric");

    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "so:question:" + questionId;
    if (cm.is_ready()) {
        auto cached = cm.get("so", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("so", cache_key)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cp);
                }
            } catch (...) { cm.invalidate("so", cache_key); }
        }
    }

    json rawArgs = json::object();
    rawArgs["question_id"] = std::stoll(questionId);
    json detail = ToolSoGetQuestionDetail(rawArgs);

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
        if (cm.is_ready()) cm.put("so", cache_key, inner.dump(), "json", 24, "", "ok", "");
        if (cm.is_ready()) {
            std::string title = inner.value("title", "");
            int score = inner.value("score", 0);
            std::string eid = cm.register_entity(
                "question", "so:" + questionId, {title}, {"stackoverflow"},
                {{"question_id", questionId}, {"score", score}}, title);
            cm.register_entity_source(eid, "so_api", questionId, {"title", "score", "tags"}, 0.9);
            cm.record_metric(eid, "so_score", (double)score, "so");
        }
    }
    return WrapMcpResult(inner);
}

} // namespace github_research
