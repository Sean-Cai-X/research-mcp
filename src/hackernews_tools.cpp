#include "github_research/hackernews_tools.hpp"
#include "github_research/webview_helpers.hpp"   // WrapMcpResult, McpError, UrlEncodeComponent
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/hn_firebase.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <string>
#include <ctime>
#include <map>
#include <mutex>
#include <memory>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[hn]";

// Algolia HN API 基础 URL (无认证,免费)
constexpr const char* kAlgoliaBase = "https://hn.algolia.com/api/v1/search";

// 单例 curl client (与 hn_firebase.cpp 独立,但共享同一模式)
std::mutex g_algolia_curl_mutex;
std::unique_ptr<CurlHttpClient> g_algolia_curl;

CurlHttpClient& get_algolia_curl() {
    std::lock_guard<std::mutex> lk(g_algolia_curl_mutex);
    if (!g_algolia_curl) {
        g_algolia_curl = std::make_unique<CurlHttpClient>("research-mcp-hn/1.0", 20);
        g_algolia_curl->initialize();
    }
    return *g_algolia_curl;
}

// ─────────────────────────────────────────────────────────────
// Firebase item → 工具输出字段转换
// ─────────────────────────────────────────────────────────────
json convert_firebase_item_to_story(const json& fb_item, int rank) {
    json story;
    int id = fb_item.value("id", 0);
    story["hn_id"] = std::to_string(id);
    story["rank"] = rank;
    story["title"] = fb_item.value("title", "");
    story["external_url"] = fb_item.value("url", "");
    story["score"] = fb_item.value("score", 0);
    story["author"] = fb_item.value("by", "");

    if (fb_item.contains("time") && fb_item["time"].is_number_integer()) {
        auto now = std::time(nullptr);
        auto t = static_cast<std::time_t>(fb_item["time"].get<int64_t>());
        auto diff_sec = static_cast<long>(now - t);
        std::string age_str;
        if (diff_sec < 0) age_str = "unknown";
        else if (diff_sec < 60) age_str = std::to_string(diff_sec) + "s ago";
        else if (diff_sec < 3600) age_str = std::to_string(diff_sec / 60) + "m ago";
        else if (diff_sec < 86400) age_str = std::to_string(diff_sec / 3600) + "h ago";
        else age_str = std::to_string(diff_sec / 86400) + "d ago";
        story["created_min_ago"] = age_str;
    } else {
        story["created_min_ago"] = "";
    }
    story["comment_count"] = fb_item.value("descendants", 0);
    story["hn_item_url"] = "item?id=" + std::to_string(id);
    return story;
}

// 纯 Firebase 索引获取(完全无 WebView2 依赖)
json fetch_stories(const std::string& source, int count) {
    json ids;
    if (source == "new")       ids = HnFirebase::get_new_ids();
    else if (source == "best") ids = HnFirebase::get_best_ids();
    else                       ids = HnFirebase::get_top_ids();

    if (!ids.is_array() || ids.empty()) {
        std::cerr << "[hn] Firebase IDs fetch failed for source=" << source << std::endl;
        return json::array();
    }

    std::vector<int> id_list;
    id_list.reserve(std::min((int)ids.size(), count));
    for (auto& v : ids) {
        if ((int)id_list.size() >= count) break;
        if (v.is_number_integer()) id_list.push_back(v.get<int>());
    }

    auto items = HnFirebase::get_items_batch(id_list);

    json stories = json::array();
    int rank = 0;
    for (int id : id_list) {
        rank++;
        auto it = items.find(id);
        if (it != items.end() && it->second.is_object()) {
            bool dead = it->second.value("dead", false);
            bool del  = it->second.value("deleted", false);
            if (dead || del) continue;
            stories.push_back(convert_firebase_item_to_story(it->second, rank));
        }
    }
    std::cerr << "[hn] " << source << ": got " << stories.size() << " stories via Firebase" << std::endl;
    return stories;
}

