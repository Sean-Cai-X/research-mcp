#include "github_research/situation_engine.hpp"
#include "github_research/cache_manager.hpp"
#include <cmath>
#include <map>
#include <set>
#include <algorithm>
#include <ctime>

namespace github_research {
namespace situation_engine {

// ── 关系类型权重(与 focus_engine 对齐,主干道三维加权之一) ──────
double relation_weight(const std::string& rel_type) {
    static const std::map<std::string, double> w = {
        {"cites",        1.0},
        {"depends_on",   0.9},
        {"extends",      0.7},
        {"competes_with",0.5},
        {"implements",   0.8},
        {"uses",         0.6},
        {"related_to",   0.3},
        {"references",    0.3},
        {"similar_to",   0.4},
    };
    auto it = w.find(rel_type);
    return it != w.end() ? it->second : 0.3;
}

// ============================================================================
// 第一层:快照
// ============================================================================
json snapshot_focus(const std::string& focus_id) {
    CacheManager& cm = CacheManager::instance();
    json snap;
    snap["focus_id"] = focus_id;
    snap["timestamp"] = (int64_t)std::time(nullptr);

    auto members = cm.get_focus_members(focus_id, "", 5000);

    json nodes = json::array();
    std::map<std::string, int> status_count;
    std::map<int, int> depth_dist;
    std::set<std::string> node_ids;

    for (const auto& m : members) {
        std::string eid = m.value("entity_id", "");
        if (eid.empty()) continue;
        std::string status = m.value("sprawl_status", "active");
        int depth = m.value("depth", 0);
        double rel = m.value("relevance", 0.0);
        int check_cnt = m.value("check_count", 0);

        // 解析 entity_type: canonical_name
        std::string etype = "unknown", canonical = eid;
        size_t colon = eid.find(':');
        if (colon != std::string::npos) {
            etype = eid.substr(0, colon);
            canonical = eid.substr(colon + 1);
        }

        json node;
        node["entity_id"] = eid;
        node["canonical_name"] = canonical;
        node["type"] = etype;
        node["depth"] = depth;
        node["relevance"] = rel;
        node["sprawl_status"] = status;
        node["check_count"] = check_cnt;
        nodes.push_back(node);

        status_count[status]++;
        depth_dist[depth]++;
        node_ids.insert(eid);
    }
    snap["nodes"] = nodes;

    // ── 关系快照 ──
    json relations = json::array();
    std::set<std::string> rel_seen;
    for (const auto& n : nodes) {
        std::string eid = n.value("entity_id", "");
        auto rels = cm.query_relations(eid, "", "outgoing");
        for (const auto& r : rels) {
            std::string target = r.value("target_entity", "");
            std::string rtype = r.value("relation_type", "");
            if (target.empty() || rtype.empty()) continue;
            // 只要关系两端都在焦点域内就记录
            if (!node_ids.count(target)) continue;
            std::string key = eid + "|" + rtype + "|" + target;
            if (rel_seen.count(key)) continue;
            rel_seen.insert(key);
            json rel;
            rel["source_id"] = eid;
            rel["target_id"] = target;
            rel["relation_type"] = rtype;
            rel["weight"] = relation_weight(rtype);
            relations.push_back(rel);
        }
    }
    snap["relations"] = relations;

    // ── 统计 ──
    json stats;
    stats["total_nodes"] = (int)nodes.size();
    for (const auto& [s, c] : status_count) stats[s] = c;
    stats["total_relations"] = (int)relations.size();
    json dd = json::object();
    for (const auto& [d, c] : depth_dist) dd[std::to_string(d)] = c;
    stats["depth_distribution"] = dd;
    snap["stats"] = stats;

    return snap;
}

// ============================================================================
// 第二层:差分
// ============================================================================
json compute_diff(const json& baseline, const json& current) {
    json diff;

    // ── 节点 index:entity_id → node ──
    std::map<std::string, json> b_nodes, c_nodes;
    std::map<std::string, std::string> b_status, c_status;
    std::map<std::string, int> b_depth, c_depth;

    for (const auto& n : baseline.value("nodes", json::array())) {
        std::string eid = n.value("entity_id", "");
        if (!eid.empty()) {
            b_nodes[eid] = n;
            b_status[eid] = n.value("sprawl_status", "");
            b_depth[eid] = n.value("depth", 0);
        }
    }
    for (const auto& n : current.value("nodes", json::array())) {
        std::string eid = n.value("entity_id", "");
        if (!eid.empty()) {
            c_nodes[eid] = n;
            c_status[eid] = n.value("sprawl_status", "");
            c_depth[eid] = n.value("depth", 0);
        }
    }

    // ── 关系 index ──
    auto rel_key = [](const json& r) -> std::string {
        return r.value("source_id","") + "|" + r.value("relation_type","") + "|" + r.value("target_id","");
    };
    std::set<std::string> b_rels, c_rels;
    std::map<std::string, json> b_rel_map, c_rel_map;
    for (const auto& r : baseline.value("relations", json::array())) {
        std::string k = rel_key(r); b_rels.insert(k); b_rel_map[k] = r;
    }
    for (const auto& r : current.value("relations", json::array())) {
        std::string k = rel_key(r); c_rels.insert(k); c_rel_map[k] = r;
    }

    // ── 节点差分 ──
    json added_nodes = json::array();
    json removed_nodes = json::array();
    json status_changed = json::array();

    for (const auto& [eid, n] : c_nodes) {
        if (!b_nodes.count(eid)) {
            json an = n;
            // 尝试找接入父节点:同深度-1 的节点中,通过关系指向本节点的
            an.erase("sprawl_status");
            an["parent_candidates"] = json::array();
            for (const auto& r : current.value("relations", json::array())) {
                if (r.value("target_id","") == eid &&
                    b_depth.count(r.value("source_id",""))) {
                    an["parent_candidates"].push_back(r.value("source_id",""));
                }
            }
            added_nodes.push_back(an);
        } else if (b_status[eid] != c_status[eid]) {
            json sc;
            sc["entity_id"] = eid;
            sc["canonical_name"] = n.value("canonical_name", "");
            sc["old_status"] = b_status[eid];
            sc["new_status"] = c_status[eid];
            sc["reason"] = c_status[eid] == "exhausted" ? "check_count 连续超阈值"
                        : c_status[eid] == "pruned"    ? "相关性低于阈值"
                        : c_status[eid] == "active"    ? "被蔓延引擎激活"
                        : "状态流转";
            status_changed.push_back(sc);
        }
    }

    for (const auto& [eid, n] : b_nodes) {
        if (!c_nodes.count(eid)) {
            json rn;
            rn["entity_id"] = eid;
            rn["canonical_name"] = n.value("canonical_name", "");
            rn["old_status"] = b_status[eid];
            rn["old_depth"] = b_depth[eid];
            rn["reason"] = "修剪或移出焦点域";
            removed_nodes.push_back(rn);
        }
    }

    // ── 关系差分 ──
    json added_rels = json::array();
    json removed_rels = json::array();
    for (const auto& k : c_rels) if (!b_rels.count(k)) added_rels.push_back(c_rel_map[k]);
    for (const auto& k : b_rels) if (!c_rels.count(k)) removed_rels.push_back(b_rel_map[k]);

    diff["added_nodes"] = added_nodes;
    diff["removed_nodes"] = removed_nodes;
    diff["status_changed"] = status_changed;
    diff["added_relations"] = added_rels;
    diff["removed_relations"] = removed_rels;

    diff["stats"] = {
        {"added_count",   (int)added_nodes.size()},
        {"removed_count", (int)removed_nodes.size()},
        {"status_changed",(int)status_changed.size()},
        {"added_rels",    (int)added_rels.size()},
        {"removed_rels",  (int)removed_rels.size()},
    };

    return diff;
}

// ============================================================================
// 第三层:态势解析 — 生长报告 + 主干道 Top3
// ============================================================================
json parse_situation(const json& diff, const json& current) {
    json report;

    // ── 1. 增量生长报告 ──
    json growth;
    growth["new_boundary"] = 0;
    growth["new_active"] = 0;
    growth["pruned"] = (int)diff.value("removed_nodes", json::array()).size();
    growth["status_changed"] = (int)diff.value("status_changed", json::array()).size();

    // 生长方向分布(按 entity_type 聚合)
    json dir_dist = json::object();
    json key_paths = json::array();

    for (const auto& n : diff.value("added_nodes", json::array())) {
        std::string status = n.value("sprawl_status", "boundary");
        std::string etype = n.value("type", "unknown");
        dir_dist[etype] = dir_dist.value(etype, 0) + 1;
        if (status == "boundary") growth["new_boundary"] = growth.value("new_boundary", 0) + 1;
        else if (status == "active") growth["new_active"] = growth.value("new_active", 0) + 1;

        // 关键路径:接入父节点候选 + 本节点 → 构成一条新通路
        for (const auto& pid : n.value("parent_candidates", json::array())) {
            json path;
            path["from"] = pid;
            path["to"] = n.value("entity_id", "");
            path["via_type"] = n.value("type", "");
            key_paths.push_back(path);
        }
    }
    growth["direction_distribution"] = dir_dist;
    growth["key_paths"] = key_paths;
    growth["efficiency"] = growth.value("new_active", 0) + growth.value("new_boundary", 0) > 0
        ? (double)(growth.value("new_active", 0) + growth.value("new_boundary", 0))
            / std::max(1, growth.value("pruned", 0))
        : 0.0;

    report["growth_report"] = growth;

    // ── 2. 主干道权重三维加权 ──
    // 三维:节点度数(0.4) + 关系类型权重(0.3) + 种子距离(0.3)
    auto nodes = current.value("nodes", json::array());
    auto rels = current.value("relations", json::array());

    // 节点度数
    std::map<std::string, int> degree;
    for (const auto& r : rels) {
        degree[r.value("source_id","")]++;
        degree[r.value("target_id","")]++;
    }
    // 种子节点(seed 或 depth=0)
    std::set<std::string> seed_ids;
    int max_depth = 0;
    for (const auto& n : nodes) {
        if (n.value("sprawl_status","") == "seed" || n.value("depth",0) == 0)
            seed_ids.insert(n.value("entity_id",""));
        max_depth = std::max(max_depth, n.value("depth",0));
    }

    // 枚举所有简单路径(深度优先,限长 8 跳),算权重
    struct PathScore {
        std::vector<std::string> nodes;
        double total_weight;
        double node_score;
        double rel_score;
        double depth_score;
    };
    std::vector<PathScore> all_paths;

    std::function<void(const std::string&, std::vector<std::string>&, double, int, std::set<std::string>&)>
    dfs = [&](const std::string& cur, std::vector<std::string>& path, double weight,
              int depth, std::set<std::string>& visited) {
        if (depth > 8 || path.size() > 10) return;
        // 到达种子节点 → 算路径分
        if (seed_ids.count(cur) && path.size() >= 3) {
            // 补入种子节点,算完整路径
            auto full_path = path; full_path.insert(full_path.begin(), cur);
            all_paths.push_back({full_path, weight, 0, 0, 0});
        }
        // 继续延伸
        for (const auto& r : rels) {
            std::string next = "";
            if (r.value("source_id","") == cur) next = r.value("target_id","");
            else if (r.value("target_id","") == cur) next = r.value("source_id","");
            if (next.empty() || visited.count(next)) continue;
            visited.insert(next);
            path.push_back(next);
            double dw = relation_weight(r.value("relation_type",""));
            dfs(next, path, weight + dw, depth + 1, visited);
            path.pop_back();
            visited.erase(next);
        }
    };

    for (const auto& n : nodes) {
        std::string eid = n.value("entity_id","");
        if (seed_ids.count(eid)) continue;
        std::set<std::string> visited{eid};
        std::vector<std::string> path{eid};
        dfs(eid, path, 0.0, 0, visited);
    }

    // 三维加权排序
    for (auto& p : all_paths) {
        double deg_sum = 0;
        double rel_sum = p.total_weight;
        double depth_sum = 0;
        for (const auto& nid : p.nodes) {
            deg_sum += degree.count(nid) ? degree[nid] : 0;
            // depth: 找节点深度
            for (const auto& n : nodes) {
                if (n.value("entity_id","") == nid) {
                    depth_sum += (max_depth > 0)
                        ? (double)(max_depth - n.value("depth",0)) / max_depth
                        : 1.0;
                    break;
                }
            }
        }
        p.node_score = deg_sum * 0.4;
        p.rel_score  = rel_sum  * 0.3;
        p.depth_score= depth_sum * 0.3;
        p.total_weight = p.node_score + p.rel_score + p.depth_score;
    }

    std::sort(all_paths.begin(), all_paths.end(),
              [](const PathScore& a, const PathScore& b) { return a.total_weight > b.total_weight; });

    json main_roads = json::array();
    int top_n = std::min(3, (int)all_paths.size());
    for (int i = 0; i < top_n; ++i) {
        json road;
        json path_arr = json::array();
        for (const auto& nid : all_paths[i].nodes) path_arr.push_back(nid);
        road["path"] = path_arr;
        road["total_weight"] = all_paths[i].total_weight;
        road["node_score"] = all_paths[i].node_score;
        road["rel_score"]  = all_paths[i].rel_score;
        road["depth_score"]= all_paths[i].depth_score;
        road["node_count"] = (int)all_paths[i].nodes.size();
        // 贯穿领域 = path 上所有 unique entity_type
        std::set<std::string> domains;
        for (const auto& nid : all_paths[i].nodes) {
            size_t c = nid.find(':');
            if (c != std::string::npos) domains.insert(nid.substr(0, c));
        }
        json dom = json::array();
        for (const auto& d : domains) dom.push_back(d);
        road["domains"] = dom;
        main_roads.push_back(road);
    }
    report["main_roads"] = main_roads;

    // ── 3. 统计快照摘要 ──
    report["snapshot_stats"] = current.value("stats", json::object());

    return report;
}

// ============================================================================
// 便捷入口:一站式 diff + parse
// ============================================================================
json build_situation_report(const json& baseline, const json& current) {
    json diff = compute_diff(baseline, current);
    json report = parse_situation(diff, current);
    report["diff"] = diff;
    report["baseline_ts"] = baseline.value("timestamp", 0);
    report["current_ts"]  = current.value("timestamp", 0);
    return report;
}

// ============================================================================
// 第 2 阶段:普通消息提醒检测
// 4 类普通提醒 — 全部基于本轮 diff + growth_report + 历史
// ============================================================================
json detect_normal_notices(const json& diff,
                           const json& growth_report,
                           const std::vector<json>& history) {
    json notices = json::array();

    auto make_notice = [](const std::string& type,
                          const std::string& message,
                          const json& detail = json::object()) -> json {
        json n;
        n["level"] = "normal";
        n["type"] = type;
        n["message"] = message;
        n["detail"] = detail;
        return n;
    };

    int added_count   = diff.value("stats", json::object()).value("added_count", 0);
    int removed_count = diff.value("stats", json::object()).value("removed_count", 0);
    int added_rels    = diff.value("stats", json::object()).value("added_rels", 0);
    int removed_rels  = diff.value("stats", json::object()).value("removed_rels", 0);
    auto status_changed = diff.value("status_changed", json::array());

    // ── 1. 路径耗尽:本轮有节点变 exhausted ──
    for (const auto& sc : status_changed) {
        if (sc.value("new_status","") == "exhausted") {
            notices.push_back(make_notice(
                "path_exhausted",
                "路径耗尽:节点已 exhausted,暂无可延伸新节点",
                {
                    {"entity_id", sc.value("entity_id","")},
                    {"canonical_name", sc.value("canonical_name","")}
                }
            ));
        }
    }

    // ── 2. 生长停滞:连续 1 轮无新增 ──
    // history 里最旧的一条是上一轮(按 tick_index DESC,history[0]=刚存的当前轮,但我们还没存当前轮)
    // 所以 history[0] = 上一轮,history[1] = 上上轮
    int total_new = added_count;
    if (total_new == 0 && !history.empty()) {
        int prev_new = history[0].value("diff_stats", json::object()).value("added_count", -1);
        // 只要上一轮也是 0 → 连续 2 轮 = 持续停滞;连续 1 轮(上一轮 > 0) = 刚进入停滞
        if (prev_new == 0) {
            notices.push_back(make_notice(
                "growth_stalled",
                "生长停滞:本轮连续 2 轮无新增节点",
                {
                    {"current_added", 0},
                    {"prev_added", prev_new},
                    {"tick_index", history[0].value("tick_index", -1)}
                }
            ));
        } else if (prev_new > 0) {
            notices.push_back(make_notice(
                "growth_stalled",
                "生长停滞:本轮无新增(上一轮 +" + std::to_string(prev_new) + ")",
                {
                    {"current_added", 0},
                    {"prev_added", prev_new}
                }
            ));
        }
    } else if (total_new == 0 && history.empty()) {
        // 第 1 轮就没新增
        notices.push_back(make_notice(
            "growth_stalled",
            "生长停滞:首轮执行无新增节点",
            {{"current_added", 0}, {"reason", "first_tick_empty"}}
        ));
    }

    // ── 3. 节点修剪通知:本轮有节点被移除 ──
    if (removed_count > 0) {
        json pruned_nodes = json::array();
        for (const auto& rn : diff.value("removed_nodes", json::array())) {
            pruned_nodes.push_back(rn.value("canonical_name", rn.value("entity_id","")));
        }
        notices.push_back(make_notice(
            "node_pruned",
            "本轮修剪 " + std::to_string(removed_count) + " 个低相关性节点",
            {{"pruned_nodes", pruned_nodes}, {"reason", "相关性低于阈值"}}
        ));
    }

    // ── 4. 关系解除通知 ──
    if (removed_rels > 0) {
        notices.push_back(make_notice(
            "relation_broken",
            "本轮解除 " + std::to_string(removed_rels) + " 条节点关系",
            {{"removed_count", removed_rels}}
        ));
    }

    return notices;
}

// ============================================================================
// 第 3 阶段:重要消息提醒 (level="important") + Flash 异动 (level="flash")
// ============================================================================
namespace {
// 算节点度数(从 current snapshot 的 relations 里数)
std::map<std::string, int> compute_degree(const json& current) {
    std::map<std::string, int> deg;
    for (const auto& r : current.value("relations", json::array())) {
        deg[r.value("source_id","")]++;
        deg[r.value("target_id","")]++;
    }
    return deg;
}
// 从 current 提取 node_id → status 映射
std::map<std::string, std::string> node_status_map(const json& current) {
    std::map<std::string, std::string> m;
    for (const auto& n : current.value("nodes", json::array()))
        m[n.value("entity_id","")] = n.value("sprawl_status","unknown");
    return m;
}
// 提取 entity_type (type:canonical)
std::string entity_type_of(const std::string& entity_id) {
    size_t c = entity_id.find(':');
    return c != std::string::npos ? entity_id.substr(0, c) : "unknown";
}
} // anon namespace

// ── 重要消息提醒:3 类 ──────────────────────────────────────────
json detect_important_notices(const json& diff,
                              const json& growth_report,
                              const json& current,
                              const std::vector<json>& history) {
    json notices = json::array();

    auto mk = [](const std::string& type, const std::string& msg,
                 const json& detail = json::object()) -> json {
        json n;
        n["level"] = "important";
        n["type"] = type;
        n["message"] = msg;
        n["detail"] = detail;
        return n;
    };

    int added_count   = diff.value("stats", json::object()).value("added_count", 0);
    int removed_count = diff.value("stats", json::object()).value("removed_count", 0);

    // ── 1. 持续停滞:连续 2 轮无新增 ──
    // history 里 history[0] = 上一轮,history[1] = 上上轮
    if (added_count == 0 && history.size() >= 1) {
        int prev_added = history[0].value("diff_stats", json::object()).value("added_count", -1);
        if (prev_added == 0) {
            notices.push_back(mk(
                "continuous_stall",
                "持续停滞:连续 2 轮无新增节点,生长卡顿",
                {{"current_added", 0}, {"prev_added", 0}, {"prev_tick", history[0].value("tick_index", -1)}}
            ));
        }
    }

    // ── 2. 大规模修剪:removed > added × 1.2 ──
    if (removed_count > 0 && (double)removed_count > (double)added_count * 1.2) {
        notices.push_back(mk(
            "mass_pruning",
            "大规模修剪:本轮修剪 " + std::to_string(removed_count) +
            " 个节点(新增 " + std::to_string(added_count) + "),领域方向可能偏转",
            {{"removed", removed_count}, {"added", added_count}, {"ratio", (double)removed_count / std::max(1, added_count)}}
        ));
    }

    // ── 3. 核心节点变动:Top 5 枢纽节点被修剪或降级 ──
    // 核心 = 度数 Top 5 (当前快照),再检查这些节点在 diff 里是否被移除或状态变更
    auto deg = compute_degree(current);
    std::vector<std::pair<int, std::string>> sorted_deg;
    for (const auto& [eid, d] : deg) sorted_deg.push_back({d, eid});
    std::sort(sorted_deg.begin(), sorted_deg.end(), std::greater<>());

    std::set<std::string> top5;
    int n_top = std::min(5, (int)sorted_deg.size());
    for (int i = 0; i < n_top; ++i) top5.insert(sorted_deg[i].second);

    // 检查 diff.removed_nodes 和 diff.status_changed 是否命中
    json affected = json::array();
    for (const auto& rn : diff.value("removed_nodes", json::array())) {
        if (top5.count(rn.value("entity_id","")))
            affected.push_back({{"entity_id", rn.value("entity_id","")}, {"canonical_name", rn.value("canonical_name","")}, {"event", "removed"}});
    }
    for (const auto& sc : diff.value("status_changed", json::array())) {
        if (top5.count(sc.value("entity_id",""))) {
            // 降级 = 从 active/boundary → exhausted/pruned
            std::string ns = sc.value("new_status","");
            std::string os = sc.value("old_status","");
            bool downgraded = (ns == "exhausted" || ns == "pruned") && (os == "active" || os == "boundary" || os == "seed");
            if (downgraded)
                affected.push_back({{"entity_id", sc.value("entity_id","")}, {"canonical_name", sc.value("canonical_name","")}, {"event", os + "->" + ns}});
        }
    }
    if (!affected.empty()) {
        notices.push_back(mk(
            "core_node_downgrade",
            "核心节点变动:Top " + std::to_string(n_top) + " 枢纽中有 " +
            std::to_string(affected.size()) + " 个被修剪或状态降级",
            {{"affected", affected}, {"top5_count", n_top}}
        ));
    }

    return notices;
}

// ── Flash 异动检测:4 类 ────────────────────────────────────────
json detect_flash_events(const json& diff,
                         const json& growth_report,
                         const json& current,
                         const std::vector<json>& history) {
    json flashes = json::array();

    auto mk = [](const std::string& type, const std::string& msg,
                 const json& detail = json::object()) -> json {
        json n;
        n["level"] = "flash";
        n["type"] = type;
        n["message"] = msg;
        n["detail"] = detail;
        return n;
    };

    auto deg = compute_degree(current);

    // ── 1. 跨领域通路:新增关系两端 entity_type 不同,且各自有 ≥2 节点 ──
    // 先统计 current 里每个 type 的节点数
    std::map<std::string, int> type_count;
    for (const auto& n : current.value("nodes", json::array()))
        type_count[n.value("type","unknown")]++;

    for (const auto& r : diff.value("added_relations", json::array())) {
        std::string s = r.value("source_id","");
        std::string t = r.value("target_id","");
        std::string stype = entity_type_of(s);
        std::string ttype = entity_type_of(t);
        if (stype != ttype &&
            type_count[stype] >= 2 && type_count[ttype] >= 2) {
            flashes.push_back(mk(
                "cross_domain_edge",
                "跨领域通路打通:" + stype + " ↔ " + ttype + " 首次建立直接关系",
                {{"source", s}, {"target", t}, {"relation_type", r.value("relation_type","")},
                  {"domain_a", stype}, {"domain_b", ttype}}
            ));
        }
    }

    // ── 2. 枢纽节点接入:新增节点在当前快照里度数 > 5 或相关性 > 0.8 ──
    for (const auto& n : diff.value("added_nodes", json::array())) {
        std::string eid = n.value("entity_id","");
        int d = deg[eid];
        double rel = n.value("relevance", 0.0);
        if (d > 5 || rel > 0.8) {
            flashes.push_back(mk(
                "hub_node_join",
                "枢纽节点接入:新增节点度数 " + std::to_string(d) +
                " / 相关性 " + std::to_string(rel).substr(0, 4),
                {{"entity_id", eid}, {"canonical_name", n.value("canonical_name","")},
                  {"degree", d}, {"relevance", rel}}
            ));
        }
    }

    // ── 3. 生长方向突变:本轮 70%+ 新增集中在前序占比 < 20% 的方向 ──
    // 前序方向分布 = history[0].growth_report.direction_distribution(history[0] 是上一轮)
    json cur_dir = growth_report.value("direction_distribution", json::object());
    json prev_dir;
    int prev_total = 0;
    if (!history.empty()) {
        prev_dir = history[0].value("growth_report", json::object()).value("direction_distribution", json::object());
        for (auto it = prev_dir.begin(); it != prev_dir.end(); ++it)
            prev_total += it.value();
    } else {
        // 无历史:用 current.snapshot_stats 的 depth_distribution 近似
        prev_dir = current.value("stats", json::object()).value("depth_distribution", json::object());
    }

    int cur_total = 0;
    for (auto it = cur_dir.begin(); it != cur_dir.end(); ++it) cur_total += it.value();

    if (cur_total >= 3 && prev_total > 0) {
        // 找本轮各方向中,在前序占比 < 20% 的那些方向
        int concentrated = 0;
        json shift_directions = json::array();
        for (auto it = cur_dir.begin(); it != cur_dir.end(); ++it) {
            std::string dir = it.key();
            int cur_n = it.value();
            int prev_n = prev_dir.value(dir, 0);
            double prev_share = (double)prev_n / prev_total;
            if (prev_share < 0.2 && cur_n > 0) {
                concentrated += cur_n;
                shift_directions.push_back({{"direction", dir}, {"added", cur_n}, {"prev_share", prev_share}});
            }
        }
        double ratio = (double)concentrated / cur_total;
        if (ratio >= 0.7 && !shift_directions.empty()) {
            flashes.push_back(mk(
                "growth_direction_shift",
                "生长方向突变:" + std::to_string((int)(ratio * 100)) +
                "% 新增集中在前序冷门方向",
                {{"concentration_ratio", ratio}, {"shift_directions", shift_directions}}
            ));
        }
    }

    // ── 4. 竞争格局:两个高权重活跃节点之间首次建立 competes_with ──
    // 高权重 = 度数 ≥ 3 且 status ∈ {active, seed, boundary}
    auto status_map = node_status_map(current);
    std::set<std::string> prev_rel_keys;
    for (const auto& h : history) {
        for (const auto& r : h.value("added_rels", json::array()))
            prev_rel_keys.insert(r.value("source_id","") + "|" + r.value("relation_type","") + "|" + r.value("target_id",""));
        for (const auto& r : h.value("removed_relations", json::array()))
            prev_rel_keys.insert(r.value("source_id","") + "|" + r.value("relation_type","") + "|" + r.value("target_id",""));
    }

    for (const auto& r : diff.value("added_relations", json::array())) {
        std::string rtype = r.value("relation_type","");
        if (rtype != "competes_with") continue;
        std::string s = r.value("source_id","");
        std::string t = r.value("target_id","");
        if (deg[s] < 3 || deg[t] < 3) continue;
        auto is_high_weight = [&](const std::string& id) {
            auto it = status_map.find(id);
            if (it == status_map.end()) return false;
            return it->second == "active" || it->second == "seed" || it->second == "boundary";
        };
        if (!is_high_weight(s) || !is_high_weight(t)) continue;

        flashes.push_back(mk(
            "competition_form",
            "竞争格局形成:" + entity_type_of(s) + " ↔ " + entity_type_of(t) +
            " 高权重活跃节点首次 competes_with",
            {{"source", s}, {"target", t}, {"deg_source", deg[s]}, {"deg_target", deg[t]}}
        ));
    }

    return flashes;
}

// ============================================================================
// 事件研判层
// ============================================================================

// ── 基础信息卡:一句话说明节点是什么 ─────────────────────────────
json make_node_card(const json& node) {
    json card;
    std::string etype = node.value("type", "unknown");
    std::string canonical = node.value("canonical_name",
                           node.value("entity_id", "?"));
    double rel = node.value("relevance", 0.0);
    int depth = node.value("depth", 0);
    std::string status = node.value("sprawl_status", "");

    card["entity_id"]    = node.value("entity_id", "");
    card["canonical_name"] = canonical;
    card["type"]         = etype;
    card["relevance"]    = rel;
    card["depth"]        = depth;
    card["sprawl_status"] = status;

    // 一句话摘要
    std::string summary = canonical + " (" + etype;
    summary += " | depth=" + std::to_string(depth);
    summary += " | relevance=" + std::to_string(rel).substr(0, 4) + ")";
    if (!status.empty()) summary += " [" + status + "]";
    card["summary"] = summary;

    // 按 type 专项标签
    if (etype == "paper")      card["domain_tag"] = "学术论文";
    else if (etype == "repo") card["domain_tag"] = "开源项目";
    else if (etype == "person") card["domain_tag"] = "领域人物";
    else if (etype == "topic") card["domain_tag"] = "话题事件";
    else                      card["domain_tag"] = etype;

    return card;
}

// ── 四维度通用研判 ─────────────────────────────────────────────
namespace {

// 1. 领域定位:用度数 + 深度 + 是否在主干道 → 核心/主干道/分支/边缘
std::string judge_domain_position(const std::string& entity_id,
                                   const json& current,
                                   const json& main_roads) {
    // 算度数
    int d = 0;
    for (const auto& r : current.value("relations", json::array())) {
        if (r.value("source_id","") == entity_id) d++;
        if (r.value("target_id","") == entity_id) d++;
    }

    // 算深度
    int depth = 0;
    for (const auto& n : current.value("nodes", json::array())) {
        if (n.value("entity_id","") == entity_id) { depth = n.value("depth", 0); break; }
    }

    // 是否在主干道上
    bool on_main_road = false;
    for (const auto& road : main_roads.value("main_roads", json::array())) {
        for (const auto& pid : road.value("path", json::array())) {
            if (pid == entity_id) { on_main_road = true; break; }
        }
        if (on_main_road) break;
    }

    if (d >= 5 && on_main_road)      return "核心节点 (度数≥5 + 在主干道上)";
    if (on_main_road)                return "主干道节点";
    if (d >= 3 && depth <= 2)        return "分支节点 (度数≥3,深度≤2)";
    if (depth >= 3 || d <= 1)        return "边缘节点";
    return "分支节点";
}

// 2. 事件性质:按 notice.type 映射
std::string judge_event_nature(const std::string& notice_type,
                                const json& detail) {
    static const std::map<std::string, std::string> m = {
        {"path_exhausted",      "路径到界 (现有技术已遍历完毕)"},
        {"growth_stalled",      "瓶颈信号 (本轮无新增)"},
        {"continuous_stall",    "瓶颈信号 (连续 2 轮无新增)"},
        {"node_pruned",         "常规迭代 (低相关性修剪)"},
        {"mass_pruning",        "格局变动 (大规模修剪)"},
        {"core_node_downgrade", "技术路线格局变动 (核心节点降级)"},
        {"relation_broken",     "常规迭代 (关系解除)"},
        {"cross_domain_edge",   "技术突破 (跨领域首次通路)"},
        {"hub_node_join",       "格局变动 (枢纽节点接入)"},
        {"growth_direction_shift","路线分叉 (生长方向突变)"},
        {"competition_form",    "格局变动 (竞争格局形成)"},
    };
    auto it = m.find(notice_type);
    return it != m.end() ? it->second : "未分类事件";
}

// 3. 影响半径:关联节点数 + 是否连通主干道
std::string judge_impact_radius(const std::string& entity_id,
                                 const json& current,
                                 const json& main_roads) {
    // 下游关联 = 直接邻居
    std::set<std::string> neighbors;
    for (const auto& r : current.value("relations", json::array())) {
        if (r.value("source_id","") == entity_id) neighbors.insert(r.value("target_id",""));
        if (r.value("target_id","") == entity_id) neighbors.insert(r.value("source_id",""));
    }

    // 是否连通主干道
    bool connected_to_main = false;
    for (const auto& road : main_roads.value("main_roads", json::array())) {
        for (const auto& pid : road.value("path", json::array())) {
            if (neighbors.count(pid) || pid == entity_id) {
                connected_to_main = true; break;
            }
        }
        if (connected_to_main) break;
    }

    int n = (int)neighbors.size();
    if (connected_to_main && n >= 3)    return "全局影响 (连通主干道,波及" + std::to_string(n) + "个关联节点)";
    if (connected_to_main)              return "主干道影响 (连通主干道)";
    if (n >= 3)                         return "分支影响 (波及" + std::to_string(n) + "个关联节点)";
    return "局部影响 (孤立叶子节点)";
}

// 4. 趋势信号:连续 3 轮历史对比
std::string judge_trend_signal(const json& notice,
                                const std::vector<json>& history) {
    if (history.empty()) return "首次出现 (无历史对比)";

    std::string ntype = notice.value("type", "");
    int cur_added = notice.value("detail", json::object()).value("current_added", -1);

    // 简单版本:看历史里同类型事件是否连续出现
    int same_type_count = 0;
    for (const auto& h : history) {
        for (const auto& n : h.value("notices", json::array())) {
            if (n.value("type","") == ntype) same_type_count++;
        }
    }

    if (same_type_count >= 2) return "持续信号 (历史中连续出现" + std::to_string(same_type_count) + "次)";
    if (same_type_count == 1) return "趋势拐点 (历史中出现过 1 次,本轮再次触发)";
    return "孤立事件 (历史中未出现过同类型事件)";
}

} // anon namespace

// ── 主入口:遍历 notices,按 level 叠加研判 ────────────────────
json judge_notices(const json& notices,
                   const json& diff,
                   const json& growth_report,
                   const json& current,
                   const std::vector<json>& history) {
    auto main_roads = parse_situation(diff, current); // 主干道需要重算一次(无额外成本)

    json result = json::array();
    for (auto notice : notices) {
        std::string level = notice.value("level", "normal");
        std::string ntype = notice.value("type", "");
        json detail = notice.value("detail", json::object());

        if (level == "flash" || level == "important") {
            // ── 深度研判:四维度完整 ──
            std::string eid = detail.value("entity_id",
                              detail.value("source",
                              detail.value("target", "")));

            json judgment;
            judgment["level"] = level;

            // 领域定位
            if (!eid.empty()) {
                judgment["domain_position"] = judge_domain_position(eid, current, main_roads);
            } else {
                judgment["domain_position"] = "领域级事件 (不绑定单一节点)";
            }

            // 事件性质
            judgment["event_nature"] = judge_event_nature(ntype, detail);

            // 影响半径
            if (!eid.empty()) {
                judgment["impact_radius"] = judge_impact_radius(eid, current, main_roads);
            } else {
                judgment["impact_radius"] = "全局影响 (领域级事件)";
            }

            // 趋势信号
            judgment["trend_signal"] = judge_trend_signal(notice, history);

            // 参考判断 (仅 flash 级别)
            if (level == "flash") {
                std::string ref;
                if (ntype == "cross_domain_edge")
                    ref = "跨领域通路打通,可能预示着新的技术融合方向出现,建议重点关注";
                else if (ntype == "hub_node_join")
                    ref = "枢纽节点接入,该节点在领域中地位重要,建议立即纳入主干道追踪";
                else if (ntype == "growth_direction_shift")
                    ref = "生长方向突变,资源正在向新方向倾斜,原方向权重可能下降";
                else if (ntype == "competition_form")
                    ref = "竞争格局形成,两条技术路线首次直接对抗,后续可能出现技术分叉或融合";
                else
                    ref = "重要信号,建议持续追踪后续演变";
                judgment["reference_judgement"] = ref;
            }

            notice["judgement"] = judgment;

        } else if (level == "normal") {
            // ── 标准研判:性质 + 简要影响 ──
            std::string eid = detail.value("entity_id", "");
            json summary;
            summary["event_nature"] = judge_event_nature(ntype, detail);
            summary["brief_impact"] = !eid.empty()
                ? judge_impact_radius(eid, current, main_roads)
                : "局部影响";
            notice["summary_judgement"] = summary;

            // 关联节点的基础信息卡
            if (!eid.empty()) {
                for (const auto& n : current.value("nodes", json::array())) {
                    if (n.value("entity_id","") == eid) {
                        notice["node_card"] = make_node_card(n);
                        break;
                    }
                }
            }
        }

        result.push_back(std::move(notice));
    }
    return result;
}

} // namespace situation_engine
} // namespace github_research
