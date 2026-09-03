#include "github_research/cache_manager.hpp"
#include "github_research/focus_engine.hpp"
#include "github_research/focus_attr_engine.hpp"
#include "github_research/focus_track_engine.hpp"
#include "github_research/focus_web_search.hpp"
#include "github_research/curl_http_client.hpp"
#include "github_research/string_utils.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace github_research {

// ============================================================
// 定向知识雷达 — Focus MCP 工具集
// 来源: next.txt "定向蔓延" 设计文档
// ============================================================

namespace {

json McpErrorFocus(const std::string& msg) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", msg}}})},
        {"isError", true}
    };
}

json McpSuccessFocus(const json& payload) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", payload.dump()}}})},
        {"isError", false}
    };
}

// 辅助:从 arguments 安全读字符串
static std::string get_str(const json& args, const std::string& key, const std::string& def = "") {
    if (args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return def;
}

static int get_int(const json& args, const std::string& key, int def) {
    if (args.contains(key) && args[key].is_number_integer()) return args[key].get<int>();
    return def;
}

static double get_double(const json& args, const std::string& key, double def) {
    if (args.contains(key) && args[key].is_number()) return args[key].get<double>();
    return def;
}

static std::vector<std::string> get_str_vec(const json& args, const std::string& key) {
    std::vector<std::string> out;
    if (args.contains(key) && args[key].is_array()) {
        for (const auto& v : args[key]) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    }
    return out;
}

} // anonymous namespace

// ── 1. focus_create: 创建关注域 ──────────────────────────────
json ToolFocusCreate(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string name = get_str(args, "name");
    if (name.empty()) return McpErrorFocus("ERROR: name is required");

    std::string description = get_str(args, "description");
    auto seed_queries = get_str_vec(args, "seed_queries");
    auto keywords = get_str_vec(args, "keywords");
    auto exclude_words = get_str_vec(args, "exclude_words");
    int max_depth = get_int(args, "max_depth", 3);
    double threshold = get_double(args, "relevance_threshold", 0.55);
    int max_nodes = get_int(args, "max_nodes", 500);

    // 种子实体:如果用户传了 seed_entity_ids 就直接用,否则从 seed_queries 搜索
    std::vector<std::string> seed_entity_ids;
    if (args.contains("seed_entity_ids") && args["seed_entity_ids"].is_array()) {
        for (const auto& v : args["seed_entity_ids"]) {
            if (v.is_string()) seed_entity_ids.push_back(v.get<std::string>());
        }
    }

    // 如果没有 seed_entity_ids,尝试用 seed_queries 搜索现有实体
    if (seed_entity_ids.empty() && !seed_queries.empty()) {
        for (const auto& q : seed_queries) {
            auto found = cm.search_entities(q, "", 0.0, 3);
            for (const auto& e : found) {
                seed_entity_ids.push_back(e.entity_id);
            }
        }
    }

    if (seed_entity_ids.empty()) {
        return McpErrorFocus("ERROR: no seed entities found. Provide seed_entity_ids or seed_queries that match existing entities.");
    }

    // 如果用户没给 keywords,从 description 做简单分词提取
    if (keywords.empty() && !description.empty()) {
        // 极简:取 description 的前 30 字符作为初始 keyword,后续可由 LLM 完善
        keywords.push_back(name);
    }
    if (keywords.empty()) keywords.push_back(name);

    std::string focus_id = cm.create_focus(
        name, description, seed_entity_ids, keywords, exclude_words,
        max_depth, threshold, max_nodes
    );

    if (focus_id.empty()) {
        return McpErrorFocus("ERROR: failed to create focus");
    }

    json result = {
        {"focus_id", focus_id},
        {"name", name},
        {"seed_entities", seed_entity_ids},
        {"keywords", keywords},
        {"status", "active"}
    };
    return McpSuccessFocus(result);
}

// ── 2. focus_list: 列出所有关注域 ────────────────────────────
json ToolFocusList(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    auto focuses = cm.list_focuses();
    json result = {
        {"focuses", focuses},
        {"total", (int)focuses.size()}
    };
    return McpSuccessFocus(result);
}

// ── 3. focus_get: 获取关注域详情 ────────────────────────────
json ToolFocusGet(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    json focus = cm.get_focus(focus_id);
    if (focus.empty()) return McpErrorFocus("ERROR: focus not found: " + focus_id);

    // 附带成员数量统计
    auto members = cm.get_focus_members(focus_id, "", 1000);
    json stats = cm.get_sprawl_stats(focus_id);
    json result = focus;
    result["member_count"] = (int)members.size();
    result["sprawl_stats"] = stats;
    return McpSuccessFocus(result);
}

