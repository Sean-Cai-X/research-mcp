#include "github_research/hackernews_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/hn_firebase.hpp"
#include <iostream>
#include <string>
#include <ctime>

namespace github_research {

// ============================================================
// 内置 JS 脚本
// ============================================================
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 2 个分层工具使用专用结构化 JS(返回 JSON 数组/对象)

namespace {

constexpr const char* kLogPrefix = "[hn]";

// HN 首页索引提取 JS:解析 tr.athing 行,返回结构化数组
// 字段: hn_id, rank, title, external_url, score, author, created_min_ago, comment_count, hn_item_url
constexpr const char* kJsHnIndexList = R"(
(function(){
  var items = [];
  var rows = document.querySelectorAll('tr.athing');
  for (var i = 0; i < rows.length; i++) {
    var row = rows[i];
    var id = row.getAttribute('id') || '';
    var titleLink = row.querySelector('.titleline > a');
    if (!titleLink) continue;
    var title = titleLink.textContent || '';
    var url = titleLink.href || '';
    var rank = i + 1;
    var rankSpan = row.querySelector('.rank');
    if (rankSpan) {
      var m = rankSpan.textContent.match(/(\d+)/);
      if (m) rank = parseInt(m[1]);
    }
    var score = 0;
    var author = '';
    var age = '';
    var commentCount = 0;
    var hnItemUrl = '';
    var subtext = null;
    var next = row.nextElementSibling;
    while (next) {
      var s = next.querySelector ? next.querySelector('.subtext') : null;
      if (s) { subtext = s; break; }
      if (next.tagName === 'TR' && next.querySelector('tr.athing')) break;
      next = next.nextElementSibling;
    }
    if (subtext) {
      var scoreEl = subtext.querySelector('.score');
      if (scoreEl) {
        var sm = scoreEl.textContent.match(/(\d+)/);
        if (sm) score = parseInt(sm[1]);
      }
      var ageEl = subtext.querySelector('.age');
      if (ageEl) {
        age = ageEl.textContent || '';
      }
      var userEl = subtext.querySelector('.hnuser');
      if (userEl) {
        author = userEl.textContent || '';
      }
      var allLinks = subtext.querySelectorAll('a');
      for (var j = 0; j < allLinks.length; j++) {
        var href = allLinks[j].getAttribute('href') || '';
        if (href.indexOf('item?id=') >= 0) {
          hnItemUrl = href;
          var cm = allLinks[j].textContent.match(/(\d+)/);
          if (cm) commentCount = parseInt(cm[1]);
          break;
        }
      }
    }
    items.push({
      hn_id: id,
      rank: rank,
      title: title,
      external_url: url,
      score: score,
      author: author,
      created_min_ago: age,
      comment_count: commentCount,
      hn_item_url: hnItemUrl
    });
  }
  return JSON.stringify(items);
})();
)";

// HN item 页提取 JS:解析标题/源URL/评论树
// 字段: {title, source_url, comments:[{author,text,reply_level}]}
// 过滤 [dead]/[deleted] 评论
constexpr const char* kJsHnItemDetail = R"(
(function(){
  var titleEl = document.querySelector('.titleline > a');
  if (!titleEl) {
    var titleRow = document.querySelector('tr.athing');
    if (titleRow) titleEl = titleRow.querySelector('a');
  }
  var title = titleEl ? titleEl.textContent : '';
  var sourceUrl = titleEl ? titleEl.href : '';
  var comments = [];
  var rows = document.querySelectorAll('.comtr');
  for (var i = 0; i < rows.length; i++) {
    var r = rows[i];
    var author = '';
    var authorEl = r.querySelector('.hnuser');
    if (authorEl) author = authorEl.textContent;
    var textEl = r.querySelector('.commtext');
    var text = textEl ? textEl.textContent : '';
    if (!text) continue;
    if (text.indexOf('[dead]') >= 0 || text.indexOf('[deleted]') >= 0) continue;
    var indent = 0;
    var img = r.querySelector('img[src*="s.gif"]');
    if (img) {
      var w = parseInt(img.getAttribute('width')) || 0;
      indent = Math.floor(w / 40);
    }
    comments.push({
      author: author,
      text: text,
      reply_level: indent + 1
    });
  }
  return JSON.stringify({
    title: title,
    source_url: sourceUrl,
    comments: comments
  });
})();
)";

} // anonymous namespace

