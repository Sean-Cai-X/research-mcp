#include "github_research/stackoverflow_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <string>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[so]";

// 将模板中的 __COUNT__ 占位符替换为 count
std::string InjectCount(const std::string& tpl, int count) {
    std::string s = tpl;
    const std::string ph = "__COUNT__";
    size_t pos = s.find(ph);
    if (pos != std::string::npos) {
        s.replace(pos, ph.size(), std::to_string(count));
    }
    return s;
}

// 校验 sort 取值,非法时回退为 relevance
std::string NormalizeSort(const std::string& s) {
    if (s == "relevance" || s == "newest" || s == "active" || s == "votes") {
        return s;
    }
    return "relevance";
}

// 分号分隔的 tags -> "+tag1+tag2"(每个 tag 已 URL 编码)
// 例如 "python;pandas" -> "+python+pandas"
std::string BuildTaggedPath(const std::string& tags) {
    std::string out;
    size_t start = 0;
    while (start <= tags.size()) {
        size_t sep = tags.find(';', start);
        std::string t;
        if (sep == std::string::npos) {
            t = tags.substr(start);
            start = tags.size() + 1;
        } else {
            t = tags.substr(start, sep - start);
            start = sep + 1;
        }
        size_t a = t.find_first_not_of(" \t");
        size_t b = t.find_last_not_of(" \t");
        if (a == std::string::npos) continue; // 空段跳过
        t = t.substr(a, b - a + 1);
        out += "+" + UrlEncodeComponent(t);
    }
    return out;
}

// ============== JS 脚本 ==============
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 所有工具统一使用 webview_helpers.hpp 中的 kJsExtractRawPage

} // anonymous namespace

// ============================================================
// 1. so_search_questions
// ============================================================
json ToolSoSearchQuestions(IBrowserSession& session, const json& args) {
    if (!args.contains("query") || !args["query"].is_string() ||
        args["query"].get<std::string>().empty()) {
        return McpError("ERROR: 'query' parameter is required");
    }
    std::string query = args["query"].get<std::string>();

    std::string tag;
    if (args.contains("tag") && args["tag"].is_string()) {
        tag = args["tag"].get<std::string>();
    }

    int count = 10;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string sort = "relevance";
    if (args.contains("sort") && args["sort"].is_string()) {
        sort = NormalizeSort(args["sort"].get<std::string>());
    }

    std::string urlStr = "https://stackoverflow.com/search?q=" +
                         UrlEncodeComponent(query) + "&sort=" + sort;
    if (!tag.empty()) {
        urlStr += "&tagged=" + UrlEncodeComponent(tag);
    }

    std::string url = urlStr;
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, kLogPrefix, 2500);
}

// ============================================================
// 2. so_get_question_detail
// ============================================================
json ToolSoGetQuestionDetail(IBrowserSession& session, const json& args) {
    if (!args.contains("question_id") || !args["question_id"].is_number_integer()) {
        return McpError("ERROR: 'question_id' parameter is required");
    }
    long long qid = args["question_id"].get<long long>();
    if (qid <= 0) {
        return McpError("ERROR: 'question_id' must be a positive integer");
    }

    std::string url =
        "https://stackoverflow.com/questions/" + std::to_string(qid);
    // 统一返回原始页面文本,解析交给 AI
    return NavigateAndExecute(session, url, kJsExtractRawPage, kLogPrefix, 2500);
}

// ============================================================
// 3. so_get_top_answers
// ============================================================
json ToolSoGetTopAnswers(IBrowserSession& session, const json& args) {
    if (!args.contains("question_id") || !args["question_id"].is_number_integer()) {
        return McpError("ERROR: 'question_id' parameter is required");
    }
    long long qid = args["question_id"].get<long long>();
    if (qid <= 0) {
        return McpError("ERROR: 'question_id' must be a positive integer");
    }

    int count = 3;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    std::string url =
        "https://stackoverflow.com/questions/" + std::to_string(qid) +
        "?answertab=votes";
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, kLogPrefix, 2500);
}