// ─────────────────────────────────────────────────────────────
// Firebase comments 递归(带深度和数量限制)
// 返回 json::array 每层是 {author, text, reply_level, hn_id, time}
// ─────────────────────────────────────────────────────────────
void fetch_comments_recursive(
    const json& kids_ids,
    int depth, int max_depth, int& count, int max_count,
    json& out)
{
    if (depth > max_depth || count >= max_count) return;
    if (!kids_ids.is_array()) return;

    for (auto& kid_id : kids_ids) {
        if (count >= max_count) break;
        if (!kid_id.is_number_integer()) continue;
        int id = kid_id.get<int>();

        json item = HnFirebase::get_item(id);
        if (!item.is_object()) continue;
        bool dead = item.value("dead", false);
        bool del  = item.value("deleted", false);
        if (dead || del) continue;

        json c;
        c["hn_id"]   = id;
        c["author"]  = item.value("by", "");
        c["text"]    = item.value("text", "");  // Firebase 已返回 HTML 片段
        c["reply_level"] = depth;
        c["time"]    = item.value("time", 0);
        if (item.contains("parent")) c["parent"] = item.value("parent", 0);

        out.push_back(std::move(c));
        count++;

        if (item.contains("kids")) {
            fetch_comments_recursive(item["kids"], depth + 1, max_depth, count, max_count, out);
        }
    }
}

// ─────────────────────────────────────────────────────────────
// 简单 HTML → text 提取(用于外部文章正文抓取)
// 不是 Markdown/HTML 解析器,够用就行
// ─────────────────────────────────────────────────────────────
std::string html_to_plaintext(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    bool prev_space = false;

    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; out += ' '; prev_space = true; continue; }
        if (in_tag) continue;

        // HTML 实体简表
        if (c == '&') {
            if (html.compare(i, 4, "&amp;") == 0) { out += '&'; i += 3; prev_space = false; continue; }
            if (html.compare(i, 4, "&lt;") == 0)  { out += '<'; i += 3; continue; }
            if (html.compare(i, 4, "&gt;") == 0)  { out += '>'; i += 3; continue; }
            if (html.compare(i, 6, "&nbsp;") == 0) { out += ' '; i += 5; continue; }
            if (html.compare(i, 8, "&quot;") == 0){ out += '"'; i += 7; continue; }
            if (html.compare(i, 6, "&apos;") == 0){ out += '\''; i += 5; continue; }
        }

        if (c == '\n' || c == '\r' || c == '\t') {
            if (!prev_space) { out += '\n'; prev_space = true; }
            continue;
        }
        if (c == ' ' || c == '\xA0') {
            if (!prev_space) { out += ' '; prev_space = true; }
            continue;
        }
        out += c;
        prev_space = false;
    }
    // 去掉连续空行
    std::string result;
    result.reserve(out.size());
    int blank_lines = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c == '\n') {
            if (blank_lines < 2) result += c;
            blank_lines++;
        } else {
            result += c;
            blank_lines = 0;
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
// Algolia HN 搜索 → 统一 stories 输出格式
// ─────────────────────────────────────────────────────────────
json algolia_search(const std::string& query, int count) {
    std::string url = std::string(kAlgoliaBase)
        + "?query=" + UrlEncodeComponent(query)
        + "&tags=story"
        + "&hitsPerPage=" + std::to_string(count);

    CurlHttpClient& curl = get_algolia_curl();
    HttpResponse resp = curl.get(url);
    if (resp.status_code != 200) {
        std::cerr << "[hn] Algolia search failed: HTTP " << resp.status_code << std::endl;
        return json::array();
    }
    try {
        json parsed = json::parse(resp.body);
        json stories = json::array();
        int rank = 0;
        auto hits = parsed.value("hits", json::array());
        for (auto& h : hits) {
            rank++;
            json story;
            std::string hn_id_str = h.value("objectID", "");
            story["hn_id"]         = hn_id_str;
            story["rank"]          = rank;
            story["title"]         = h.value("title", "");
            story["external_url"]  = h.value("url", "");
            story["score"]         = h.value("points", 0);
            story["author"]        = h.value("author", "");
            story["created_min_ago"] = h.value("created_at", "");  // ISO 8601, AI 能解析
            story["comment_count"] = h.value("num_comments", 0);
            if (!hn_id_str.empty()) {
                story["hn_item_url"] = "item?id=" + hn_id_str;
            }
            stories.push_back(std::move(story));
        }
        std::cerr << "[hn] Algolia search '" << query << "': got " << stories.size() << " hits" << std::endl;
        return stories;
    } catch (const std::exception& e) {
        std::cerr << "[hn] Algolia parse error: " << e.what() << std::endl;
        return json::array();
    }
}

} // anonymous namespace

