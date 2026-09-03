#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// ── 蔓延引擎:定向知识雷达的核心调度算法 ──────────────────────
// next2.txt "第1步:蔓延引擎骨架"
//
// 一轮 tick 执行流程:
//   1. 取 active 节点,按 relevance/check_count 排序 TOP N
//   2. 对每个选中节点,发现邻居(调用现有检索工具定向查询)
//   3. 对每个候选邻居,算相关性(多维度加权 + 深度衰减)
//   4. 分类入队(active / boundary / pruned / exhausted)
//   5. 更新状态(check_count++, 连续 3 轮无新邻居→exhausted)

namespace focus_engine {

// ── 关系类型权重(relation_type_weight) ─────────────────────
double relation_type_weight(const std::string& rel_type);

// ── 多字段关键词重叠得分 ─────────────────────────────────────
int score_keyword_overlap_multi(const json& entity_candidate,
                                const std::vector<std::string>& keywords);

// ── 多维度相关性评分 ────────────────────────────────────────
// relevance = 0.4 * keyword_overlap_norm
//           + 0.3 * relation_type_weight
//           + 0.2 * source_trust
//           × depth_decay^depth
double compute_relevance(const json& focus,
                          const json& entity_candidate,
                          const std::string& relation_type,
                          int depth,
                          const std::string& source_id);

// ── 分类结果 ────────────────────────────────────────────────
struct ClassifyResult {
    std::string status;     // active | boundary | pruned
    double relevance;
    std::string reason;
};

// ── 分类入队 ────────────────────────────────────────────────
ClassifyResult classify_member(double relevance,
                                int depth,
                                const json& focus,
                                int existing_node_count);

// ── 发现邻居(定向查询) ──────────────────────────────────────
std::vector<json> discover_neighbors(const std::string& entity_id,
                                      const std::string& entity_type,
                                      const std::string& canonical_name);

// ── 执行一轮蔓延 tick ────────────────────────────────────────
json run_sprawl_tick(const std::string& focus_id,
                      int max_nodes_per_tick = 5,
                      bool dry_run = false);

// ── 估算相关性(调试用) ──────────────────────────────────────
json estimate_relevance(const std::string& focus_id,
                         const json& entity_candidate,
                         const std::string& relation_type = "",
                         int depth = 1);

} // namespace focus_engine
} // namespace github_research