// ============================================================
// Firebase → 工具输出格式 转换层
// ============================================================
// Firebase item JSON 字段名与 WebView2+JS 解析输出不同,
// 这里统一成工具对外的标准字段,保持向后兼容。
static json convert_firebase_item_to_story(const json& fb_item, int rank) {
    json story;
    int id = fb_item.value("id", 0);
    story["hn_id"] = std::to_string(id);
    story["rank"] = rank;
    story["title"] = fb_item.value("title", "");

    std::string url = fb_item.value("url", "");
    story["external_url"] = url;

    story["score"] = fb_item.value("score", 0);
    story["author"] = fb_item.value("by", "");

    if (fb_item.contains("time") && fb_item["time"].is_number_integer()) {
        auto now = std::time(nullptr);
        auto t = static_cast<std::time_t>(fb_item["time"].get<int64_t>());
        auto diff_sec = static_cast<long>(now - t);
        std::string age_str;
        if (diff_sec < 0) age_str = "unknown";
        else if (diff_sec < 60) age_str = std::to_string(diff_sec) + " seconds ago";
        else if (diff_sec < 3600) age_str = std::to_string(diff_sec / 60) + " minutes ago";
        else if (diff_sec < 86400) age_str = std::to_string(diff_sec / 3600) + " hours ago";
        else age_str = std::to_string(diff_sec / 86400) + " days ago";
        story["created_min_ago"] = age_str;
    } else {
        story["created_min_ago"] = "";
    }

    story["comment_count"] = fb_item.value("descendants", 0);
    story["hn_item_url"] = "item?id=" + std::to_string(id);
    return story;
}

// Firebase 优先 + WebView2 兜底
static json fetch_stories_with_fallback(
    WebViewSession& session, const std::string& source, int count) {

    std::cerr << "[hn] attempting Firebase API first (source=" << source << ")" << std::endl;
    json ids;
    if (source == "new")           ids = HnFirebase::get_new_ids();
    else if (source == "best")     ids = HnFirebase::get_best_ids();
    else                           ids = HnFirebase::get_top_ids();

    if (ids.is_array() && !ids.empty()) {
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
                bool del = it->second.value("deleted", false);
                if (dead || del) continue;
                stories.push_back(convert_firebase_item_to_story(it->second, rank));
            }
        }

        std::cerr << "[hn] Firebase API OK: got " << stories.size()
                  << " stories for source=" << source << std::endl;
        return stories;
    }

    // 兜底 WebView2(仅当 session 已初始化时)
    if (session.IsReady()) {
        std::cerr << "[hn] Firebase API failed/empty, falling back to WebView2 scrape" << std::endl;
        std::wstring url;
        if (source == "new")           url = L"https://news.ycombinator.com/newest";
        else if (source == "best")     url = L"https://news.ycombinator.com/best";
        else                           url = L"https://news.ycombinator.com/";

        json raw = NavigateAndExecuteRaw(session, url, kJsHnIndexList, kLogPrefix, 4000, 45000);
        if (raw.is_array()) return raw;
        if (raw.is_string()) {
            try { return json::parse(raw.get<std::string>()); }
            catch (...) {}
        }
    } else {
        std::cerr << "[hn] Firebase API failed and WebView2 session not ready, no fallback available" << std::endl;
    }
    return json::array();
}

// ============================================================
// 工具实现
// ============================================================