// ============================================================
// 4. so_search_by_tags
// ============================================================
json ToolSoSearchByTags(IBrowserSession& session, const json& args) {
    if (!args.contains("tags") || !args["tags"].is_string() ||
        args["tags"].get<std::string>().empty()) {
        return McpError("ERROR: 'tags' parameter is required");
    }
    std::string tags = args["tags"].get<std::string>();

    int count = 10;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string taggedPath = BuildTaggedPath(tags);
    if (taggedPath.empty()) {
        return McpError("ERROR: 'tags' must contain at least one non-empty tag");
    }

    std::string url = 
        "https://stackoverflow.com/questions/tagged" + taggedPath;
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, kLogPrefix, 2500);
}

// ============================================================
// 5. so_get_similar
// ============================================================
json ToolSoGetSimilar(IBrowserSession& session, const json& args) {
    if (!args.contains("title") || !args["title"].is_string() ||
        args["title"].get<std::string>().empty()) {
        return McpError("ERROR: 'title' parameter is required");
    }
    std::string title = args["title"].get<std::string>();

    int count = 5;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 30) count = 30;

    std::string url =
        "https://stackoverflow.com/search?q=" + UrlEncodeComponent(title) +
        "&sort=relevance";
    // 统一返回原始页面文本,解析交给 AI
    (void)count;
    return NavigateAndExecute(session, url, kJsExtractRawPage, kLogPrefix, 2500);
}

// ============================================================
// 6. ToolSoFetchQuestionDetail - 分层工具: 缓存 + entity_mapper
//    args: question_id (int|string)
//    cache_key: so:question:{id}, TTL=24h
//    entity: question 实体 + tagged_with(tag) 关系 + score 时间快照
// ============================================================
json ToolSoFetchQuestionDetail(IBrowserSession& session, const json& args) {
    std::string questionId;
    if (args.contains("question_id")) {
        if (args["question_id"].is_string()) {
            questionId = args["question_id"].get<std::string>();
        } else if (args["question_id"].is_number_integer()) {
            questionId = std::to_string(args["question_id"].get<int>());
        }
    }
    if (questionId.empty()) {
        return McpError("ERROR: [so] 'question_id' parameter is required");
    }
    // 校验为纯数字
    for (char c : questionId) {
        if (c < '0' || c > '9') {
            return McpError("ERROR: [so] 'question_id' must be numeric");
        }
    }

    // ── 缓存查询: so:question:{id} (TTL=24h) ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "so:question:" + questionId;
    if (cm.is_ready()) {
        auto cached = cm.get("so", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("so", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("so", cache_key);
            }
        }
    }

    std::string url = "https://stackoverflow.com/questions/" + questionId;
    json raw = NavigateAndExecuteRaw(session, url, kJsExtractRawPage,
                                      kLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        if (cm.is_ready()) {
            cm.put("so", cache_key, "", "json", 1, "", "failed", "so question page fetch failed");
        }
        return McpError(std::string("ERROR: [so] failed to fetch question=") + questionId);
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
        size_t pos = title.find(" - Stack Overflow");
        if (pos != std::string::npos) title = title.substr(0, pos);
    }

    json payload = {
        {"success", true},
        {"question_id", questionId},
        {"title", title},
        {"page_url", url},
        {"page_title", pageTitle},
        {"raw_text", pageText}
    };

    if (cm.is_ready()) {
        cm.put("so", cache_key, payload.dump(), "json", 24, "", "ok", "");
    }

    // entity_mapper: question 实体
    if (cm.is_ready() && !questionId.empty()) {
        std::string q_eid = cm.register_entity(
            "question",
            "so:" + questionId,  // canonical_name,带 so: 前缀
            {title},              // aliases
            {"stackoverflow"},    // tags
            {{"question_id", questionId},
             {"page_url", url}},
            title
        );
        cm.register_entity_source(q_eid, "so_web", questionId,
                                  {"title", "score", "view_count", "tags"}, 0.85);
        cm.record_metric(q_eid, "so_observed", 1.0, "so");
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