// ============================================================
// 7 个工具实现 (全纯 HTTP,零 session 依赖)
// ============================================================

// 1. hn_get_top_stories
json ToolHnGetTopStories(const json& args) {
    int count = args.value("count", 20);
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories("top", count);
    if (stories.empty()) {
        return McpError("ERROR: [hn] Firebase API returned empty top stories");
    }
    if ((int)stories.size() > count) stories.erase(stories.begin() + count, stories.end());

    json payload = {
        {"source", "hackernews"},
        {"list_type", "top"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 2. hn_get_new_stories
json ToolHnGetNewStories(const json& args) {
    int count = args.value("count", 20);
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories("new", count);
    if (stories.empty()) {
        return McpError("ERROR: [hn] Firebase API returned empty new stories");
    }
    if ((int)stories.size() > count) stories.erase(stories.begin() + count, stories.end());

    json payload = {
        {"source", "hackernews"},
        {"list_type", "new"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 3. hn_get_best_stories
json ToolHnGetBestStories(const json& args) {
    int count = args.value("count", 20);
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories("best", count);
    if (stories.empty()) {
        return McpError("ERROR: [hn] Firebase API returned empty best stories");
    }
    if ((int)stories.size() > count) stories.erase(stories.begin() + count, stories.end());

    json payload = {
        {"source", "hackernews"},
        {"list_type", "best"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 4. hn_get_item - Firebase + 递归 comments
json ToolHnGetItem(const json& args) {
    int id = 0;
    if (args.contains("id") && args["id"].is_number_integer()) {
        id = args["id"].get<int>();
    } else if (args.contains("id") && args["id"].is_string()) {
        try { id = std::stoi(args["id"].get<std::string>()); } catch (...) { id = 0; }
    }
    if (id <= 0) {
        return McpError("ERROR: [hn] 'id' must be a positive integer");
    }

    int max_depth = args.value("comment_max_depth", 2);
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 5) max_depth = 5;

    int max_comments = args.value("max_comment_count", 60);
    if (max_comments < 1) max_comments = 1;
    if (max_comments > 200) max_comments = 200;

    json item = HnFirebase::get_item(id);
    if (!item.is_object()) {
        return McpError("ERROR: [hn] item " + std::to_string(id) + " not found (Firebase returned null)");
    }

    json payload;
    payload["hn_id"]          = id;
    payload["title"]          = item.value("title", "");
    payload["url"]            = item.value("url", "");   // 外部链接,ask/post 为 null
    payload["hn_item_url"]    = "item?id=" + std::to_string(id);
    payload["author"]         = item.value("by", "");
    payload["score"]          = item.value("score", 0);
    payload["comment_count"]  = item.value("descendants", 0);
    payload["time"]           = item.value("time", 0);
    payload["type"]           = item.value("type", "");  // story / comment / job / poll

    // ask post: text 就是 post 正文
    if (item.contains("text") && item["text"].is_string()) {
        payload["text"] = item["text"];
    }

    // 递归拉 comments
    json comments = json::array();
    int count = 0;
    if (item.contains("kids")) {
        fetch_comments_recursive(item["kids"], 1, max_depth, count, max_comments, comments);
    }
    payload["comments"] = comments;

    return WrapMcpResult(payload);
}

// 5. hn_search_by_keyword - Algolia REST API
json ToolHnSearchByKeyword(const json& args) {
    std::string query;
    int count = 10;

    if (args.contains("query") && args["query"].is_string()) {
        query = args["query"].get<std::string>();
    }
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (query.empty()) {
        return McpError("ERROR: [hn] 'query' is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    json stories = algolia_search(query, count);
    if (stories.empty()) {
        // Algolia 对 very short query 可能返回空,或网络问题
        return McpError("ERROR: [hn] Algolia search returned no results for query='" + query + "'");
    }

    json payload = {
        {"source", "hn_algolia"},
        {"query", query},
        {"count", (int)stories.size()},
        {"results", stories}
    };
    return WrapMcpResult(payload);
}

// 6. hn_get_latest_index - Firebase
json ToolHnGetLatestIndex(const json& args) {
    int limit = args.value("limit", 30);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    std::string source = args.value("source", "front");
    std::string fb_type = "top";
    if (source == "newest") fb_type = "new";
    else if (source == "best") fb_type = "best";

    json stories = fetch_stories(fb_type, limit);
    if (stories.empty()) {
        return McpError("ERROR: [hn] Firebase returned empty index for source=" + source);
    }

    json payload = {
        {"success", true},
        {"source", source},
        {"total_returned", (int)stories.size()},
        {"items", stories}
    };
    return WrapMcpResult(payload);
}

// 7. hn_fetch_detailed_story - Firebase item + comments + 外部文章 curl 抓取
json ToolHnFetchDetailedStory(const json& args) {
    // ── 参数解析 ──
    std::string hn_id;
    if (args.contains("hn_id")) {
        if (args["hn_id"].is_string())       hn_id = args["hn_id"].get<std::string>();
        else if (args["hn_id"].is_number_integer()) hn_id = std::to_string(args["hn_id"].get<int>());
    }
    if (hn_id.empty()) return McpError("ERROR: [hn] 'hn_id' is required");
    for (char c : hn_id) if (c < '0' || c > '9') return McpError("ERROR: [hn] 'hn_id' must be numeric");
    int id = std::stoi(hn_id);

    bool fetch_article = args.value("fetch_external_article", true);
    bool fetch_comments = args.value("fetch_comments", true);
    int max_depth = args.value("comment_max_depth", 2);
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 5) max_depth = 5;
    int max_comments = args.value("max_comment_count", 80);
    if (max_comments < 1) max_comments = 1;
    if (max_comments > 200) max_comments = 200;
    int text_max_chars = args.value("text_max_chars", 20000);
    if (text_max_chars < 1000) text_max_chars = 1000;
    if (text_max_chars > 50000) text_max_chars = 50000;

    // ── 缓存查询(cache_key 包含 fetch 选项,避免 comments false 缓存污染 true 查询) ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "item:" + hn_id
        + ":fc=" + (fetch_comments ? "1" : "0")
        + ":fa=" + (fetch_article ? "1" : "0");
    if (cm.is_ready()) {
        auto cached = cm.get("hn", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hn", cache_key)) {
            try {
                json payload = json::parse(cached->payload);
                if (payload.is_object()) {
                    payload["cache_hit"] = true;
                    return WrapMcpResult(payload);
                }
            } catch (...) { cm.invalidate("hn", cache_key); }
        }
    }

    // ── 步骤 1: Firebase item ──
    json item = HnFirebase::get_item(id);
    if (!item.is_object()) {
        if (cm.is_ready()) cm.put("hn", cache_key, "", "json", 1, "", "failed", "item not found");
        return McpError("ERROR: [hn] item " + hn_id + " not found");
    }

    std::string title = item.value("title", "");
    std::string source_url = item.value("url", "");  // ask post 无 url
    std::string hn_type = item.value("type", "");

    // ── 步骤 2: Firebase comments 递归 ──
    json comments = json::array();
    if (fetch_comments && item.contains("kids")) {
        int count = 0;
        fetch_comments_recursive(item["kids"], 1, max_depth, count, max_comments, comments);
    }

    // ── 步骤 3: 外部文章 curl 抓取 (纯 HTTP GET + HTML→text) ──
    std::string article_text;
    std::string article_status = "skipped";
    bool is_external = !source_url.empty() && source_url.find("news.ycombinator.com") == std::string::npos;

    if (fetch_article && is_external) {
        CurlHttpClient& curl = get_algolia_curl();  // 复用实例
        HttpResponse resp = curl.get(source_url);
        if (resp.status_code == 200 && !resp.body.empty()) {
            std::string text = html_to_plaintext(resp.body);
            if ((int)text.size() > text_max_chars) text = text.substr(0, text_max_chars);
            article_text = text;
            article_status = article_text.empty() ? "no_text" : "ok";
        } else {
            article_status = "fetch_failed";
        }
    } else if (!fetch_article) {
        article_status = "disabled";
    } else if (!is_external) {
        article_status = "no_external_url";
    }

    // ask post 的 text 字段就是正文
    std::string ask_text;
    if (hn_type == "story" && !source_url.empty() && item.contains("text")) {
        ask_text = item["text"].get<std::string>();
    } else if (hn_type == "ask" && item.contains("text")) {
        ask_text = item["text"].get<std::string>();
    }

    // ── 步骤 4: 合并输出 ──
    json payload = {
        {"success", true},
        {"hn_id", hn_id},
        {"title", title},
        {"type", hn_type},
        {"source_url", source_url},
        {"hn_item_url", "item?id=" + hn_id},
        {"author", item.value("by", "")},
        {"score", item.value("score", 0)},
        {"comment_count", item.value("descendants", 0)},
        {"article_plaintext", article_text},
        {"article_fetch_status", article_status},
        {"discussion_comments", comments},
        {"returned_comment_count", (int)comments.size()},
        {"comment_max_depth_applied", max_depth}
    };
    if (!ask_text.empty()) {
        payload["hn_post_text"] = ask_text;
    }

    // ── 缓存写入 ──
    if (cm.is_ready()) cm.put("hn", cache_key, payload.dump(), "json", 12, "", "ok", "");

    // ── entity_mapper (保持原有逻辑,无 session 依赖) ──
    if (cm.is_ready() && !title.empty()) {
        std::string story_eid = cm.register_entity(
            "topic", "hn:" + hn_id, {title}, {"hackernews"},
            {{"source_url", source_url},
             {"comment_count", (int)comments.size()},
             {"hn_id", hn_id}},
            title
        );
        cm.record_metric(story_eid, "comment_count", (double)comments.size(), "hn");

        int idx = 0;
        for (auto& c : comments) {
            if (!c.is_object()) continue;
            std::string author = c.value("author", "");
            std::string text   = c.value("text", "");
            int level          = c.value("reply_level", 1);
            if (text.empty() && author.empty()) continue;

            std::string comment_eid = cm.register_entity(
                "comment", "hn:" + hn_id + "#c" + std::to_string(idx),
                {}, {"hackernews"},
                {{"author", author},
                 {"reply_level", level},
                 {"text_preview", text.size() > 100 ? text.substr(0, 100) : text}},
                ""
            );
            cm.add_relation(comment_eid, story_eid, "discussed_in", 0.8, "hn", std::to_string(id));

            if (!author.empty()) {
                std::string person_eid = cm.register_entity(
                    "person", author, {}, {}, json::object(), author);
                cm.add_relation(comment_eid, person_eid, "authored_by", 1.0, "hn", std::to_string(id));
            }
            if (++idx >= 30) break;
        }

        if (!source_url.empty() && source_url.find("arxiv.org") != std::string::npos) {
            std::string arxiv_id;
            size_t p = source_url.find("/abs/");
            if (p != std::string::npos) {
                arxiv_id = source_url.substr(p + 5);
                for (auto sep : {'v', '?', '/', '#'}) {
                    size_t q = arxiv_id.find(sep);
                    if (q != std::string::npos) arxiv_id = arxiv_id.substr(0, q);
                }
            }
            if (!arxiv_id.empty()) {
                std::string paper_eid = cm.register_entity("paper", arxiv_id, {}, {}, json::object(), "");
                cm.add_relation(story_eid, paper_eid, "mentions", 0.9, "hn", std::to_string(id));
            }
        }
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