// 1. hn_get_topstories
json ToolHnGetTopStories(WebViewSession& session, const json& args) {
    int count = 20;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories_with_fallback(session, "top", count);

    if (stories.empty()) {
        return McpError("ERROR: [hn] failed to fetch top stories (Firebase API + WebView2 both failed)");
    }

    // 按 count 截断(Firebase 返回最多 500,WebView2 返回 ~30)
    if ((int)stories.size() > count) {
        json trimmed = json::array();
        for (int i = 0; i < count && i < (int)stories.size(); ++i) {
            trimmed.push_back(stories[i]);
        }
        stories = std::move(trimmed);
    }

    json payload = {
        {"source", "hackernews"},
        {"list_type", "top"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 2. hn_get_new_stories
json ToolHnGetNewStories(WebViewSession& session, const json& args) {
    int count = 20;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories_with_fallback(session, "new", count);

    if (stories.empty()) {
        return McpError("ERROR: [hn] failed to fetch new stories (Firebase API + WebView2 both failed)");
    }

    if ((int)stories.size() > count) {
        json trimmed = json::array();
        for (int i = 0; i < count && i < (int)stories.size(); ++i) {
            trimmed.push_back(stories[i]);
        }
        stories = std::move(trimmed);
    }

    json payload = {
        {"source", "hackernews"},
        {"list_type", "new"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 3. hn_get_best_stories
json ToolHnGetBestStories(WebViewSession& session, const json& args) {
    int count = 20;
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }
    if (count < 1) count = 1;
    if (count > 100) count = 100;

    json stories = fetch_stories_with_fallback(session, "best", count);

    if (stories.empty()) {
        return McpError("ERROR: [hn] failed to fetch best stories (Firebase API + WebView2 both failed)");
    }

    if ((int)stories.size() > count) {
        json trimmed = json::array();
        for (int i = 0; i < count && i < (int)stories.size(); ++i) {
            trimmed.push_back(stories[i]);
        }
        stories = std::move(trimmed);
    }

    json payload = {
        {"source", "hackernews"},
        {"list_type", "best"},
        {"count", (int)stories.size()},
        {"stories", stories}
    };
    return WrapMcpResult(payload);
}

// 4. hn_get_item
json ToolHnGetItem(WebViewSession& session, const json& args) {
    int id = 0;
    if (args.contains("id") && args["id"].is_number_integer()) {
        id = args["id"].get<int>();
    } else if (args.contains("id") && args["id"].is_string()) {
        try {
            id = std::stoi(args["id"].get<std::string>());
        } catch (...) {
            id = 0;
        }
    }

    if (id <= 0) {
        return McpError("ERROR: [hn] 'id' parameter is required and must be a positive integer");
    }

    std::string urlStr = "https://news.ycombinator.com/item?id=" + std::to_string(id);
    std::wstring url = to_wstring(urlStr);

    return NavigateAndExecute(session, url, kJsExtractRawPage, "[hn]", 2500, 30000);
}

// 5. hn_search_by_keyword
json ToolHnSearchByKeyword(WebViewSession& session, const json& args) {
    std::string query;
    int count = 10;

    if (args.contains("query") && args["query"].is_string()) {
        query = args["query"].get<std::string>();
    }
    if (args.contains("count") && args["count"].is_number_integer()) {
        count = args["count"].get<int>();
    }

    if (query.empty()) {
        return McpError("ERROR: [hn] 'query' parameter is required");
    }
    if (count < 1) count = 1;
    if (count > 50) count = 50;

    std::string encoded = UrlEncodeComponent(query);
    std::string urlStr = "https://hn.algolia.com/?q=" + encoded;
    std::wstring url = to_wstring(urlStr);

    // Algolia 是 React SPA,等待时间稍长
    json result = NavigateAndExecute(session, url, kJsExtractRawPage, "[hn]", 3000, 30000);

    (void)count;
    return result;
}

// ============================================================
// 6. hn_get_latest_index - 轻量索引(结构化,无深度请求)
// ============================================================
// Firebase API 优先,WebView2 兜底
json ToolHnGetLatestIndex(WebViewSession& session, const json& args) {
    int limit = 30;
    if (args.contains("limit") && args["limit"].is_number_integer()) {
        limit = args["limit"].get<int>();
    }
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    std::string source = "front";
    if (args.contains("source") && args["source"].is_string()) {
        source = args["source"].get<std::string>();
    }

    // source → Firebase fetch_stories_with_fallback 的类型名
    std::string fb_type = "top";  // front/top → topstories
    if (source == "newest") fb_type = "new";
    else if (source == "best") fb_type = "best";

    json stories = fetch_stories_with_fallback(session, fb_type, limit);

    if (stories.empty()) {
        return McpError("ERROR: [hn] index extraction failed (Firebase API + WebView2 both failed)");
    }

    json items = json::array();
    int n = 0;
    for (auto& it : stories) {
        if (n >= limit) break;
        items.push_back(it);
        ++n;
    }

    json payload = {
        {"success", true},
        {"source", source},
        {"total_returned", items.size()},
        {"items", items}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 7. hn_fetch_detailed_story - 按 hn_id 深度抓取
// ============================================================
// 流程:
//   1. 导航 HN item 页,提取 title/source_url/comments
//   2. (可选)导航 source_url,用 kJsExtractRawPage 取正文
//   3. C++ 合并结构化结果,按 max_comment_count/comment_max_depth 过滤
json ToolHnFetchDetailedStory(WebViewSession& session, const json& args) {
    // --- 参数解析 ---
    std::string hnId;
    if (args.contains("hn_id")) {
        if (args["hn_id"].is_string()) {
            hnId = args["hn_id"].get<std::string>();
        } else if (args["hn_id"].is_number_integer()) {
            hnId = std::to_string(args["hn_id"].get<int>());
        }
    }
    if (hnId.empty()) {
        return McpError("ERROR: [hn] 'hn_id' parameter is required");
    }
    // 校验 hn_id 为纯数字
    for (char c : hnId) {
        if (c < '0' || c > '9') {
            return McpError("ERROR: [hn] 'hn_id' must be numeric");
        }
    }

    bool fetchArticle = true;
    if (args.contains("fetch_external_article") && args["fetch_external_article"].is_boolean()) {
        fetchArticle = args["fetch_external_article"].get<bool>();
    }
    bool fetchComments = true;
    if (args.contains("fetch_comments") && args["fetch_comments"].is_boolean()) {
        fetchComments = args["fetch_comments"].get<bool>();
    }
    int maxDepth = 2;
    if (args.contains("comment_max_depth") && args["comment_max_depth"].is_number_integer()) {
        maxDepth = args["comment_max_depth"].get<int>();
    }
    if (maxDepth < 1) maxDepth = 1;
    if (maxDepth > 5) maxDepth = 5;
    int maxComments = 80;
    if (args.contains("max_comment_count") && args["max_comment_count"].is_number_integer()) {
        maxComments = args["max_comment_count"].get<int>();
    }
    if (maxComments < 1) maxComments = 1;
    if (maxComments > 200) maxComments = 200;
    int textMaxChars = 20000;
    if (args.contains("text_max_chars") && args["text_max_chars"].is_number_integer()) {
        textMaxChars = args["text_max_chars"].get<int>();
    }
    if (textMaxChars < 1000) textMaxChars = 1000;
    if (textMaxChars > 50000) textMaxChars = 50000;

    // ── 缓存查询:item:{hn_id} (TTL=12h) ──
    // 命中且新鲜时直接返回,避免重复访问 HN
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "item:" + hnId;
    if (cm.is_ready()) {
        auto cached = cm.get("hn", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("hn", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("hn", cache_key);
            }
        }
    }

    // --- 步骤1: 导航 HN item 页,提取 title/source_url/comments ---
    std::string itemUrlStr = "https://news.ycombinator.com/item?id=" + hnId;
    std::wstring itemUrl = to_wstring(itemUrlStr);

    // 注意: 如果不抓取评论,仍可只取 title/source_url;但 JS 一次性返回,无额外开销
    json itemRaw = NavigateAndExecuteRaw(session, itemUrl, kJsHnItemDetail, kLogPrefix, 2500, 30000);
    if (itemRaw.is_null()) {
        // 写入短 TTL 失败缓存
        if (cm.is_ready()) {
            cm.put("hn", cache_key, "", "json", 1, "", "failed", "HN item page fetch failed");
        }
        return McpError(std::string("ERROR: [hn] failed to fetch HN item page for id=") + hnId);
    }

    std::string title;
    std::string sourceUrl;
    json comments = json::array();
    if (itemRaw.is_object()) {
        if (itemRaw.contains("title") && itemRaw["title"].is_string()) {
            title = itemRaw["title"].get<std::string>();
        }
        if (itemRaw.contains("source_url") && itemRaw["source_url"].is_string()) {
            sourceUrl = itemRaw["source_url"].get<std::string>();
        }
        if (fetchComments && itemRaw.contains("comments") && itemRaw["comments"].is_array()) {
            // 按 max_depth 过滤,按 max_count 截断
            int kept = 0;
            for (auto& c : itemRaw["comments"]) {
                if (kept >= maxComments) break;
                int level = 1;
                if (c.contains("reply_level") && c["reply_level"].is_number_integer()) {
                    level = c["reply_level"].get<int>();
                }
                if (level > maxDepth) continue;
                comments.push_back(c);
                ++kept;
            }
        }
    }

    // --- 步骤2: (可选)导航 source_url,用 kJsExtractRawPage 取正文 ---
    std::string articleText;
    std::string articleStatus = "skipped";
    // 判断 source_url 是否为外部链接(非 HN 站内)
    bool isExternal = !sourceUrl.empty() &&
                      sourceUrl.find("news.ycombinator.com") == std::string::npos;
    if (fetchArticle && isExternal) {
        std::wstring extUrl = to_wstring(sourceUrl);
        // 外部站点(尤其 GitHub Pages / 博客 / 文档站)页面渲染差异大,
        // 采用递进式抓取:先常规等待,失败后用更长等待时间重试
        struct Attempt { int wait_ms; uint32_t nav_timeout_ms; };
        constexpr Attempt attempts[] = {
            { 4000, 35000 },  // 第一次:适度等待,覆盖多数静态站
            { 7000, 45000 }   // 第二次:更长等待,应对慢加载/SPA/网络抖动
        };
        json artRaw;
        bool gotResult = false;
        for (const auto& att : attempts) {
            artRaw = NavigateAndExecuteRaw(session, extUrl, kJsExtractRawPage,
                                            kLogPrefix, att.wait_ms, att.nav_timeout_ms);
            if (artRaw.is_object()) {
                gotResult = true;
                break;
            }
        }
        if (gotResult) {
            if (artRaw.contains("text") && artRaw["text"].is_string()) {
                articleText = artRaw["text"].get<std::string>();
                // 截断
                if ((int)articleText.size() > textMaxChars) {
                    articleText = articleText.substr(0, textMaxChars);
                }
                articleStatus = articleText.empty() ? "no_text" : "ok";
            } else {
                articleStatus = "no_text";
            }
        } else {
            articleStatus = "fetch_failed";
        }
    } else if (!fetchArticle) {
        articleStatus = "disabled";
    } else if (!isExternal) {
        articleStatus = "no_external_url";
    }

    // --- 步骤3: 合并结构化结果 ---
    json payload = {
        {"success", true},
        {"hn_id", hnId},
        {"title", title},
        {"source_url", sourceUrl},
        {"article_plaintext", articleText},
        {"article_fetch_status", articleStatus},
        {"discussion_comments", comments},
        {"comment_count", comments.size()},
        {"comment_max_depth_applied", maxDepth}
    };

    // ── 缓存写入:item:{hn_id} (TTL=12h) ──
    if (cm.is_ready()) {
        cm.put("hn", cache_key, payload.dump(), "json", 12, "", "ok", "");
    }

    // ── entity_mapper: story → topic 实体, comments → comment 实体 ──
    // 跨源闭合:HN story 注册为 topic 实体,建立 discussed_in/mentions 关系
    if (cm.is_ready() && !title.empty()) {
        // 注册 topic 实体(story 本身)
        std::string story_eid = cm.register_entity(
            "topic",
            "hn:" + hnId,  // canonical_name,带 hn: 前缀避免与其他源冲突
            {title},        // aliases = title
            {"hackernews"}, // tags
            {{"source_url", sourceUrl},
             {"comment_count", (int)comments.size()},
             {"hn_id", hnId}},
            title
        );

        // 时间快照: comment_count
        cm.record_metric(story_eid, "comment_count",
                          (double)comments.size(), "hn");

        // 关系: comment discussed_in story
        // 为每个评论建立 comment 实体 + discussed_in 关系
        int comment_idx = 0;
        for (auto& c : comments) {
            if (!c.is_object()) continue;
            std::string author = c.value("author", "");
            std::string text = c.value("text", "");
            int level = c.value("reply_level", 1);
            if (text.empty() && author.empty()) continue;

            // comment 实体 ID: hn:{hnId}#comment:{idx}
            std::string comment_eid = cm.register_entity(
                "comment",
                "hn:" + hnId + "#c" + std::to_string(comment_idx),
                {},
                {"hackernews"},
                {{"author", author},
                 {"reply_level", level},
                 {"text_preview", text.size() > 100 ? text.substr(0, 100) : text}},
                ""
            );
            cm.add_relation(comment_eid, story_eid, "discussed_in", 0.8, "hn", hnId);

            // 关系: comment mentions 作者(如果作者非空)
            if (!author.empty()) {
                std::string person_eid = cm.register_entity(
                    "person", author, {}, {}, json::object(), author
                );
                cm.add_relation(comment_eid, person_eid, "authored_by", 1.0, "hn", hnId);
            }
            ++comment_idx;
            // 限制每 story 最多注册 30 条 comment 实体,防止过载
            if (comment_idx >= 30) break;
        }

        // 跨源闭合:如果 source_url 指向 arxiv.org,建立 story mentions paper 关系
        if (!sourceUrl.empty() && sourceUrl.find("arxiv.org") != std::string::npos) {
            // 从 URL 提取 arxiv_id: https://arxiv.org/abs/2608.00757
            std::string arxiv_id;
            size_t abs_pos = sourceUrl.find("/abs/");
            if (abs_pos != std::string::npos) {
                arxiv_id = sourceUrl.substr(abs_pos + 5);
                // 去除可能的版本号和查询参数
                size_t v_pos = arxiv_id.find('v');
                size_t q_pos = arxiv_id.find('?');
                size_t s_pos = arxiv_id.find('/');
                size_t cut = std::string::npos;
                if (v_pos != std::string::npos) cut = std::min(cut, v_pos);
                if (q_pos != std::string::npos) cut = std::min(cut, q_pos);
                if (s_pos != std::string::npos) cut = std::min(cut, s_pos);
                if (cut != std::string::npos) arxiv_id = arxiv_id.substr(0, cut);
            }
            if (!arxiv_id.empty()) {
                // 查找或注册 arxiv paper 实体
                std::string paper_eid = cm.register_entity(
                    "paper", arxiv_id, {}, {}, json::object(), ""
                );
                cm.add_relation(story_eid, paper_eid, "mentions", 0.9, "hn", hnId);
            }
        }
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
