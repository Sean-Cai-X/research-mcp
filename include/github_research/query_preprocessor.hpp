#pragma once

#include <string>
#include <vector>

namespace github_research {

// =============================================================
// Query Preprocessor — 全源公共查询预处理层
//
// 起源: wiki_mining.cpp 内嵌 static 函数组 (query_preprocessor namespace)
// 目的: 提升到 src/common/, 让 arxiv_search / s2_search / 其他搜索工具
//       也能用分词 + 拼写容错 + 同义词变体生成能力
//
// 风险控制 (硬约束, 用户明确要求):
//   - 编辑距离阈值 永远 ≤ 1 字符 (len<5 不容忍, 5-8 允许 1, >8 也只允许 1)
//     原来 len>8 允许 2 的逻辑已在抽公共模块时收窄
//   - fuzzy 命中方 调用方负责 confidence - 0.1 + _source.match_type = "fuzzy"
//
// 依赖: string_utils.hpp (to_lower)
// 可选: cppjieba (通过 __has_include + RESEARCH_MCP_HAS_CPPJIEBA 宏)
// =============================================================

struct VariantSource {
    std::string variant;
    std::string source_tag;   // "original" / "exact_norm" / "token_combo" / "core_token" / "fuzzy_lev1" / "synonym"
};

// 分级查询组合生成 — 唯一对外入口
// 输出队列按优先级从高到低排列 (original 最高, fuzzy/synonym 最低)
// context_focus: 可选领域上下文, 用于跨领域分词 (arxiv / s2 传空串即可)
std::vector<VariantSource> buildQueryQueue(const std::string& raw_query,
                                            const std::string& context_focus = "");

// 快速检查: 这个变体是否属于 fuzzy / synonym 来源 (用于风险控制)
bool isFuzzySource(const std::string& source_tag);

// 核心能力暴露 (高级调用方可选直接使用)
std::vector<std::string> rule_tokenize(const std::string& input);
std::vector<std::string> jieba_normalize(const std::string& term);
int levenshteinDistance(const std::string& a, const std::string& b);
int maxEditDistance(const std::string& term);  // 永远 ≤ 1

} // namespace github_research
