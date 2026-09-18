#pragma once

#include "github_research/datasource_registry.hpp"
#include "github_research/cache_manager.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace github_research {

// =============================================================
// Wiki Mining Pipeline — 零命中自动兜底深度挖掘
//
// 触发: wiki_discover 返回 resources.empty()
// 能力: 复用 9 源检索 + Kiwix expand + CacheManager 实体注册
// 产出: 标准化实体 + 分级缓存, 与原生 Wiki 条目格式对齐
//
// 第一阶段 (v0.4): 路径1 分类树向下遍历 + 路径2 别名/重定向挖掘
// 后续阶段: 路径3 全文定位 + 路径4 跨源反向 + 路径5 邻近扩散
// =============================================================

// 挖掘产出的候选实体
struct MinedEntity {
    std::string entity_id;          // gen_entity_id("wiki_mined", canonical_name)
    std::string canonical_name;     // 规范名
    std::vector<std::string> aliases; // 别名/变体
    std::string domain_tag;         // 领域标签 (kernel / memory / networking / ...)
    std::string summary;            // 1-2 句定义摘要
    std::string canonical_uri;      // 最终锚定的 wiki 条目
    std::string source_path;        // 挖掘路径: "category_tree" / "alias_redirect"
    double confidence = 0.0;         // 0.0 ~ 1.0
    bool is_formal = false;         // true → entity_index, false → cache_entries
};

// 调度配置
struct MiningConfig {
    int max_paths = 4;              // 第二阶段加路径3/4
    int max_hits_per_path = 10;
    bool enable_category_tree = true;
    bool enable_alias_redirect = true;
    bool enable_fulltext_scan = true;    // 路径3 全文上下文定位
    bool enable_cross_source_map = true;  // 路径4 跨源反向映射
    bool enable_jieba_segment = true;     // 启用 cppjieba 真结巴 (vs 规则分词)
    int category_scan_depth = 2;
    int alias_variant_limit = 6;
    double formal_threshold = 0.8;
    int fail_ttl_hours = 24;
};

class WikiMiningPipeline {
public:
    WikiMiningPipeline(DataSourceRegistry& registry, CacheManager& cache);

    // 主入口: discover 零命中时调用
    // 返回可直接合入 discover resources 的条目 (可能为空)
    json mine(const std::string& query,
              const std::string& context_focus = "",
              const MiningConfig& config = MiningConfig{});

private:
    DataSourceRegistry& registry_;
    CacheManager& cache_;

    // —— 触发判断 ——
    bool shouldMine(const std::string& query);
    bool isInFailCache(const std::string& query);
    void recordFail(const std::string& query, const std::string& reason);

    // —— 路径 1: 分类树向下遍历 ——
    // 先找上位分类页面 (关键词匹配), 再 expand 出子页面, 在其中匹配目标术语
    std::vector<MinedEntity> pathCategoryTree(const std::string& query,
                                               const MiningConfig& config);

    // —— 路径 2: 别名与重定向挖掘 ——
    // 生成术语变体, 批量 discover, 命中时 read 检查是否重定向/消歧义
    std::vector<MinedEntity> pathAliasRedirect(const std::string& query,
                                                const MiningConfig& config);

    // —— 路径 3: 全文上下文定位 ——
    // 用 Kiwix search 做全文检索, 从 snippet 反推所属条目 + 领域归类
    std::vector<MinedEntity> pathFulltextScan(const std::string& query,
                                               const MiningConfig& config);

    // —— 路径 4: 跨源反向映射 ——
    // github / hn / arxiv 搜项目/论文 → 从 description/readme 反推 Wiki 领域锚点
    std::vector<MinedEntity> pathCrossSourceMap(const std::string& query,
                                                 const MiningConfig& config);

    // —— 路径辅助 ——
    std::vector<std::string> generateVariants(const std::string& query, int limit);
    std::vector<std::string> guessParentCategories(const std::string& query);

    // —— 结构化校准 ——
    MinedEntity calibrate(const std::string& query,
                          const std::string& hit_title,
                          const std::string& hit_uri,
                          const std::string& hit_content,
                          const std::string& source_path,
                          double base_confidence);

    // —— 落库 ——
    void persist(const std::vector<MinedEntity>& entities);
    json toResourceJson(const MinedEntity& e);

    // —— 常量 ——
    static constexpr const char* FAIL_CACHE_TYPE = "wiki_mining_fail";
    static constexpr const char* MINED_CACHE_TYPE = "wiki_mined";
    static constexpr const char* ENTITY_TYPE = "wiki_concept";
};

} // namespace github_research
