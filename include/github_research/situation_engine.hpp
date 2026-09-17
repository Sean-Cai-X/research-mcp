#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// ── 态势感知引擎:绑定 focus_sprawl_tick 每轮执行周期 ──────────
// 四层执行流程:
//   第一层 snapshot_baseline / snapshot_current
//   第二层 compute_diff
//   第三层 parse_situation (生长报告 + 主干道 Top3)
//   第四层 (由调用方决定输出形式)

namespace situation_engine {

// ── 关系类型权重(与 focus_engine 保持一致,主干道权重计算用) ──
double relation_weight(const std::string& rel_type);

// ── 第一层:对焦点域做全量状态快照 ──────────────────────────
// 返回 json 包含:
//   nodes: [{entity_id, canonical_name, type, depth, relevance, sprawl_status, check_count}]
//   relations: [{source_id, target_id, relation_type, weight}]
//   stats: {total_nodes, active, boundary, exhausted, pruned, seed, total_relations, depth_distribution}
//   timestamp: unix seconds
json snapshot_focus(const std::string& focus_id);

// ── 第二层:差分计算 ────────────────────────────────────────
// 对比 baseline 和 current,返回:
//   added_nodes:   [{entity_id, name, type, depth, relevance,接入父节点}]
//   removed_nodes:  [{entity_id, name, 修剪原因}]
//   status_changed:[{entity_id, old_status, new_status, reason}]
//   added_relations:[{source, target, type, weight}]
//   removed_relations:[{source, target, type}]
json compute_diff(const json& baseline, const json& current);

// ── 第三层:态势解析 ────────────────────────────────────────
// 输入 diff + current snapshot,输出结构化态势报告:
//   growth_report: {新增边界,新增活跃,修剪,生长方向分布,关键路径}
//   main_roads:    [{path: [node_ids], weight, 覆盖节点数, 贯穿领域}] Top 3
json parse_situation(const json& diff, const json& current);

// ── 便捷入口:一轮完整态势快照+差分+解析 ──────────────────
// 调用方先调用 snapshot_focus(baseline) → sprawl_tick → snapshot_focus(current)
// 然后调用此函数一次性完成 diff + parse,直接返回可输出报告
json build_situation_report(const json& baseline, const json& current);

// ── 第 2 阶段:普通消息提醒检测 ──────────────────────────
// 输入:本轮 diff + growth_report + 历史 tick 列表(最近 N 轮)
// 输出:json array,每条 notice = {level:"normal", type, message, detail}
//
// 4 类普通提醒:
//   path_exhausted   — 路径耗尽:末端节点 exhausted
//   growth_stalled   — 生长停滞:连续 1 轮无新增
//   node_pruned      — 节点修剪通知:本轮有修剪
//   relation_broken  — 关系解除通知:本轮有 rel 删除
json detect_normal_notices(const json& diff,
                           const json& growth_report,
                           const std::vector<json>& history);

// ── 第 3 阶段:重要消息提醒 + Flash 异动 ──────────────────────
//
// 重要消息提醒 (level="important", 3 类):
//   continuous_stall     — 持续停滞:连续 2 轮无新增
//   mass_pruning         — 大规模修剪:removed > added × 1.2
//   core_node_downgrade  — 核心节点变动:Top 5 枢纽被修剪/降级
//
// Flash 异动 (level="flash", 4 类):
//   cross_domain_edge    — 跨领域通路:两个不同领域首次直接关联
//   hub_node_join        — 枢纽节点接入:新增节点预估度数 > 5 或相关性 > 0.8
//   growth_direction_shift — 生长方向突变:70%+ 新增集中在前序 < 20% 的方向
//   competition_form     — 竞争格局:两个高权重活跃节点首次建立 competes_with
json detect_important_notices(const json& diff,
                              const json& growth_report,
                              const json& current,
                              const std::vector<json>& history);
json detect_flash_events(const json& diff,
                         const json& growth_report,
                         const json& current,
                         const std::vector<json>& history);

// ── 事件研判层:通用四维度 + 分类型专项 ──────────────────────
// 输入:一条 notice (level/type/detail) + 完整上下文(diff/current/history)
// 输出:带研判的 notice — 在原 notice 基础上叠加:
//   level="normal"      → notice_node_card (一句话摘要)
//   level="important"   → notice_judgement (领域定位 + 事件性质 + 影响半径)
//   level="flash"       → deep_judgement (四维度完整研判 + 参考判断)
json judge_notices(const json& notices,
                   const json& diff,
                   const json& growth_report,
                   const json& current,
                   const std::vector<json>& history);

// 单个节点的基础信息卡 (第一阶段,随增量生长报告)
json make_node_card(const json& node);

} // namespace situation_engine
} // namespace github_research
