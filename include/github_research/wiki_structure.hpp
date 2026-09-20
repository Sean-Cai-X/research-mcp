#pragma once

#include "github_research/datasource_registry.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>

namespace github_research {

// =============================================================
// Wiki Structure Miner — 内源结构信号提取
//
// 区别于 WikiMiningPipeline (兜底路径):
//   mining = discover 零命中时的被动兜底
//   structure = 主动挖掘分类树/链接网络/重定向关系
//
// 四类结构信号:
//   1. Category Graph — 分类树递归 + is_a 层级边
//   2. Link Network    — 内部链接 BFS + 核心度 (in-degree / PageRank-lite)
//   3. Redirect Trace  — 重定向检测 + 消歧义页识别
//   (编辑活跃度放弃 — Kiwix ZIM 是静态快照, 无历史版本)
// =============================================================

struct GraphNode {
    std::string canonical_uri;     // kiwix://zim_id/Article_path
    std::string title;              // 从 HTML <title> 或 h1 提取
    std::string node_type;          // "category" / "article" / "redirect" / "disambiguation"
    int depth = 0;                  // BFS 深度
    double core_score = 0.0;        // 核心度 (link graph 计算)
    int in_degree = 0;              // 被链接次数
    int out_degree = 0;             // 链接出去次数
};

struct GraphEdge {
    std::string from;               // canonical_uri
    std::string to;
    std::string edge_type;          // "is_a" / "link" / "redirects_to"
};

struct CategoryGraph {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;       // is_a 关系
    std::string root_category;
    int max_depth_reached = 0;
    int category_count = 0;
    int article_count = 0;
};

struct LinkGraph {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;       // link 关系
    std::string center_uri;
    int depth = 1;
};

struct RedirectInfo {
    bool is_redirect = false;
    std::string redirect_target;       // 最终指向的 canonical_uri
    bool is_disambiguation = false;
    std::vector<std::string> disambig_targets;  // 消歧义候选
    std::string detected_from_html;     // 检测依据: "#REDIRECT" / "mw-redirect" / "disambiguation"
};

class WikiStructure {
public:
    explicit WikiStructure(DataSourceRegistry& registry);

    // —— 工具 1: 分类树递归 + is_a 边 ——
    // root_category: 分类名, 如 "Machine_learning" (不带 Category: 前缀也行)
    // zim_id:        ZIM 文件标识, 如 "wikipedia/en"
    // max_depth:     递归深度上限 (默认 2, 防止爆炸)
    CategoryGraph categoryGraph(const std::string& root_category,
                                const std::string& zim_id,
                                int max_depth = 2);

    // —— 工具 2: 内部链接网络 + 核心度 ——
    // 从一个中心页面 BFS 展开内部 wiki 链接, 构建有向图
    // depth: 1 = 只取中心页直接链接, 2 = 链接的链接
    LinkGraph linkGraph(const std::string& canonical_uri,
                        int depth = 1,
                        int max_nodes = 100);

    // —— 工具 3: 重定向 + 消歧义检测 ——
    // 拉取原始 HTML, 检测 #REDIRECT / mw-redirect / disambiguation 模板
    RedirectInfo redirectInfo(const std::string& canonical_uri);

    // —— JSON 便捷输出 ——
    json toJson(const CategoryGraph& g) const;
    json toJson(const LinkGraph& g) const;
    json toJson(const RedirectInfo& r) const;

private:
    DataSourceRegistry& registry_;

    // —— 核心辅助 ——
    std::optional<std::string> fetchHtml(const std::string& canonical_uri);
    std::string extractTitleFromHtml(const std::string& html);
    std::vector<std::pair<std::string, std::string>> extractWikiLinks(
        const std::string& html,
        const std::string& base_url,
        const std::string& content_prefix);

    // —— 分类树辅助 ——
    // 判断一个链接是否是子分类 (vs 普通条目)
    static bool isCategoryPath(const std::string& path);
    // 递归爬分类页
    void crawlCategory(const std::string& category_uri,
                       const std::string& zim_id,
                       int current_depth,
                       int max_depth,
                       CategoryGraph& out,
                       std::set<std::string>& visited);

    // —— 链接网络辅助 ——
    void computeCoreScores(LinkGraph& graph);  // in-degree + PageRank-lite

    // —— 常量 ——
    static constexpr const char* CATEGORY_PREFIX_EN = "Category:";
    static constexpr const char* CATEGORY_PREFIX_ZH = "分类:";
    static constexpr const char* REDIRECT_MARKER = "#REDIRECT";
    static constexpr const char* MW_REDIRECT_CLASS = "mw-redirect";
    static constexpr const char* DISAMBIG_TEMPLATE = "disambiguation";
};

} // namespace github_research
