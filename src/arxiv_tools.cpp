#include "github_research/arxiv_tools.hpp"
#include "github_research/webview_helpers.hpp"  // NavigateAndExecute / McpError / McpSuccess
#include "github_research/string_utils.hpp"  // to_utf8 / to_wstring
#include "github_research/cache_manager.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <codecvt>
#include <locale>
#include <thread>
#include <chrono>

namespace github_research {

namespace {

// ============== 辅助:URL 编码 ==============
std::string UrlEncode(const std::string& str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << static_cast<char>(c);
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

// ============== 内置 JS 脚本 ==============
// 设计理念:工具只负责"取到页面内容",解析交给 AI
// 所有工具统一使用 webview_helpers.hpp 中的 kJsExtractRawPage

// 网站连通性检测 (https://arxiv.org 首页) - 仅此工具保留专用 JS
constexpr const char* kJsCheckAvailable = R"(
(function(){
    var hasHeader = !!document.querySelector("header#header, .header-banner, header[role=banner]");
    var hasSearch = !!document.querySelector('input[name="query"], form[action*="search"] input[type=text]');
    var hasLogo = !!document.querySelector('img[src*="arxiv"], svg, a#logo, .logo');
    var title = document.title || "";
    return JSON.stringify({
        success: true,
        available: (hasHeader || hasSearch || hasLogo || title.toLowerCase().indexOf("arxiv") !== -1),
        page_title: title
    });
})();
)";

// ============== 通用:导航 + 等待 + 执行JS ==============
// 已统一到 webview_helpers.hpp 的 NavigateAndExecute
// 所有 6 源工具共用同一调用模式(Navigate + ExecuteScript)

} // anonymous namespace

// ============================================================
// 1. arxiv_search_papers
// ============================================================
json ToolArxivSearchPapers(IBrowserSession& session, const json& args) {
    std::string query;
    int maxResults = 10;

    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        maxResults = args["max_results"].get<int>();

    if (query.empty()) {
        return {
            {"content", json::array({{{"type", "text"}, {"text", "ERROR: 'query' parameter is required"}}})},
            {"isError", true}
        };
    }
    if (maxResults < 1) maxResults = 1;
    if (maxResults > 50) maxResults = 50;

    // 构造搜索 URL,start=0,按相关度排序
    std::string encoded = UrlEncode(query);
    std::string url =
        "https://arxiv.org/search/?query=" + encoded +
        "&searchtype=all&start=0&order=-announced_date_first";

    // 统一返回原始页面文本,解析交给 AI
    (void)maxResults;
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[arxiv]", 2500, 30000);
}

// ============================================================
// 2. arxiv_get_paper_detail
// ============================================================
json ToolArxivGetPaperDetail(IBrowserSession& session, const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string())
        arxivId = args["arxiv_id"].get<std::string>();

    if (arxivId.empty()) {
        return {
            {"content", json::array({{{"type", "text"}, {"text", "ERROR: 'arxiv_id' parameter is required"}}})},
            {"isError", true}
        };
    }
    // 清洗:去掉可能的 .pdf 后缀
    if (arxivId.size() > 4 &&
        arxivId.compare(arxivId.size() - 4, 4, ".pdf") == 0)
        arxivId = arxivId.substr(0, arxivId.size() - 4);

    std::string url = "https://arxiv.org/abs/" + arxivId;
    // 统一返回原始页面文本,解析交给 AI
    return NavigateAndExecute(session, url, kJsExtractRawPage, "[arxiv]", 1500, 30000);
}

// ============================================================
// 3. arxiv_get_pdf_link (无需 WebView,零延迟)
// ============================================================
json ToolArxivGetPdfLink(const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string())
        arxivId = args["arxiv_id"].get<std::string>();

    if (arxivId.empty()) {
        return McpError("ERROR: 'arxiv_id' parameter is required");
    }
    if (arxivId.size() > 4 &&
        arxivId.compare(arxivId.size() - 4, 4, ".pdf") == 0)
        arxivId = arxivId.substr(0, arxivId.size() - 4);

    json payload = {
        {"success", true},
        {"arxiv_id", arxivId},
        {"pdf_url", "https://arxiv.org/pdf/" + arxivId},
        {"abs_url", "https://arxiv.org/abs/" + arxivId}
    };
    return McpSuccess(payload);
}

