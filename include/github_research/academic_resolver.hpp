#pragma once
// ============================================================
// Academic Resolver — 学术源统一五级降级调度器
//
// 解决的问题:
//   1. ID 失效 (arXiv / S2 paper_id 不存在)
//   2. S2 429 限流 (自动退避重试 + 跨源降级)
//   3. 精确标题搜索零命中 (核心术语窄搜)
//   4. 所有学术源失败 (线索留存, 不污染主库)
//
// 五级链路 (每降一级 confidence -0.1):
//   L1 原ID调用           confidence=1.0   formal
//   L2 精确标题检索       confidence=0.9   formal
//   L3 核心术语窄搜       confidence=0.8   formal
//   L4 跨源交叉验证       confidence=0.7   candidate
//   L5 线索留存           confidence<0.6   clue_only (不实体化)
//
// 锚点硬化规则 (实体入库前强制校验):
//   必须同时具备: title (string) + authors (非空数组) + year (int)
//   缺失任一 → 自动降级为 candidate 或线索
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

enum class DegradationLevel {
    L1_ORIGINAL_ID = 1,
    L2_EXACT_TITLE = 2,
    L3_CORE_TERMS  = 3,
    L4_CROSS_SOURCE = 4,
    L5_CLUE_ONLY   = 5
};

enum class AcademicSource {
    S2,
    ARXIV
};

// 标准化论文结果 — 所有降级路径最终输出同一 schema
struct PaperResult {
    bool found = false;
    bool is_formal_entity = false;       // confidence >= 0.8
    bool is_candidate = false;           // 0.6 <= confidence < 0.8
    bool is_clue_only = false;           // confidence < 0.6

    double confidence = 0.0;              // 0.0 ~ 1.0
    DegradationLevel level = DegradationLevel::L5_CLUE_ONLY;
    AcademicSource resolved_source = AcademicSource::S2;

    // 硬锚点三要素
    std::string title;
    std::vector<std::string> authors;
    int year = 0;

    // 可选补充字段
    std::string abstract;
    std::string venue;
    std::string external_id;              // S2 paper_id / arXiv ID
    std::string url;
    int citation_count = 0;
    std::vector<std::string> fields_of_study;

    // 溯源
    std::string fallback_chain;           // "L1_ORIGINAL_ID→L2_EXACT_TITLE→L3_CORE_TERMS"
    json raw_source_payload;              // 原始 API 响应片段, 便于调试
};

// ============================================================
// 统一入口: 给定 source + args (可含 paper_id / arxiv_id / title / core_terms)
// 返回 PaperResult
// ============================================================
PaperResult resolvePaper(AcademicSource source, const json& args);

// 辅助: 判断三个硬锚点是否齐全
bool hasHardAnchors(const PaperResult& r);

// 辅助: 从降级链字符串构造人类可读描述
std::string degradationLabel(DegradationLevel lvl);

} // namespace github_research
