#pragma once

// Semantic Scholar MCP 工具集(7 个 s2_* 工具)
// 架构:纯 HTTP REST (api.semanticscholar.org/graph/v1/)
// 零浏览器依赖,启动即用
// paper_id 支持:DOI / corpus ID / arXiv ID(由 S2 API 自动路由)

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// 1. s2_search_papers - 论文检索(支持年份过滤)
json ToolS2SearchPapers(const json& args);

// 2. s2_get_paper_detail - 论文详情(完整摘要、引用数、External IDs)
json ToolS2GetPaperDetail(const json& args);

// 3. s2_get_citations - 获取引用该论文的论文列表
json ToolS2GetCitations(const json& args);

// 4. s2_get_references - 获取该论文引用的参考文献列表
json ToolS2GetReferences(const json& args);

// 5. s2_get_author_papers - 获取作者论文列表及作者元信息
json ToolS2GetAuthorPapers(const json& args);

// 6. s2_search_author - 作者检索
json ToolS2SearchAuthor(const json& args);

// 7. s2_fetch_paper_detail - 分层工具: 缓存 + entity_mapper
//    cache_key=s2:{paper_id}, TTL=72h, entity=paper + cites/cited_by 关系
json ToolS2FetchPaperDetail(const json& args);

} // namespace github_research