// ============================================================
// 4. arxiv_check_available
// ============================================================
json ToolArxivCheckAvailable(IBrowserSession& session, const json& /*args*/) {
    std::string url = "https://arxiv.org";
    return NavigateAndExecute(session, url, kJsCheckAvailable, "[arxiv]", 1500, 30000);
}

// ============================================================
// 分层渐进挖掘工具(新增,与 HN 对称)
// ============================================================

namespace {

constexpr const char* kArxivLogPrefix = "[arxiv]";

// arXiv 搜索结果页结构化提取 JS
// 解析 arxiv.org/search 结果列表中的 li.arxiv-result
// 字段: arxiv_id, title, authors, primary_category, abstract_short, pdf_url, submitted_date
constexpr const char* kJsArxivSearchIndex = R"(
(function(){
  var items = [];
  var results = document.querySelectorAll('li.arxiv-result');
  for (var i = 0; i < results.length; i++) {
    var r = results[i];
    var id = '';
    var idEl = r.querySelector('.arxiv-id');
    if (idEl) {
      id = idEl.textContent.trim();
    } else {
      var pdfLink = r.querySelector('a[href*="/pdf/"]');
      if (pdfLink) {
        var m = pdfLink.href.match(/\/pdf\/([0-9]{4}\.[0-9]+)/);
        if (m) id = m[1];
      }
    }
    var title = '';
    var titleEl = r.querySelector('.title');
    if (titleEl) title = titleEl.textContent.trim();

    var authors = '';
    var authorsEl = r.querySelector('.authors');
    if (authorsEl) authors = authorsEl.textContent.trim();

    var category = '';
    var tagEl = r.querySelector('.tag');
    if (tagEl) category = tagEl.textContent.trim();
    if (!category) {
      var primaryEl = r.querySelector('.primary-subject');
      if (primaryEl) category = primaryEl.textContent.trim();
    }

    var abstract = '';
    var absEl = r.querySelector('.abstract-short, .abstract');
    if (absEl) {
      abstract = absEl.textContent.trim();
      var more = absEl.querySelector('.abstract-short.has-abstract');
      if (more) abstract = abstract.replace('△ Less', '').trim();
    }
    if (abstract.length > 500) abstract = abstract.substring(0, 500) + '...';

    var pdfUrl = '';
    var pdfEl = r.querySelector('a[href*="/pdf/"]');
    if (pdfEl) pdfUrl = pdfEl.href;

    var submitted = '';
    var dateEl = r.querySelector('.is-date');
    if (dateEl) submitted = dateEl.textContent.trim();

    items.push({
      arxiv_id: id,
      title: title,
      authors: authors,
      primary_category: category,
      abstract_short: abstract,
      pdf_url: pdfUrl,
      submitted_date: submitted
    });
  }
  return JSON.stringify(items);
})();
)";

// arXiv abs 页结构化提取 JS
// 提取完整元数据 + 完整摘要
// 字段: {title, authors, primary_category, abstract_full, submitted_date, pdf_url}
constexpr const char* kJsArxivAbsDetail = R"(
(function(){
  var title = '';
  var titleEl = document.querySelector('h1.title');
  if (titleEl) title = titleEl.textContent.replace(/^\s*Title:\s*/, '').trim();

  var authors = '';
  var authorsEl = document.querySelector('.authors');
  if (authorsEl) {
    var names = [];
    authorsEl.querySelectorAll('a').forEach(function(a) { names.push(a.textContent.trim()); });
    authors = names.join(', ');
  }

  var category = '';
  var primaryEl = document.querySelector('.primary-subject');
  if (primaryEl) category = primaryEl.textContent.trim();

  var abstract = '';
  var absEl = document.querySelector('blockquote.abstract');
  if (absEl) {
    abstract = absEl.textContent.replace(/^\s*Abstract:\s*/, '').trim();
  }

  var submitted = '';
  var dateEl = document.querySelector('.dateline');
  if (dateEl) submitted = dateEl.textContent.trim();
  if (!submitted) {
    var histEl = document.querySelector('.submission-history');
    if (histEl) submitted = histEl.textContent.trim();
  }

  var pdfUrl = '';
  var pdfLink = document.querySelector('a[href*="/pdf/"]');
  if (pdfLink) pdfUrl = pdfLink.href;

  return JSON.stringify({
    title: title,
    authors: authors,
    primary_category: category,
    abstract_full: abstract,
    submitted_date: submitted,
    pdf_url: pdfUrl
  });
})();
)";

} // anonymous namespace

