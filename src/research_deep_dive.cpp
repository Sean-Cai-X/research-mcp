#include "github_research/research_deep_dive.hpp"
#include "github_research/hackernews_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <set>
#include <map>
#include <tuple>
#include <sstream>

namespace github_research {

namespace {
constexpr const char* kLogPrefix = "[dd]";

// ============================================================
// Helpers: keyword tokenization + MCP payload unwrap
// ============================================================
std::vector<std::string> split_keywords(const std::string& query) {
    std::vector<std::string> result;
    std::istringstream iss(query);
    std::string word;
    while (iss >> word) {
        while (!word.empty() && !isalnum(static_cast<unsigned char>(word.front())))
            word.erase(word.begin());
        while (!word.empty() && !isalnum(static_cast<unsigned char>(word.back())))
            word.pop_back();
        if (word.size() >= 3) result.push_back(word);
    }
    return result;
}

int score_keyword_overlap(const std::string& title,
                          const std::vector<std::string>& keywords) {
    if (title.empty() || keywords.empty()) return 0;
    std::string lt = to_lower(title);
    int s = 0;
    for (const auto& kw : keywords) {
        if (lt.find(to_lower(kw)) != std::string::npos) s += 3;
    }
    return s;
}

// Unwrap WrapMcpResult output: {content:[{type:"text",text:"JSON_STR"}]}
// Returns inner parsed JSON on success, null json on failure.
json unwrap_mcp_content(const json& wrapped) {
    if (!wrapped.contains("content") || !wrapped["content"].is_array() ||
        wrapped["content"].empty()) return nullptr;
    const auto& c0 = wrapped["content"][0];
    if (!c0.contains("type") || !c0.contains("text") ||
        !c0["text"].is_string()) return nullptr;
    try {
        json inner = json::parse(c0["text"].get<std::string>());
        if (inner.is_object()) return inner;
    } catch (...) {}
    return nullptr;
}

// ============================================================
// Step A: 从 HN 正文中提取 URL
// ============================================================
// 返回: 每个 URL + 出现次数(该篇 HN 中)
void collect_urls_from_hn(const json& hn_story_payload,
                          std::map<std::string, int>& url_scores,
                          std::map<std::string, std::string>& url_evidence) {
    if (!hn_story_payload.is_object()) return;

    // 1. 文章正文 plaintext
    if (hn_story_payload.contains("article_plaintext") &&
        hn_story_payload["article_plaintext"].is_string()) {
        std::string txt = hn_story_payload["article_plaintext"].get<std::string>();
        for (auto& u : extract_urls(txt)) {
            std::string nu = normalize_url(u);
            if (is_low_value_url(nu)) continue;
            url_scores[nu] += 1;
            if (url_evidence.find(nu) == url_evidence.end())
                url_evidence[nu] = "mentioned in article body";
        }
    }
    // 2. 每条评论文本
    if (hn_story_payload.contains("discussion_comments") &&
        hn_story_payload["discussion_comments"].is_array()) {
        for (auto& c : hn_story_payload["discussion_comments"]) {
            if (!c.is_object() || !c.contains("text") || !c["text"].is_string()) continue;
            for (auto& u : extract_urls(c["text"].get<std::string>())) {
                std::string nu = normalize_url(u);
                if (is_low_value_url(nu)) continue;
                url_scores[nu] += 1;
                std::string author = c.value("author", "");
                url_evidence[nu] = std::string("cited in comment") +
                                   (author.empty() ? "" : " by " + author);
            }
        }
    }
    // 3. source_url 本身(即使正文也包含了也再给个高分,它是主文章)
    if (hn_story_payload.contains("source_url") &&
        hn_story_payload["source_url"].is_string()) {
        std::string su = hn_story_payload["source_url"].get<std::string>();
        std::string nu = normalize_url(su);
        if (!is_low_value_url(nu)) {
            url_scores[nu] += 5;
            url_evidence[nu] = "primary article (HN story source_url)";
        }
    }
}

// ============================================================
// Step B: 从实体 metadata 中提取 URL
// ============================================================
// 常见 URL 字段名
static const char* kUrlMetadataKeys[] = {
    "source_url", "homepage", "homepage_url", "repo_url", "project_url",
    "website", "web", "link", "url", "blog_url", "paper_url",
    "documentation_url", "docs_url", "issue_url", "download_url"
};

void collect_urls_from_entity(const EntityData& ent, double relation_weight,
                              std::map<std::string, int>& url_scores,
                              std::map<std::string, std::string>& url_evidence) {
    int w = std::max(1, (int)(relation_weight * 3.0 + 0.5));
    // 1. 扫描 entity_fields 中所有字段值
    //    优先识别 URL 类字段名(给证据标签),其余字段也兜底尝试
    for (auto& [k, fv] : ent.fields) {
        if (!fv.value.is_string()) continue;
        std::string val = fv.value.get<std::string>();
        if (val.empty() || val.size() < 8) continue;
        std::string lk = to_lower(k);
        bool is_explicit_url_key = false;
        for (auto t : kUrlMetadataKeys) {
            if (lk.find(t) != std::string::npos) { is_explicit_url_key = true; break; }
        }
        // 只在以下情况提取:
        //   a) 字段名显式包含 URL 类键
        //   b) 值以 "http" 开头(兜底发现,即使字段名不标准)
        bool has_http_prefix = (::memcmp(val.data(), "http", 4) == 0);
        if (!is_explicit_url_key && !has_http_prefix) continue;

        auto urls = extract_urls(val);
        int score_inc = is_explicit_url_key ? w * 2 : w;
        for (auto& u : urls) {
            std::string nu = normalize_url(u);
            if (is_low_value_url(nu)) continue;
            url_scores[nu] += score_inc;
            url_evidence[nu] = std::string("entity field ") + k + " of " + ent.canonical_name;
        }
    }
}

// ============================================================
// Step C: 从图谱遍历结果中收集 URL
// ============================================================
void collect_urls_from_graph(const json& graph_result,
                             CacheManager& cm,
                             std::map<std::string, int>& url_scores,
                             std::map<std::string, std::string>& url_evidence) {
    // graph_result: traverse_graph 返回值
    // 格式: {levels:[ [{entity_id, depth, weight, relation_type}], ... ]}
    if (!graph_result.is_object() || !graph_result.contains("levels") ||
        !graph_result["levels"].is_array()) {
        return;
    }
    std::set<std::string> visited_entities;
    for (auto& level : graph_result["levels"]) {
        if (!level.is_array()) continue;
        for (auto& node : level) {
            if (!node.is_object()) continue;
            std::string eid = node.value("entity_id", "");
            if (eid.empty() || visited_entities.count(eid)) continue;
            visited_entities.insert(eid);
            double w = node.value("weight", 0.5);
            EntityData ed = cm.get_entity(eid);
            if (ed.entity_id.empty()) continue;
            collect_urls_from_entity(ed, w, url_scores, url_evidence);
        }
    }
}

// ============================================================
// Step D: 排序取 Top N
// ============================================================
std::vector<std::tuple<std::string, int, std::string>>
rank_urls(const std::map<std::string, int>& scores,
          const std::map<std::string, std::string>& evidence,
          int limit) {
    // <score desc, url>
    std::vector<std::pair<int, std::string>> order;
    order.reserve(scores.size());
    for (auto& [u, s] : scores) order.emplace_back(s, u);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    std::vector<std::tuple<std::string, int, std::string>> result;
    result.reserve(std::min(limit, (int)order.size()));
    for (auto& p : order) {
        if ((int)result.size() >= limit) break;
        std::string ev;
        auto it = evidence.find(p.second);
        if (it != evidence.end()) ev = it->second;
        result.emplace_back(p.second, p.first, ev);
    }
    return result;
}

// ============================================================
// Step E: 抓次级网页(带缓存 TTL=72h)
// ============================================================
json fetch_secondary_page(WebViewSession& session,
                          const std::string& url,
                          int text_max_chars,
                          bool force_refresh) {
    CacheManager& cm = CacheManager::instance();
    std::string ck = "page:" + normalize_url(url);
    json out = json::object();
    out["url"] = url;
    out["normalized_url"] = normalize_url(url);

    if (cm.is_ready() && !force_refresh) {
        auto cached = cm.get("dd", ck);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("dd", ck)) {
            try {
                json cp = json::parse(cached->payload);
                if (cp.is_object()) {
                    cp["cache_hit"] = true;
                    cp["cache_expires_at"] = cached->expires_at;
                    return cp;
                }
            } catch (...) { cm.invalidate("dd", ck); }
        }
    }

