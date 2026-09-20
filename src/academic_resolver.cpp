#include "github_research/academic_resolver.hpp"
#include "github_research/semanticscholar_tools.hpp"
#include "github_research/arxiv_tools.hpp"
#include "github_research/string_utils.hpp"
#include <iostream>
#include <algorithm>
#include <set>

namespace github_research {

namespace {

// ─── 工具: 从 McpResult wrapper 里剥出 inner json ───
json extractInner(const json& mcp_result) {
    if (mcp_result.contains("content") && mcp_result["content"].is_array() && !mcp_result["content"].empty()) {
        auto& c = mcp_result["content"][0];
        if (c.contains("text")) {
            try { return json::parse(c["text"].get<std::string>()); } catch (...) {}
        }
    }
    return {};
}

bool isMcpError(const json& inner) {
    return inner.is_object() && inner.value("success", false) == false;
}

bool innerHasTitle(const json& inner) {
    return inner.is_object() && inner.contains("title") &&
           inner["title"].is_string() && !inner["title"].get<std::string>().empty();
}

// ─── S2 search 返回的 results 数组里, 挑 title 最匹配的 ───
int pickBestS2Match(const json& results, const std::string& target_title) {
    if (!results.is_array() || results.empty()) return -1;
    std::string tgt_lower = to_lower(target_title);
    int best_idx = -1;
    int best_score = -1;
    for (size_t i = 0; i < results.size(); ++i) {
        std::string cand = to_lower(results[i].value("title", ""));
        if (cand.empty()) continue;
        // exact → score=100, prefix → score=50, contains → score=20
        int score = 0;
        if (cand == tgt_lower) score = 100;
        else if (cand.find(tgt_lower) == 0 || tgt_lower.find(cand) == 0) score = 50;
        else if (cand.find(tgt_lower) != std::string::npos || tgt_lower.find(cand) != std::string::npos) score = 20;
        if (score > best_score) { best_score = score; best_idx = (int)i; }
    }
    // 要求至少 prefix/contains 匹配, 否则不选
    return best_score >= 20 ? best_idx : -1;
}

// ─── arXiv search 返回的 results 数组(结构化的), 挑最佳匹配 ───
int pickBestArxivMatch(const json& results, const std::string& target_title) {
    if (!results.is_array() || results.empty()) return -1;
    std::string tgt_lower = to_lower(target_title);
    int best_idx = -1;
    int best_score = -1;
    for (size_t i = 0; i < results.size(); ++i) {
        std::string cand = to_lower(results[i].value("title", ""));
        if (cand.empty()) continue;
        int score = 0;
        if (cand == tgt_lower) score = 100;
        else if (cand.find(tgt_lower) == 0 || tgt_lower.find(cand) == 0) score = 50;
        else if (cand.find(tgt_lower) != std::string::npos || tgt_lower.find(cand) != std::string::npos) score = 20;
        if (score > best_score) { best_score = score; best_idx = (int)i; }
    }
    return best_score >= 20 ? best_idx : -1;
}

// ─── 从 title 提取 3~5 个核心术语 (简单分词, 去停用词) ───
std::vector<std::string> extractCoreTerms(const std::string& title) {
    static const std::set<std::string> kStop = {
        "a","an","the","of","and","or","in","on","for","to","with","from","by","is","are","was","were",
        "as","at","be","been","being","but","if","then","so","such","that","this","these","those",
        "its","it","he","she","they","we","you","i","not","no","nor","do","does","did","can",
        "may","might","will","would","could","should","shall","must","have","has","had","having",
        "into","onto","upon","about","over","under","between","through","during","before","after",
        "new","novel","proposed","propose","using","use","used","based","approach","method","methods",
        "model","models","system","systems","framework","frameworks","analysis","analyzing","study",
        "studies","towards","toward","survey","review","case","example","examples"
    };
    std::vector<std::string> terms;
    std::string lower = to_lower(title);
    // 简单按非字母数字切分
    std::string cur;
    for (char c : lower) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
        else if (!cur.empty()) {
            if (cur.size() >= 3 && !kStop.count(cur)) terms.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty() && cur.size() >= 3 && !kStop.count(cur)) terms.push_back(cur);
    // 去重
    std::set<std::string> seen(terms.begin(), terms.end());
    terms.assign(seen.begin(), seen.end());
    // 取前 5 个
    if (terms.size() > 5) terms.resize(5);
    return terms;
}

std::string joinTerms(const std::vector<std::string>& terms) {
    std::string out;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i) out += " ";
        out += terms[i];
    }
    return out;
}

// ─── 从 S2 inner payload 构造 PaperResult ───
PaperResult buildFromS2(const json& inner) {
    PaperResult r;
    r.title = inner.value("title", "");
    r.abstract = inner.value("abstract", "");
    r.year = inner.value("year", 0);
    r.venue = inner.value("venue", "");
    r.external_id = inner.value("s2_paper_id", inner.value("paper_id", ""));
    r.url = inner.value("url", "");
    r.citation_count = inner.value("citation_count", 0);
    r.resolved_source = AcademicSource::S2;
    if (inner.contains("fields_of_study") && inner["fields_of_study"].is_array()) {
        for (auto& f : inner["fields_of_study"]) if (f.is_string()) r.fields_of_study.push_back(f.get<std::string>());
    }
    if (inner.contains("authors") && inner["authors"].is_array()) {
        for (auto& a : inner["authors"]) {
            if (a.is_object() && a.contains("name")) r.authors.push_back(a.value("name", ""));
            else if (a.is_string()) r.authors.push_back(a.get<std::string>());
        }
    }
    return r;
}