// ============================================================
// 5. arxiv_search_index - 轻量索引(结构化,无 PDF 下载)
// ============================================================
// 设计:只导航一次 arxiv 搜索页,解析 li.arxiv-result
// 返回结构化数组,不下载 PDF,网络请求最小化
json ToolArxivSearchIndex(IBrowserSession& session, const json& args) {
    std::string query;
    if (args.contains("query") && args["query"].is_string()) {
        query = args["query"].get<std::string>();
    }
    if (query.empty()) {
        return McpError("ERROR: [arxiv] 'query' parameter is required");
    }

    int maxResults = 20;
    if (args.contains("max_results") && args["max_results"].is_number_integer()) {
        maxResults = args["max_results"].get<int>();
    }
    if (maxResults < 1) maxResults = 1;
    if (maxResults > 50) maxResults = 50;

    std::string searchtype = "all";
    if (args.contains("searchtype") && args["searchtype"].is_string()) {
        searchtype = args["searchtype"].get<std::string>();
    }
    if (searchtype != "all" && searchtype != "title" &&
        searchtype != "abstract" && searchtype != "author") {
        searchtype = "all";
    }

    std::string encoded = UrlEncode(query);
    std::string urlStr = "https://arxiv.org/search/?query=" + encoded +
                         "&searchtype=" + searchtype +
                         "&start=0&order=-announced_date_first";
    std::string url = urlStr;

    json raw = NavigateAndExecuteRaw(session, url, kJsArxivSearchIndex, kArxivLogPrefix, 2500, 30000);
    if (raw.is_null()) {
        return McpError("ERROR: [arxiv] search index extraction failed");
    }

    json items = json::array();
    if (raw.is_array()) {
        int n = 0;
        for (auto& it : raw) {
            if (n >= maxResults) break;
            items.push_back(it);
            ++n;
        }
    }

    json payload = {
        {"success", true},
        {"query", query},
        {"searchtype", searchtype},
        {"total_returned", items.size()},
        {"items", items}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 6. arxiv_fetch_paper_detail - 单 ID 深度抓取
// ============================================================
// 流程:
//   1. 导航 arxiv.org/abs/{id},提取 title/authors/category/abstract_full/pdf_url
//   2. (可选)导航 ar5iv.org/abs/{id},用 kJsExtractRawPage 提取 HTML 全文
//      (ar5iv 是 arXiv 官方 HTML 渲染版,替代 PDF 解析,无需 PyMuPDF)
//   3. (可选)从全文末尾提取参考文献段落(简单启发式:References/REFERENCES 之后)
//   4. C++ 合并结构化结果,按 text_limit_chars 截断
json ToolArxivFetchPaperDetail(IBrowserSession& session, const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string()) {
        arxivId = args["arxiv_id"].get<std::string>();
    }
    if (arxivId.empty()) {
        return McpError("ERROR: [arxiv] 'arxiv_id' parameter is required");
    }
    // 清洗:去掉可能的 .pdf 后缀和版本号 v1/v2
    if (arxivId.size() > 4 &&
        arxivId.compare(arxivId.size() - 4, 4, ".pdf") == 0) {
        arxivId = arxivId.substr(0, arxivId.size() - 4);
    }
    // 去掉版本号 (如 2608.00757v2 -> 2608.00757)
    if (arxivId.size() > 2) {
        size_t vpos = arxivId.find_last_of('v');
        if (vpos != std::string::npos && vpos > 0) {
            bool allDigitAfter = true;
            for (size_t i = vpos + 1; i < arxivId.size(); ++i) {
                if (arxivId[i] < '0' || arxivId[i] > '9') {
                    allDigitAfter = false;
                    break;
                }
            }
            if (allDigitAfter && vpos >= 4) {
                // 确认 v 前面是数字(版本号模式)
                if (arxivId[vpos-1] >= '0' && arxivId[vpos-1] <= '9') {
                    arxivId = arxivId.substr(0, vpos);
                }
            }
        }
    }

    bool fetchFullText = true;
    if (args.contains("fetch_full_text") && args["fetch_full_text"].is_boolean()) {
        fetchFullText = args["fetch_full_text"].get<bool>();
    }
    bool fetchReferences = true;
    if (args.contains("fetch_references") && args["fetch_references"].is_boolean()) {
        fetchReferences = args["fetch_references"].get<bool>();
    }
    int textLimitChars = 20000;
    if (args.contains("text_limit_chars") && args["text_limit_chars"].is_number_integer()) {
        textLimitChars = args["text_limit_chars"].get<int>();
    }
    if (textLimitChars < 1000) textLimitChars = 1000;
    if (textLimitChars > 50000) textLimitChars = 50000;

    // ── 缓存查询:paper:{arxiv_id} (TTL=72h) ──
    // 命中且新鲜时直接返回,避免重复访问 arxiv.org
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "paper:" + arxivId;
    if (cm.is_ready()) {
        auto cached = cm.get("arxiv", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("arxiv", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    // 标记来自缓存
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) {
                cm.invalidate("arxiv", cache_key);
            }
        }
    }

    // --- 步骤1: 导航 abs 页,提取元数据 + 完整摘要 ---
    std::string absUrl = "https://arxiv.org/abs/" + arxivId;
    json metaRaw = NavigateAndExecuteRaw(session, absUrl, kJsArxivAbsDetail, kArxivLogPrefix, 2000, 30000);
    if (metaRaw.is_null()) {
        // abs 页拉取失败,写入短 TTL 失败缓存(避免短时间内重复回源)
        if (cm.is_ready()) {
            cm.put("arxiv", cache_key, "", "json", 1, "", "failed", "abs page fetch failed");
        }
        return McpError(std::string("ERROR: [arxiv] failed to fetch abs page for id=") + arxivId);
    }

    std::string title, authors, category, abstractFull, submittedDate, pdfUrl;
    if (metaRaw.is_object()) {
        if (metaRaw.contains("title") && metaRaw["title"].is_string())
            title = metaRaw["title"].get<std::string>();
        if (metaRaw.contains("authors") && metaRaw["authors"].is_string())
            authors = metaRaw["authors"].get<std::string>();
        if (metaRaw.contains("primary_category") && metaRaw["primary_category"].is_string())
            category = metaRaw["primary_category"].get<std::string>();
        if (metaRaw.contains("abstract_full") && metaRaw["abstract_full"].is_string())
            abstractFull = metaRaw["abstract_full"].get<std::string>();
        if (metaRaw.contains("submitted_date") && metaRaw["submitted_date"].is_string())
            submittedDate = metaRaw["submitted_date"].get<std::string>();
        if (metaRaw.contains("pdf_url") && metaRaw["pdf_url"].is_string())
            pdfUrl = metaRaw["pdf_url"].get<std::string>();
    }
    // 兜底:如果 abs 页没拿到 pdf_url,按规则拼接
    if (pdfUrl.empty()) {
        pdfUrl = "https://arxiv.org/pdf/" + arxivId;
    }

    // --- 步骤2: (可选)导航 ar5iv.org/abs/{id},用 kJsExtractRawPage 提取 HTML 全文 ---
    std::string fullText;
    std::string fullTextStatus = "skipped";
    if (fetchFullText) {
        std::string ar5ivUrl = "https://ar5iv.org/abs/" + arxivId;
        json fullRaw = NavigateAndExecuteRaw(session, ar5ivUrl, kJsExtractRawPage, kArxivLogPrefix, 3000, 30000);
        if (fullRaw.is_object()) {
            if (fullRaw.contains("text") && fullRaw["text"].is_string()) {
                fullText = fullRaw["text"].get<std::string>();
                if (fullText.empty()) {
                    // ar5iv 页面加载但内容为空(可能该论文未被 ar5iv 渲染)
                    fullTextStatus = "no_text";
                } else {
                    // 截断
                    if ((int)fullText.size() > textLimitChars) {
                        fullText = fullText.substr(0, textLimitChars);
                    }
                    fullTextStatus = "ok";
                }
            } else {
                fullTextStatus = "no_text";
            }
        } else {
            fullTextStatus = "fetch_failed";
        }
    }

    // --- 步骤3: (可选)从全文末尾提取参考文献段落 ---
    json references = json::array();
    std::string refStatus = "skipped";
    if (fetchReferences && !fullText.empty()) {
        // 简单启发式:查找 "References" 或 "REFERENCES" 标记
        std::string refSection;
        // 从后向前查找(参考文献通常在末尾)
        size_t refPos = fullText.rfind("References");
        if (refPos == std::string::npos) {
            refPos = fullText.rfind("REFERENCES");
        }
        if (refPos != std::string::npos) {
            refSection = fullText.substr(refPos);
            // 按行分割,提取形如 "[1] ..." 或 "1. ..." 的条目
            std::istringstream iss(refSection);
            std::string line;
            int refCount = 0;
            while (std::getline(iss, line)) {
                if (refCount >= 50) break;  // 最多 50 条
                // 去掉首尾空白
                size_t s = line.find_first_not_of(" \t");
                if (s == std::string::npos) continue;
                line = line.substr(s);
                // 跳过 "References" 标题行
                if (line == "References" || line == "REFERENCES") continue;
                // 跳过空行
                if (line.empty()) continue;
                // 必须以 [数字] 或 数字. 开头才认为是参考文献
                if (line.size() >= 3) {
                    if ((line[0] == '[' && line[1] >= '0' && line[1] <= '9') ||
                        (line[0] >= '0' && line[0] <= '9' &&
                         (line[1] == '.' || line[1] == ' '))) {
                        // 截断单条长度
                        if (line.size() > 300) line = line.substr(0, 300) + "...";
                        references.push_back(line);
                        ++refCount;
                    }
                }
            }
            refStatus = (refCount > 0) ? "ok" : "no_refs_found";
        } else {
            refStatus = "no_refs_section";
        }
    }

    // --- 步骤4: 合并结构化结果 ---
    json payload = {
        {"success", true},
        {"arxiv_id", arxivId},
        {"title", title},
        {"authors", authors},
        {"primary_category", category},
        {"abstract_full", abstractFull},
        {"submitted_date", submittedDate},
        {"pdf_url", pdfUrl},
        {"full_text", fullText},
        {"full_text_status", fullTextStatus},
        {"references", references},
        {"references_status", refStatus},
        {"reference_count", references.size()}
    };

    // ── 缓存写入:paper:{arxiv_id} (TTL=72h) ──
    // 同时写入 abs:{arxiv_id} 作为原始 abs 页快照(供后续对比)
    if (cm.is_ready()) {
        cm.put("arxiv", cache_key, payload.dump(), "json", 72, "", "ok", "");
        // abs:{id} 存储原始 metaRaw(元数据快照,用于版本对比)
        if (!metaRaw.is_null()) {
            cm.put("arxiv", "abs:" + arxivId, metaRaw.dump(), "json", 72, "", "ok", "");
        }
    }

    // ── entity_mapper: paper → paper 实体 + 关系 + 时间快照 ──
    // 跨源闭合:arXiv paper 注册为 paper 实体,建立 authored_by 关系
    if (cm.is_ready() && !title.empty()) {
        // 注册 paper 实体
        std::string paper_eid = cm.register_entity(
            "paper",
            arxivId,  // canonical_name = arxiv_id
            {title},   // aliases = title
            {category},  // tags = primary_category
            {{"primary_category", category},
             {"submitted_date", submittedDate},
             {"pdf_url", pdfUrl}},
            title  // description = title
        );

        // 关系: paper authored_by 作者
        // authors 是逗号分隔字符串,简化处理:为每个作者注册 person 实体
        if (!authors.empty()) {
            std::istringstream iss(authors);
            std::string author;
            int author_count = 0;
            while (std::getline(iss, author, ',') && author_count < 20) {
                // 去除首尾空白
                size_t s = author.find_first_not_of(" \t");
                size_t e = author.find_last_not_of(" \t");
                if (s == std::string::npos) continue;
                author = author.substr(s, e - s + 1);
                if (author.empty()) continue;
                ++author_count;
                std::string person_eid = cm.register_entity(
                    "person", author, {}, {}, json::object(), author
                );
                cm.add_relation(paper_eid, person_eid, "authored_by", 1.0, "arxiv", arxivId);
            }
        }

        // 关系: paper mentions 引用的参考文献(简化:记录引用计数)
        if (references.size() > 0) {
            cm.record_metric(paper_eid, "reference_count",
                              (double)references.size(), "arxiv");
        }

        // 关系: paper discussed_in HN(跨源闭合占位)
        // 实际 HN 讨论关系在 HN entity_mapper 中建立
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