    // 实际抓取
    auto start = std::chrono::steady_clock::now();
    json raw = NavigateAndExecuteRaw(session, to_wstring(url), kJsExtractRawPage,
                                     kLogPrefix, 2500, 30000);
    auto end = std::chrono::steady_clock::now();
    int latency_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    out["latency_ms"] = latency_ms;
    if (!raw.is_object()) {
        out["fetch_status"] = "fetch_failed";
        out["plaintext"] = "";
        out["page_title"] = "";
    } else {
        out["page_title"] = raw.value("title", "");
        std::string txt = raw.value("text", "");
        if ((int)txt.size() > text_max_chars) txt.resize(text_max_chars);
        out["plaintext"] = txt;
        out["plaintext_chars"] = (int)txt.size();
        out["fetch_status"] = txt.empty() ? "no_text" : "ok";
    }

    // 缓存写入
    if (cm.is_ready()) {
        cm.put("dd", ck, out.dump(), "json", 72, "", out.value("fetch_status", "ok"), "");
    }
    return out;
}

} // namespace

// ============================================================
// 主入口
// ============================================================
json ToolResearchDeepDive(WebViewSession& hn_session, const json& args) {
    // ── 参数解析 ──
    std::string seed_query;
    if (args.contains("seed_query") && args["seed_query"].is_string()) {
        seed_query = args["seed_query"].get<std::string>();
    }
    if (seed_query.empty()) {
        return McpError("ERROR: [dd] 'seed_query' parameter is required");
    }
    int max_links = 5;
    if (args.contains("max_secondary_links") && args["max_secondary_links"].is_number_integer()) {
        max_links = args["max_secondary_links"].get<int>();
    }
    if (max_links < 1) max_links = 1;
    if (max_links > 15) max_links = 15;

    int page_text_max = 15000;
    if (args.contains("page_text_max_chars") && args["page_text_max_chars"].is_number_integer()) {
        page_text_max = args["page_text_max_chars"].get<int>();
    }
    if (page_text_max < 1000) page_text_max = 1000;
    if (page_text_max > 50000) page_text_max = 50000;

    int graph_depth = 2;
    if (args.contains("graph_max_depth") && args["graph_max_depth"].is_number_integer()) {
        graph_depth = args["graph_max_depth"].get<int>();
    }
    if (graph_depth < 1) graph_depth = 1;
    if (graph_depth > 3) graph_depth = 3;

    bool force_refresh = false;
    if (args.contains("force_refresh") && args["force_refresh"].is_boolean()) {
        force_refresh = args["force_refresh"].get<bool>();
    }

    CacheManager& cm = CacheManager::instance();

    // ── SEED DISCOVERY ──
    json seed_payload = json::object();
    std::string seed_entity_id;
    std::string seed_type; // "hn" | "entity" | "keyword"
    std::vector<std::string> start_entities;

    // --- 规则 1: 数字 或 hn:XXXX 前缀 -> HN story ---
    bool looks_hn = false;
    std::string hn_id;
    if (seed_query.size() >= 4 &&
        ::memcmp(seed_query.data(), "hn:", 3) == 0) {
        looks_hn = true;
        hn_id = seed_query.substr(3);
    } else {
        bool all_digit = true;
        for (char c : seed_query) if (c < '0' || c > '9') { all_digit = false; break; }
        if (all_digit && seed_query.size() >= 4) {
            looks_hn = true;
            hn_id = seed_query;
        }
    }
    if (looks_hn) {
        seed_type = "hn";
        json hn_args = {
            {"hn_id", hn_id},
            {"fetch_external_article", true},
            {"fetch_comments", true},
            {"comment_max_depth", 3},
            {"max_comment_count", 120},
            {"text_max_chars", 30000}
        };
        seed_payload = ToolHnFetchDetailedStory(hn_session, hn_args);
        // Tool 返回格式是 WrapMcpResult: {content:[{type:"text",text:"..."}]}
        // 解包出实际 payload(由 caller 的 MCP 包装一致性决定)
        // 这里解到 payload 层即可: 如果有 content[0].text 且是 JSON 字符串则 parse
        if (seed_payload.contains("content") && seed_payload["content"].is_array() &&
            !seed_payload["content"].empty() &&
            seed_payload["content"][0].contains("type") &&
            seed_payload["content"][0]["type"] == "text" &&
            seed_payload["content"][0].contains("text") &&
            seed_payload["content"][0]["text"].is_string()) {
            try {
                json inner = json::parse(seed_payload["content"][0]["text"].get<std::string>());
                if (inner.is_object()) seed_payload = inner;
            } catch (...) {}
        }
        // 找 HN 实体(即使没找到, seed_type 也保持 "hn")
        if (cm.is_ready()) {
            seed_entity_id = cm.find_entity_by_name("hn:" + hn_id, "topic");
            if (!seed_entity_id.empty()) start_entities.push_back(seed_entity_id);
        }
    }
    // --- 规则 2: 非 HN 路径 -> 实体搜索(只在未命中 HN 时执行) ---
    if (seed_type != "hn" && start_entities.empty() && cm.is_ready()) {
        // 先精确名匹配
        seed_entity_id = cm.find_entity_by_name(seed_query, "");
        if (seed_entity_id.empty()) {
            // 退回到 search_entities (模糊)
            auto hits = cm.search_entities(seed_query, "", 0.0, 5);
            if (!hits.empty()) {
                seed_entity_id = hits[0].entity_id;
                for (auto& h : hits) start_entities.push_back(h.entity_id);
            }
        } else {
            start_entities.push_back(seed_entity_id);
        }
        if (seed_entity_id.empty()) {
            seed_type = "keyword";
        } else if (seed_type.empty()) {
            seed_type = "entity";
        }
    }

    // ── URL AGGREGATION 容器:提前声明,以便 Rule 3 keyword 回退可填充 ──
    std::map<std::string, int> url_scores;
    std::map<std::string, std::string> url_evidence;

    // --- 规则 3: keyword 模式回退 -> 抓取 HN 首页作为种子素材 ---
    // 当 seed_type=keyword 且既无 HN 前缀命中、也无实体缓存匹配时,
    // 从 HN 首页拉取热门故事,按关键词重叠度打分,取 TOP 若干条深度抓取,
    // 再进入后续 URL 聚合 + 图谱遍历链路,避免直接返回空结果.
    if (seed_type == "keyword" && url_scores.empty() && start_entities.empty()) {
        auto keywords = split_keywords(seed_query);

        // 1) 拉取 HN 前 20 条故事索引
        json top_args = {{"count", 20}};
        json top_wrapped = ToolHnGetTopStories(hn_session, top_args);
        json top_payload = unwrap_mcp_content(top_wrapped);
        json top_stories = json::array();
        if (top_payload.is_object() && top_payload.contains("stories") &&
            top_payload["stories"].is_array()) {
            top_stories = top_payload["stories"];
        }

        if (!top_stories.empty()) {
            // 2) 按标题关键词重叠打分,无命中者也给个最低分保证 coverage
            std::vector<std::tuple<int, std::string, std::string>> scored;
            // score, hn_id, title
            for (auto& s : top_stories) {
                if (!s.is_object()) continue;
                std::string sid = s.value("hn_id", "");
                std::string title = s.value("title", "");
                if (sid.empty()) continue;
                int score = score_keyword_overlap(title, keywords);
                scored.emplace_back(score, sid, title);
            }
            // 分数降序,同分按 hn_id 升序稳定排序
            std::sort(scored.begin(), scored.end(),
                [](const auto& a, const auto& b) {
                    if (std::get<0>(a) != std::get<0>(b))
                        return std::get<0>(a) > std::get<0>(b);
                    return std::get<1>(a) < std::get<1>(b);
                });

            // 3) 对 TOP 3 条匹配/热门故事深度抓取,收集 URL/评论/实体
            const int kFetchLimit = 3;
            int fetched = 0;
            for (const auto& item : scored) {
                if (fetched >= kFetchLimit) break;
                const std::string& sid = std::get<1>(item);
                json detail_args = {
                    {"hn_id", sid},
                    {"fetch_external_article", true},
                    {"fetch_comments", true},
                    {"comment_max_depth", 2},
                    {"max_comment_count", 60},
                    {"text_max_chars", 20000}
                };
                json detail_wrapped = ToolHnFetchDetailedStory(hn_session, detail_args);
                json detail_payload = unwrap_mcp_content(detail_wrapped);
                if (!detail_payload.is_object()) continue;
                collect_urls_from_hn(detail_payload, url_scores, url_evidence);
                if (cm.is_ready()) {
                    std::string eid = cm.find_entity_by_name("hn:" + sid, "topic");
                    if (!eid.empty()) {
                        start_entities.push_back(eid);
                        if (seed_entity_id.empty()) seed_entity_id = eid;
                    }
                }
                ++fetched;
            }
        }
    }

    // ── URL AGGREGATION ──
    // 注:url_scores / url_evidence 已在 Rule 3 前声明(提前填充 keyword 回退结果)

    // 源 1: HN story 正文+评论
    collect_urls_from_hn(seed_payload, url_scores, url_evidence);

    // 源 2: 起点实体 + 图谱 BFS
    json graph_trace = json::array();
    if (cm.is_ready()) {
        // 先从起点实体采 URL
        for (auto& eid : start_entities) {
            EntityData ed = cm.get_entity(eid);
            if (ed.entity_id.empty()) continue;
            collect_urls_from_entity(ed, 1.0, url_scores, url_evidence);
            // 图谱 BFS
            json gr = cm.traverse_graph(eid, graph_depth, 0.2, 20);
            if (gr.is_object()) {
                graph_trace.push_back({
                    {"start_entity", eid},
                    {"result", gr}
                });
                collect_urls_from_graph(gr, cm, url_scores, url_evidence);
            }
        }
    }

    // ── 排序取 Top N ──
    auto ranked = rank_urls(url_scores, url_evidence, max_links);

    // ── SECONDARY FETCH ──
    json secondary_pages = json::array();
    json ranked_urls_json = json::array();
    for (auto& [u, score, ev] : ranked) {
        ranked_urls_json.push_back({
            {"url", u},
            {"score", score},
            {"evidence", ev}
        });
    }
    // 记录次级链接优先级列表(即使失败也可见)
    seed_payload["secondary_links_priority"] = ranked_urls_json;

    int page_idx = 0;
    for (auto& [u, score, ev] : ranked) {
        auto pg_start = std::chrono::steady_clock::now();
        json pg = fetch_secondary_page(hn_session, u, page_text_max, force_refresh);
        pg["priority_score"] = score;
        pg["discovery_evidence"] = ev;
        pg["index"] = page_idx;
        auto pg_end = std::chrono::steady_clock::now();
        pg["total_process_ms"] = (int)std::chrono::duration_cast<std::chrono::milliseconds>(pg_end - pg_start).count();
        secondary_pages.push_back(pg);
        ++page_idx;
    }

    // ── 大包组装 ──
    json result = json::object();
    result["seed"] = {
        {"query", seed_query},
        {"resolved_type", seed_type},
        {"hn_id", hn_id.empty() ? nullptr : json(hn_id)},
        {"seed_entity_id", seed_entity_id.empty() ? nullptr : json(seed_entity_id)},
        {"start_entities", start_entities}
    };
    result["seed_payload"] = seed_payload;
    result["graph_traces"] = graph_trace;
    result["url_aggregation"] = {
        {"candidate_count", (int)url_scores.size()},
        {"ranked_candidates", ranked_urls_json}
    };
    result["secondary_pages"] = secondary_pages;
    result["secondary_page_count"] = secondary_pages.size();
    // 汇总统计
    int total_secondary_chars = 0;
    int pages_ok = 0;
    for (auto& p : secondary_pages) {
        if (!p.is_object()) continue;
        if (p.value("fetch_status", "") == "ok") pages_ok++;
        if (p.contains("plaintext") && p["plaintext"].is_string())
            total_secondary_chars += (int)p["plaintext"].get<std::string>().size();
    }
    result["summary_stats"] = {
        {"secondary_pages_total", secondary_pages.size()},
        {"secondary_pages_ok", pages_ok},
        {"secondary_pages_failed", (int)secondary_pages.size() - pages_ok},
        {"total_secondary_plaintext_chars", total_secondary_chars}
    };

    return WrapMcpResult(result);
}

} // namespace github_research