// ─── 从 arXiv inner payload 构造 PaperResult ───
PaperResult buildFromArxiv(const json& inner) {
    PaperResult r;
    r.title = inner.value("title", "");
    r.abstract = inner.value("abstract_full", inner.value("abstract", ""));
    // arXiv 没有 year 字段, 从 submitted_date ("YYYY-MM-DD") 截取
    std::string sd = inner.value("submitted_date", "");
    if (sd.size() >= 4) {
        try { r.year = std::stoi(sd.substr(0, 4)); } catch (...) { r.year = 0; }
    }
    r.venue = inner.value("primary_category", "");
    r.external_id = inner.value("arxiv_id", "");
    r.url = inner.value("url", "");
    r.citation_count = 0;  // arXiv 免费 API 不给 citationCount
    r.resolved_source = AcademicSource::ARXIV;
    if (inner.contains("authors") && inner["authors"].is_array()) {
        for (auto& a : inner["authors"]) {
            if (a.is_object() && a.contains("name")) r.authors.push_back(a.value("name", ""));
            else if (a.is_string()) r.authors.push_back(a.get<std::string>());
        }
    }
    return r;
}

// ─── 执行 L1: 原 ID 调用 ───
PaperResult tryL1OriginalId(AcademicSource source, const json& args) {
    PaperResult r;
    r.level = DegradationLevel::L1_ORIGINAL_ID;
    if (source == AcademicSource::S2) {
        std::string pid = args.value("paper_id", args.value("s2_paper_id", ""));
        if (pid.empty()) return r;
        json call_args = {{"paper_id", pid}};
        json resp = ToolS2GetPaperDetail(call_args);
        json inner = extractInner(resp);
        if (!isMcpError(inner) && innerHasTitle(inner)) {
            r = buildFromS2(inner);
            r.found = true;
            r.confidence = 1.0;
            r.raw_source_payload = inner;
        }
    } else {
        std::string aid = args.value("arxiv_id", "");
        if (aid.empty()) return r;
        json call_args = {{"arxiv_id", aid}};
        json resp = ToolArxivGetPaperDetail(call_args);
        json inner = extractInner(resp);
        if (!isMcpError(inner) && innerHasTitle(inner)) {
            r = buildFromArxiv(inner);
            r.found = true;
            r.confidence = 1.0;
            r.raw_source_payload = inner;
        }
    }
    return r;
}

// ─── 执行 L2: 精确标题检索 (同源) ───
PaperResult tryL2ExactTitle(AcademicSource source, const std::string& title) {
    PaperResult r;
    r.level = DegradationLevel::L2_EXACT_TITLE;
    if (title.empty()) return r;

    if (source == AcademicSource::S2) {
        json call_args = {{"query", title}, {"count", 5}};
        json resp = ToolS2SearchPapers(call_args);
        json inner = extractInner(resp);
        if (!inner.is_object()) return r;
        auto results = inner.value("results", json::array());
        int idx = pickBestS2Match(results, title);
        if (idx >= 0) {
            r = buildFromS2(results[idx]);
            r.found = true;
            r.confidence = 0.9;
            r.raw_source_payload = results[idx];
        }
    } else {
        // arXiv ToolArxivSearchPapers 是 Atom XML, 用 ToolArxivSearchIndex 更结构化
        json call_args = {{"query", title}, {"max_results", 5}};
        json resp = ToolArxivSearchIndex(call_args);
        json inner = extractInner(resp);
        if (!inner.is_object()) return r;
        auto results = inner.value("results", json::array());
        int idx = pickBestArxivMatch(results, title);
        if (idx >= 0) {
            r = buildFromArxiv(results[idx]);
            r.found = true;
            r.confidence = 0.9;
            r.raw_source_payload = results[idx];
        }
    }
    return r;
}

// ─── 执行 L3: 核心术语窄搜 (同源 + 可选领域限定) ───
PaperResult tryL3CoreTerms(AcademicSource source, const std::string& title,
                           const std::string& domain_hint) {
    PaperResult r;
    r.level = DegradationLevel::L3_CORE_TERMS;
    auto terms = extractCoreTerms(title);
    if (terms.empty()) return r;
    std::string query = joinTerms(terms);
    if (!domain_hint.empty()) query += " " + domain_hint;

    if (source == AcademicSource::S2) {
        json call_args = {{"query", query}, {"count", 5}};
        json resp = ToolS2SearchPapers(call_args);
        json inner = extractInner(resp);
        if (!inner.is_object()) return r;
        auto results = inner.value("results", json::array());
        int idx = pickBestS2Match(results, title);
        if (idx >= 0) {
            r = buildFromS2(results[idx]);
            r.found = true;
            r.confidence = 0.8;
            r.raw_source_payload = results[idx];
        }
    } else {
        json call_args = {{"query", query}, {"max_results", 5}};
        json resp = ToolArxivSearchIndex(call_args);
        json inner = extractInner(resp);
        if (!inner.is_object()) return r;
        auto results = inner.value("results", json::array());
        int idx = pickBestArxivMatch(results, title);
        if (idx >= 0) {
            r = buildFromArxiv(results[idx]);
            r.found = true;
            r.confidence = 0.8;
            r.raw_source_payload = results[idx];
        }
    }
    return r;
}