// ── 4. focus_delete: 删除关注域 ──────────────────────────────
json ToolFocusDelete(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    bool keep = args.value("keep_entities", true);
    bool ok = cm.delete_focus(focus_id, keep);
    json result = {{"focus_id", focus_id}, {"deleted", ok}, {"keep_entities", keep}};
    return McpSuccessFocus(result);
}

// ── 5. focus_members: 列出关注域成员 ────────────────────────
json ToolFocusMembers(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    std::string status = get_str(args, "sprawl_status", "");
    int limit = get_int(args, "limit", 100);

    auto members = cm.get_focus_members(focus_id, status, limit);
    json result = {
        {"focus_id", focus_id},
        {"members", members},
        {"total", (int)members.size()}
    };
    return McpSuccessFocus(result);
}

// ── 6. focus_gaps: 列出缺口(待补的属性) ──────────────────────
json ToolFocusGaps(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    double min_priority = get_double(args, "min_priority", 0.0);
    int limit = get_int(args, "limit", 20);

    auto gaps = cm.get_gaps(focus_id, min_priority, limit);
    json result = {
        {"focus_id", focus_id},
        {"gaps", gaps},
        {"total", (int)gaps.size()}
    };
    return McpSuccessFocus(result);
}

// ── 7. focus_stats: 蔓延进度看板 ──────────────────────────────
json ToolFocusStats(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    json stats = cm.get_sprawl_stats(focus_id);

    // 如果指定了 focus,附加该 focus 的详细成员统计
    if (!focus_id.empty()) {
        auto members = cm.get_focus_members(focus_id, "", 2000);
        int n_seed = 0, n_active = 0, n_boundary = 0, n_pruned = 0, n_exhausted = 0;
        for (const auto& m : members) {
            std::string s = m.value("sprawl_status", "");
            if (s == "seed") n_seed++;
            else if (s == "active") n_active++;
            else if (s == "boundary") n_boundary++;
            else if (s == "pruned") n_pruned++;
            else if (s == "exhausted") n_exhausted++;
        }
        stats["seed"] = n_seed;
        stats["active"] = n_active;
        stats["boundary"] = n_boundary;
        stats["pruned"] = n_pruned;
        stats["exhausted"] = n_exhausted;
    }

    return McpSuccessFocus(stats);
}

// ── 8. entity_attrs: 查询实体的属性 ──────────────────────────
json ToolEntityAttrs(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string entity_id = get_str(args, "entity_id");
    if (entity_id.empty()) return McpErrorFocus("ERROR: entity_id is required");

    std::string attr_key = get_str(args, "attr_key", "");
    auto attrs = cm.get_attributes(entity_id, attr_key);

    // 如果指定了 attr_key,附加合并结果
    json result;
    result["entity_id"] = entity_id;
    result["attributes"] = attrs;
    if (!attr_key.empty()) {
        result["merged"] = cm.get_merged_attribute(entity_id, attr_key);
    }
    return McpSuccessFocus(result);
}

// ── 9. focus_prune: 人工剪枝 ─────────────────────────────────
json ToolFocusPrune(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    std::string entity_id = get_str(args, "entity_id");
    if (focus_id.empty() || entity_id.empty()) {
        return McpErrorFocus("ERROR: focus_id and entity_id are required");
    }
    std::string reason = get_str(args, "reason", "manual_prune");
    (void)reason;

    bool ok = cm.update_member_status(focus_id, entity_id, "pruned");
    json result = {{"focus_id", focus_id}, {"entity_id", entity_id}, {"pruned", ok}};
    return McpSuccessFocus(result);
}

// ── 10. focus_promote: 人工提升 ──────────────────────────────
json ToolFocusPromote(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    std::string entity_id = get_str(args, "entity_id");
    if (focus_id.empty() || entity_id.empty()) {
        return McpErrorFocus("ERROR: focus_id and entity_id are required");
    }

    bool ok = cm.update_member_status(focus_id, entity_id, "active");
    json result = {{"focus_id", focus_id}, {"entity_id", entity_id}, {"promoted", ok}};
    return McpSuccessFocus(result);
}

// ── 11. focus_sprawl_tick: 执行一轮蔓延 tick ──────────────────
json ToolFocusSprawlTick(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    int max_nodes = get_int(args, "max_nodes_per_tick", 5);
    bool dry_run = args.value("dry_run", false);

    json result = focus_engine::run_sprawl_tick(focus_id, max_nodes, dry_run);
    if (result.contains("error")) {
        return McpErrorFocus(result["error"].get<std::string>());
    }
    return McpSuccessFocus(result);
}

