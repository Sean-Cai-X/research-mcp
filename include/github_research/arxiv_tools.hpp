#pragma once

// arXiv MCP 工具集(6 个工具: 4 原始 + 2 分层渐进挖掘)
// 架构:纯 HTTP REST(export.arxiv.org/api/query Atom XML + arxiv.org/html)
// 零浏览器依赖,启动即用,不需要 --arxiv-profile <DIR>
//
// 分层渐进挖掘(与 HN 对称):
//   - arxiv_search_index:      轻量索引,结构化返回搜索结果元数据
//   - arxiv_fetch_paper_detail: 单 ID 深挖,API 元数据 + HTML 全文

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

// 1. arxiv_search_papers - 论文检索(Atom XML API,结构化)
json ToolArxivSearchPapers(const json& args);

// 2. arxiv_get_paper_detail - 论文详情(API 元数据 + 摘要)
json ToolArxivGetPaperDetail(const json& args);

// 3. arxiv_get_pdf_link - 快速获取 PDF 直链(无需访问页面)
json ToolArxivGetPdfLink(const json& args);

// 4. arxiv_check_available - 检测 arXiv 网站连通性
json ToolArxivCheckAvailable(const json& args);

// 5. arxiv_search_index - 轻量索引(结构化,无 PDF 下载)
// args: query (string, required), max_results (int, default 20, max 50),
//       searchtype (string, default "all", 可选 all/title/abstract/author)
// 返回: [{arxiv_id, title, authors, primary_category, abstract_short, pdf_url, submitted_date}]
json ToolArxivSearchIndex(const json& args);

// 6. arxiv_fetch_paper_detail - 单 ID 深度抓取(API 元数据 + HTML 全文)
// args: arxiv_id (string, required),
//       fetch_full_text (bool, default true, 通过 arxiv.org/html 获取 HTML 全文),
//       fetch_references (bool, default true, 从全文末尾提取参考文献段落),
//       text_limit_chars (int, default 20000, 全文截断上限)
// 返回: {arxiv_id, title, authors, primary_category, abstract_full, submitted_date,
//        pdf_url, full_text, full_text_status, references}
json ToolArxivFetchPaperDetail(const json& args);

} // namespace github_research
