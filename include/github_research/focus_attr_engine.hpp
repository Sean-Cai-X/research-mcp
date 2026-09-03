#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace github_research {

using json = nlohmann::json;

// ═════════════════════════════════════════════════════════════
//  属性提取 + 缺口检测引擎 (next2.txt 第 2 步)
// ═════════════════════════════════════════════════════════════

// ── 属性模板:不同实体类型需要提取哪些 key ──────────────────
std::vector<std::string> attr_template_for(const std::string& entity_type);

// ── 从实体已有信息中提取属性(窄操作,每次 1-2 个 key) ───────
// 返回本轮实际提取的属性数量
int extract_attributes_for_entity(const std::string& entity_id,
                                   const std::string& entity_type,
                                   int max_keys_per_pass = 3);

// ── 批量提取:对 focus 下所有 active 节点做一轮属性提取 ──────
json run_extract_tick(const std::string& focus_id,
                       int max_entities_per_tick = 10,
                       bool dry_run = false);

// ── 合并属性:跨来源提升置信度 ────────────────────────────────
// 同一 entity_id + attr_key 有多个来源时,如果值一致则提升 confidence
int merge_attributes_for_entity(const std::string& entity_id);

// ── 缺口检测:找出属性缺失的实体 ──────────────────────────────
// 返回 [{entity_id, entity_type, missing_keys[], coverage_ratio}]
std::vector<json> detect_gaps(const std::string& focus_id);

// ── 为缺口实体生成提取任务(写入 gaps 表) ────────────────────
int schedule_gap_extractions(const std::string& focus_id);

} // namespace github_research