// ── 12. focus_estimate_relevance: 估算实体相关性(调试) ────────
json ToolFocusEstimateRelevance(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    json candidate;
    if (args.contains("entity_candidate") && args["entity_candidate"].is_object()) {
        candidate = args["entity_candidate"];
    } else {
        // 简化:从 entity_name / entity_description 构造
        std::string name = get_str(args, "entity_name");
        std::string desc = get_str(args, "entity_description");
        if (name.empty()) return McpErrorFocus("ERROR: entity_candidate or entity_name is required");
        candidate["canonical_name"] = name;
        if (!desc.empty()) candidate["description"] = desc;
    }
    std::string rel_type = get_str(args, "relation_type", "");
    int depth = get_int(args, "depth", 1);

    json result = focus_engine::estimate_relevance(focus_id, candidate, rel_type, depth);
    return McpSuccessFocus(result);
}

// ── 13. focus_extract_tick: 对 focus 批量提取属性 ────────────
json ToolFocusExtractTick(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    int max_entities = get_int(args, "max_entities_per_tick", 10);
    bool dry_run = args.value("dry_run", false);

    json result = run_extract_tick(focus_id, max_entities, dry_run);
    if (result.contains("error")) {
        return McpErrorFocus(result["error"].get<std::string>());
    }
    return McpSuccessFocus(result);
}

// ── 14. focus_gaps_detect: 检测属性缺口 ──────────────────────
json ToolFocusGapsDetect(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");

    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");

    auto gaps = detect_gaps(focus_id);

    json result;
    result["focus_id"] = focus_id;
    result["total_gaps"] = (int)gaps.size();
    result["gaps"] = gaps;
    return McpSuccessFocus(result);
}

// ── 15. focus_track_tick: 执行自适应跟踪 tick ────────────────
json ToolFocusTrackTick(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");
    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");
    int max_schedules = get_int(args, "max_schedules_per_tick", 10);
    bool dry_run = args.value("dry_run", false);
    json result = run_track_tick(focus_id, max_schedules, dry_run);
    return McpSuccessFocus(result);
}

// ── 16. focus_updates_since: 类 RSS 查询增量 ──────────────────
json ToolFocusUpdatesSince(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");
    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");
    std::string since = get_str(args, "since", "");
    json result = get_updates_since(focus_id, since);
    return McpSuccessFocus(result);
}

// ── 17. web_search: 综合搜索引擎(Bing + Tavily + 缓存) ──────
json ToolWebSearch(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");
    std::string query = get_str(args, "query");
    if (query.empty()) return McpErrorFocus("ERROR: query is required");
    std::string focus_id = get_str(args, "focus_id");
    int max_results = get_int(args, "max_results", 10);
    std::string freshness = get_str(args, "freshness", "");
    std::string mkt = get_str(args, "mkt", "en-US");
    auto results = web_search(query, focus_id, max_results, freshness, mkt);
    json out = json::array();
    for (const auto& r : results) {
        out.push_back({
            {"title", r.title},
            {"url", r.url},
            {"snippet", r.snippet},
            {"source_engine", r.source_engine}
        });
    }
    json result;
    result["query"] = query;
    result["count"] = (int)out.size();
    result["results"] = out;
    return McpSuccessFocus(result);
}

// ── 18. focus_cross_gain: 多 focus 交叉增益 ──────────────────
json ToolFocusCrossGain(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");
    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");
    // 简化版:列出同时被多个 focus 引用的实体,建议提升优先级
    json result;
    result["focus_id"] = focus_id;
    result["cross_focus_hits"] = json::array();
    result["note"] = "cross-focus boost: needs full graph traversal, framework ready";
    return McpSuccessFocus(result);
}

// ── 19. focus_export: 导出 focus 数据 ────────────────────────
json ToolFocusExport(const json& args) {
    CacheManager& cm = CacheManager::instance();
    if (!cm.is_ready()) return McpErrorFocus("ERROR: cache not initialized");
    std::string focus_id = get_str(args, "focus_id");
    if (focus_id.empty()) return McpErrorFocus("ERROR: focus_id is required");
    std::string format = get_str(args, "format", "json");
    json focus = cm.get_focus(focus_id);
    if (focus.empty()) return McpErrorFocus("ERROR: focus not found");
    auto members = cm.get_focus_members(focus_id, "", 2000);
    auto stats = cm.get_sprawl_stats(focus_id);
    json result;
    result["format"] = format;
    result["focus"] = focus;
    result["member_count"] = (int)members.size();
    result["stats"] = stats;
    if (format == "json") {
        result["members"] = members;
    }
    result["note"] = "export: full JSON available, Markdown/OPML pending";
    return McpSuccessFocus(result);
}
} // namespace github_research
