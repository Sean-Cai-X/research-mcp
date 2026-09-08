#pragma once

// arXiv MCP 工具集(6 个工具: 4 原始 + 2 分层渐进挖掘)
// 架构:通过独立 WebViewSession 访问 arxiv.org / ar5iv.org,JS 注入提取 DOM
// 不依赖登录态(arXiv 开放检索),保持 WebView2 统一架构一致性
//
// 分层渐进挖掘(新增,与 HN 对称):
//   - arxiv_search_index:      轻量索引,结构化返回搜索结果元数据
//   - arxiv_fetch_paper_detail: 单 ID 深挖,abs 页元数据 + ar5iv HTML 全文

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

// ============ 工具实现(由 MCP Server dispatch 调用) ============
// session:传入 arXiv 独立 WebViewSession 引用(已 Init 并就绪)

// 1. arxiv_search_papers - 论文检索(原始文本)
json ToolArxivSearchPapers(IBrowserSession& session, const json& args);

// 2. arxiv_get_paper_detail - 论文详情(完整摘要,原始文本)
json ToolArxivGetPaperDetail(IBrowserSession& session, const json& args);

// 3. arxiv_get_pdf_link - 快速获取 PDF 直链(无需访问页面)
json ToolArxivGetPdfLink(const json& args);

// 4. arxiv_check_available - 检测 arXiv 网站连通性
json ToolArxivCheckAvailable(IBrowserSession& session, const json& args);

// 5. arxiv_search_index - 轻量索引(结构化,无 PDF 下载)
// args: query (string, required), max_results (int, default 20, max 50),
//       searchtype (string, default "all", 可选 all/title/abstract/author)
// 返回: [{arxiv_id, title, authors, primary_category, abstract_short, pdf_url, submitted_date}]
json ToolArxivSearchIndex(IBrowserSession& session, const json& args);

// 6. arxiv_fetch_paper_detail - 单 ID 深度抓取(abs 元数据 + ar5iv 全文)
// args: arxiv_id (string, required),
//       fetch_full_text (bool, default true, 通过 ar5iv.org 获取 HTML 全文),
//       fetch_references (bool, default true, 从全文末尾提取参考文献段落),
//       text_limit_chars (int, default 20000, 全文截断上限)
// 返回: {arxiv_id, title, authors, primary_category, abstract_full, submitted_date,
//        pdf_url, full_text, full_text_status, references}
json ToolArxivFetchPaperDetail(IBrowserSession& session, const json& args);

} // namespace github_research