// ─── 执行 L4: 跨源交叉验证 ───
PaperResult tryL4CrossSource(AcademicSource original_source,
                              const std::string& title,
                              const std::string& domain_hint) {
    PaperResult r;
    r.level = DegradationLevel::L4_CROSS_SOURCE;
    AcademicSource other = (original_source == AcademicSource::S2)
                            ? AcademicSource::ARXIV : AcademicSource::S2;

    PaperResult found;
    if (other == AcademicSource::S2) {
        found = tryL3CoreTerms(AcademicSource::S2, title, domain_hint);
    } else {
        found = tryL3CoreTerms(AcademicSource::ARXIV, title, domain_hint);
    }
    if (found.found) {
        found.level = DegradationLevel::L4_CROSS_SOURCE;
        found.confidence = 0.7;  // 跨源降一级
        r = found;
    }
    return r;
}

} // anonymous namespace

// ============================================================
// 公共 API
// ============================================================
PaperResult resolvePaper(AcademicSource source, const json& args) {
    PaperResult best;
    std::string chain;
    std::string original_title;
    std::string domain_hint;

    // 先从 args 提取 title — 后续降级路径都需要
    if (args.contains("title") && args["title"].is_string())
        original_title = args["title"].get<std::string>();
    if (args.contains("domain") && args["domain"].is_string())
        domain_hint = args["domain"].get<std::string>();
    if (args.contains("primary_category") && args["primary_category"].is_string() && domain_hint.empty())
        domain_hint = args["primary_category"].get<std::string>();

    // ── L1: 原 ID ──
    best = tryL1OriginalId(source, args);
    chain += degradationLabel(best.level);
    if (best.found) {
        best.fallback_chain = chain;
        return best;
    }

    // L1 失败, 如果有 paper_id 还能从 external_ids 里补 title — 但此处 title 缺失就跳过 L2
    if (original_title.empty()) {
        // L1 没命中且没 title, 试试从 args 里找别的锚点
        if (args.contains("core_terms") && args["core_terms"].is_string()) {
            original_title = args["core_terms"].get<std::string>();
        } else {
            best.level = DegradationLevel::L5_CLUE_ONLY;
            best.is_clue_only = true;
            best.fallback_chain = chain + "→L5_CLUE_ONLY(no_anchor)";
            if (args.contains("paper_id")) best.external_id = args.value("paper_id", "");
            if (args.contains("arxiv_id"))  best.external_id = args.value("arxiv_id", "");
            return best;
        }
    }

    // ── L2: 精确标题 ──
    best = tryL2ExactTitle(source, original_title);
    chain += "→L2_EXACT_TITLE";
    if (best.found) {
        best.fallback_chain = chain;
        return best;
    }

    // ── L3: 核心术语 ──
    best = tryL3CoreTerms(source, original_title, domain_hint);
    chain += "→L3_CORE_TERMS";
    if (best.found) {
        best.fallback_chain = chain;
        return best;
    }

    // ── L4: 跨源 ──
    best = tryL4CrossSource(source, original_title, domain_hint);
    chain += "→L4_CROSS_SOURCE";
    if (best.found) {
        best.fallback_chain = chain;
        return best;
    }

    // ── L5: 线索留存 ──
    best = PaperResult{};
    best.level = DegradationLevel::L5_CLUE_ONLY;
    best.is_clue_only = true;
    best.fallback_chain = chain + "→L5_CLUE_ONLY";
    best.title = original_title;
    best.confidence = 0.0;
    if (args.contains("paper_id")) best.external_id = args.value("paper_id", "");
    if (args.contains("arxiv_id"))  best.external_id = args.value("arxiv_id", "");
    return best;
}

bool hasHardAnchors(const PaperResult& r) {
    return !r.title.empty() && !r.authors.empty() && r.year > 0;
}

std::string degradationLabel(DegradationLevel lvl) {
    switch (lvl) {
        case DegradationLevel::L1_ORIGINAL_ID: return "L1_ORIGINAL_ID";
        case DegradationLevel::L2_EXACT_TITLE: return "L2_EXACT_TITLE";
        case DegradationLevel::L3_CORE_TERMS:  return "L3_CORE_TERMS";
        case DegradationLevel::L4_CROSS_SOURCE: return "L4_CROSS_SOURCE";
        case DegradationLevel::L5_CLUE_ONLY:   return "L5_CLUE_ONLY";
    }
    return "UNKNOWN";
}

} // namespace github_research
