#pragma once

// Semantic Scholar MCP 工具集(6 个工具)
// 架构:通过独立 WebViewSession 访问 semanticscholar.org,JS 注入提取 DOM
// 不依赖登录态(S2 开放检索),保持 WebView2 统一架构一致性
// paper_id 支持:DOI / corpus ID / arXiv ID(由 S2 URL 路由解析)

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

// ============ 工具实现(由 MCP Server dispatch 调用) ============
// session:传入 Semantic Scholar 独立 WebViewSession 引用(已 Init 并就绪)

// 1. s2_search_papers - 论文检索(支持年份过滤)
json ToolS2SearchPapers(IBrowserSession& session, const json& args);

// 2. s2_get_paper_detail - 论文详情(完整摘要、引用数、External IDs)
json ToolS2GetPaperDetail(IBrowserSession& session, const json& args);

// 3. s2_get_citations - 获取引用该论文的论文列表
json ToolS2GetCitations(IBrowserSession& session, const json& args);

// 4. s2_get_references - 获取该论文引用的参考文献列表
json ToolS2GetReferences(IBrowserSession& session, const json& args);

// 5. s2_get_author_papers - 获取作者论文列表及作者元信息
json ToolS2GetAuthorPapers(IBrowserSession& session, const json& args);

// 6. s2_search_author - 作者检索
json ToolS2SearchAuthor(IBrowserSession& session, const json& args);

// 7. s2_fetch_paper_detail - 分层工具: 缓存 + entity_mapper
//    cache_key=s2:{paper_id}, TTL=72h, entity=paper + cites/cited_by 关系
json ToolS2FetchPaperDetail(IBrowserSession& session, const json& args);

} // namespace github_research
