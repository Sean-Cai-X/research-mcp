#include "github_research/arxiv_tools.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/cache_manager.hpp"
#include "github_research/curl_http_client.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[arxiv]";

// ─── Curl HTTP client singleton (per-source, like HN pattern) ───
std::mutex g_arxiv_curl_mutex;
std::unique_ptr<CurlHttpClient> g_arxiv_curl;

CurlHttpClient& get_arxiv_curl() {
    std::lock_guard<std::mutex> lk(g_arxiv_curl_mutex);
    if (!g_arxiv_curl) {
        g_arxiv_curl = std::make_unique<CurlHttpClient>("research-mcp-arxiv/1.0", 30);
        g_arxiv_curl->initialize();
    }
    return *g_arxiv_curl;
}

// ─── Simple Atom XML parser (arXiv export API response) ───
// Only extracts what we need: <entry> blocks with children
std::string xml_find_between(const std::string& src, const std::string& openTag, const std::string& closeTag, size_t start = 0) {
    size_t open = src.find(openTag, start);
    if (open == std::string::npos) return "";
    size_t afterOpen = open + openTag.size();
    size_t close = src.find(closeTag, afterOpen);
    if (close == std::string::npos) return "";
    // Skip whitespace between open tag and content
    size_t contentStart = src.find_first_not_of(" \t\n\r", afterOpen);
    if (contentStart == std::string::npos || contentStart >= close) return "";
    return src.substr(contentStart, close - contentStart);
}

std::string xml_find_between_no_ws(const std::string& src, const std::string& openTag, const std::string& closeTag, size_t start = 0) {
    size_t open = src.find(openTag, start);
    if (open == std::string::npos) return "";
    size_t afterOpen = open + openTag.size();
    size_t close = src.find(closeTag, afterOpen);
    if (close == std::string::npos) return "";
    return src.substr(afterOpen, close - afterOpen);
}

// Parse author blocks: each <author><name>...</name></author>
std::string parse_authors(const std::string& entry) {
    std::string authors;
    size_t pos = 0;
    bool first = true;
    while (true) {
        size_t auStart = entry.find("<author>", pos);
        if (auStart == std::string::npos) break;
        size_t auEnd = entry.find("</author>", auStart);
        if (auEnd == std::string::npos) break;
        std::string authorBlock = entry.substr(auStart, auEnd - auStart);
        std::string name = xml_find_between(authorBlock, "<name>", "</name>");
        // XML unescape
        if (!name.empty()) {
            if (name.find("&amp;") != std::string::npos) {
                size_t p = 0;
                while ((p = name.find("&amp;", p)) != std::string::npos) { name.replace(p, 5, "&"); p++; }
            }
            if (!first) authors += ", ";
            authors += name;
            first = false;
        }
        pos = auEnd + 9;
    }
    return authors;
}

// Parse <primary_category term="..."/> 
std::string parse_primary_category(const std::string& entry) {
    size_t pos = entry.find("<arxiv:primary_category");
    if (pos == std::string::npos) return "";
    size_t eq = entry.find("term=\"", pos);
    if (eq == std::string::npos) return "";
    size_t q1 = eq + 6;
    size_t q2 = entry.find("\"", q1);
    if (q2 == std::string::npos) return "";
    return entry.substr(q1, q2 - q1);
}

// Parse <id>http://arxiv.org/abs/2501.01234v1</id> → "2501.01234v1"
std::string parse_arxiv_id(const std::string& entry) {
    std::string id = xml_find_between(entry, "<id>", "</id>");
    size_t pos = id.find("abs/");
    if (pos == std::string::npos) {
        pos = id.find("/pdf/");
        if (pos == std::string::npos) return id;
    }
    return id.substr(pos + 4);
}

// Parse full Atom feed → json::array of entries
json parse_atom_feed(const std::string& xml) {
    json entries = json::array();
    size_t pos = 0;
    while (true) {
        size_t eStart = xml.find("<entry>", pos);
        if (eStart == std::string::npos) break;
        size_t eEnd = xml.find("</entry>", eStart);
        if (eEnd == std::string::npos) break;
        std::string entryBlock = xml.substr(eStart, eEnd - eStart);

        json item;
        item["arxiv_id"] = parse_arxiv_id(entryBlock);
        // clean arxiv_id: remove version suffix for canonical id
        std::string aid = item["arxiv_id"].get<std::string>();
        std::string canonicalId = aid;
        size_t vpos = canonicalId.find_last_of('v');
        if (vpos != std::string::npos && vpos > 0) {
            bool allDigitAfter = true;
            for (size_t i = vpos + 1; i < canonicalId.size(); ++i) {
                if (canonicalId[i] < '0' || canonicalId[i] > '9') { allDigitAfter = false; break; }
            }
            if (allDigitAfter) canonicalId = canonicalId.substr(0, vpos);
        }
        item["canonical_id"] = canonicalId;
        item["title"] = xml_find_between(entryBlock, "<title>", "</title>");
        // XML unescape title
        {
            std::string& t = item["title"].get_ref<std::string&>();
            size_t p = 0;
            while ((p = t.find("&#10;", p)) != std::string::npos) { t.replace(p, 5, " "); }
            p = 0;
            while ((p = t.find("&#xA;", p)) != std::string::npos) { t.replace(p, 5, " "); }
            p = 0;
            while ((p = t.find("&amp;", p)) != std::string::npos) { t.replace(p, 5, "&"); }
        }
        item["authors"] = parse_authors(entryBlock);
        item["primary_category"] = parse_primary_category(entryBlock);
        item["abstract"] = xml_find_between(entryBlock, "<summary>", "</summary>");
        item["published"] = xml_find_between(entryBlock, "<published>", "</published>");
        item["pdf_url"] = "https://arxiv.org/pdf/" + canonicalId;
        item["abs_url"] = "https://arxiv.org/abs/" + canonicalId;

        entries.push_back(std::move(item));
        pos = eEnd + 8;
    }
    return entries;
}

// HTTP helper: GET and parse Atom XML
json fetch_arxiv_api(const std::string& url) {
    CurlHttpClient& curl = get_arxiv_curl();
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "ResearchMCP/1.0";
    headers["Accept"] = "application/atom+xml";
    HttpResponse resp = curl.get(url, headers);
    if (resp.status_code != 200 || resp.body.empty()) {
        std::cerr << kLogPrefix << " API fetch failed: HTTP " << resp.status_code << std::endl;
        return json::array();
    }
    return parse_atom_feed(resp.body);
}

// HTML to plaintext (reuse pattern from HN tools)
std::string html_to_plaintext(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    bool prev_space = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; out += ' '; prev_space = true; continue; }
        if (in_tag) continue;
        if (c == '&') {
            if (html.compare(i, 4, "&amp;") == 0) { out += '&'; i += 3; continue; }
            if (html.compare(i, 4, "&lt;") == 0)  { out += '<'; i += 3; continue; }
            if (html.compare(i, 4, "&gt;") == 0)  { out += '>'; i += 3; continue; }
            if (html.compare(i, 6, "&nbsp;") == 0) { out += ' '; i += 5; continue; }
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!prev_space) { out += '\n'; prev_space = true; }
            continue;
        }
        if (c == ' ') {
            if (!prev_space) { out += ' '; prev_space = true; }
            continue;
        }
        out += c; prev_space = false;
    }
    // collapse blank lines
    std::string result;
    int blank = 0;
    for (char c : out) {
        if (c == '\n') {
            if (blank < 2) result += c;
            blank++;
        } else {
            result += c; blank = 0;
        }
    }
    return result;
}

// Strip .pdf and version suffix from arxiv_id
std::string clean_arxiv_id(const std::string& raw) {
    std::string id = raw;
    if (id.size() > 4 && id.compare(id.size() - 4, 4, ".pdf") == 0) {
        id = id.substr(0, id.size() - 4);
    }
    if (id.size() > 2) {
        size_t vpos = id.find_last_of('v');
        if (vpos != std::string::npos && vpos > 0) {
            bool allDigit = true;
            for (size_t i = vpos + 1; i < id.size(); ++i) {
                if (id[i] < '0' || id[i] > '9') { allDigit = false; break; }
            }
            if (allDigit && vpos >= 4 && id[vpos - 1] >= '0' && id[vpos - 1] <= '9') {
                id = id.substr(0, vpos);
            }
        }
    }
    return id;
}

} // anonymous namespace

// ============================================================
// 1. arxiv_search_papers - Atom XML API → structured results
// ============================================================
json ToolArxivSearchPapers(const json& args) {
    std::string query;
    int maxResults = 10;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        maxResults = args["max_results"].get<int>();
    if (query.empty()) {
        return McpError("ERROR: 'query' parameter is required");
    }
    if (maxResults < 1) maxResults = 1;
    if (maxResults > 50) maxResults = 50;

    std::string url = std::string("http://export.arxiv.org/api/query?search_query=all:")
        + UrlEncodeComponent(query)
        + "&start=0&max_results=" + std::to_string(maxResults)
        + "&sortBy=relevance&sortOrder=descending";

    json entries = fetch_arxiv_api(url);
    if (entries.empty()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " Atom API returned no results for query='" + query + "'");
    }

    // Truncate abstracts to 500 chars for search results
    json results = json::array();
    for (auto& e : entries) {
        json item = e;
        std::string abs = item.value("abstract", "");
        if (abs.size() > 500) abs = abs.substr(0, 500) + "...";
        item["abstract_short"] = abs;
        item.erase("abstract");
        results.push_back(item);
    }

    json payload = {
        {"success", true},
        {"source", "arxiv_atom_api"},
        {"query", query},
        {"total_returned", results.size()},
        {"items", results}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 2. arxiv_get_paper_detail - Atom XML API by ID
// ============================================================
json ToolArxivGetPaperDetail(const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string())
        arxivId = args["arxiv_id"].get<std::string>();
    if (arxivId.empty()) {
        return McpError("ERROR: 'arxiv_id' parameter is required");
    }
    arxivId = clean_arxiv_id(arxivId);

    std::string url = "http://export.arxiv.org/api/query?id_list=" + arxivId;
    json entries = fetch_arxiv_api(url);
    if (entries.empty()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " paper " + arxivId + " not found via Atom API");
    }

    json item = entries[0];
    json payload = {
        {"success", true},
        {"arxiv_id", arxivId},
        {"title", item.value("title", "")},
        {"authors", item.value("authors", "")},
        {"primary_category", item.value("primary_category", "")},
        {"abstract_full", item.value("abstract", "")},
        {"submitted_date", item.value("published", "")},
        {"pdf_url", item.value("pdf_url", "")},
        {"abs_url", item.value("abs_url", "")}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 3. arxiv_get_pdf_link (zero network, ID-rule based)
// ============================================================
json ToolArxivGetPdfLink(const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string())
        arxivId = args["arxiv_id"].get<std::string>();
    if (arxivId.empty()) {
        return McpError("ERROR: 'arxiv_id' parameter is required");
    }
    arxivId = clean_arxiv_id(arxivId);

    json payload = {
        {"success", true},
        {"arxiv_id", arxivId},
        {"pdf_url", "https://arxiv.org/pdf/" + arxivId},
        {"abs_url", "https://arxiv.org/abs/" + arxivId}
    };
    return McpSuccess(payload);
}

// ============================================================
// 4. arxiv_check_available - simple HTTP HEAD to export.arxiv.org
// ============================================================
json ToolArxivCheckAvailable(const json& args) {
    (void)args;
    CurlHttpClient& curl = get_arxiv_curl();
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "ResearchMCP/1.0";
    HttpResponse resp = curl.get("http://export.arxiv.org/api/query?search_query=all:test&max_results=1", headers);
    
    bool available = (resp.status_code >= 200 && resp.status_code < 500);
    json payload = {
        {"success", true},
        {"available", available},
        {"http_status", resp.status_code},
        {"api_endpoint", "export.arxiv.org/api/query"}
    };
    if (!available) {
        payload["error_detail"] = "export.arxiv.org returned HTTP " + std::to_string(resp.status_code);
    }
    return WrapMcpResult(payload);
}

// ============================================================
// 5. arxiv_search_index - light structured index (uses Atom API)
// ============================================================
json ToolArxivSearchIndex(const json& args) {
    std::string query;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    if (query.empty()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " 'query' parameter is required");
    }
    int maxResults = 20;
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        maxResults = args["max_results"].get<int>();
    if (maxResults < 1) maxResults = 1;
    if (maxResults > 50) maxResults = 50;

    std::string searchtype = "all";
    if (args.contains("searchtype") && args["searchtype"].is_string())
        searchtype = args["searchtype"].get<std::string>();
    if (searchtype != "all" && searchtype != "title" &&
        searchtype != "abstract" && searchtype != "author") {
        searchtype = "all";
    }

    // search_query prefix: all: or ti: or abs: or au:
    std::string prefix = (searchtype == "all") ? "all:" : (searchtype + ":");
    std::string url = std::string("http://export.arxiv.org/api/query?search_query=")
        + prefix + UrlEncodeComponent(query)
        + "&start=0&max_results=" + std::to_string(maxResults)
        + "&sortBy=relevance&sortOrder=descending";

    json entries = fetch_arxiv_api(url);
    json items = json::array();
    int n = 0;
    for (auto& e : entries) {
        if (n >= maxResults) break;
        json item;
        item["arxiv_id"] = e.value("canonical_id", e.value("arxiv_id", ""));
        item["title"] = e.value("title", "");
        item["authors"] = e.value("authors", "");
        item["primary_category"] = e.value("primary_category", "");
        std::string abs = e.value("abstract", "");
        if (abs.size() > 500) abs = abs.substr(0, 500) + "...";
        item["abstract_short"] = abs;
        item["pdf_url"] = e.value("pdf_url", "");
        item["submitted_date"] = e.value("published", "");
        items.push_back(std::move(item));
        ++n;
    }

    json payload = {
        {"success", true},
        {"source", "arxiv_atom_api"},
        {"query", query},
        {"searchtype", searchtype},
        {"total_returned", items.size()},
        {"items", items}
    };
    return WrapMcpResult(payload);
}

// ============================================================
// 6. arxiv_fetch_paper_detail - deep fetch (API meta + HTML full text)
// ============================================================
json ToolArxivFetchPaperDetail(const json& args) {
    std::string arxivId;
    if (args.contains("arxiv_id") && args["arxiv_id"].is_string())
        arxivId = args["arxiv_id"].get<std::string>();
    if (arxivId.empty()) {
        return McpError(std::string("ERROR: ") + kLogPrefix + " 'arxiv_id' parameter is required");
    }
    arxivId = clean_arxiv_id(arxivId);

    bool fetchFullText = true;
    if (args.contains("fetch_full_text") && args["fetch_full_text"].is_boolean())
        fetchFullText = args["fetch_full_text"].get<bool>();
    bool fetchReferences = true;
    if (args.contains("fetch_references") && args["fetch_references"].is_boolean())
        fetchReferences = args["fetch_references"].get<bool>();
    int textLimitChars = 20000;
    if (args.contains("text_limit_chars") && args["text_limit_chars"].is_number_integer())
        textLimitChars = args["text_limit_chars"].get<int>();
    if (textLimitChars < 1000) textLimitChars = 1000;
    if (textLimitChars > 50000) textLimitChars = 50000;

    // ── Cache lookup ──
    CacheManager& cm = CacheManager::instance();
    std::string cache_key = "paper:" + arxivId;
    if (cm.is_ready()) {
        auto cached = cm.get("arxiv", cache_key);
        if (cached && cached->fetch_status == "ok" && cm.is_fresh("arxiv", cache_key)) {
            try {
                json cached_payload = json::parse(cached->payload);
                if (cached_payload.is_object()) {
                    cached_payload["cache_hit"] = true;
                    cached_payload["cache_expires_at"] = cached->expires_at;
                    return WrapMcpResult(cached_payload);
                }
            } catch (...) { cm.invalidate("arxiv", cache_key); }
        }
    }

    // ── Step 1: Atom API for metadata ──
    std::string apiUrl = "http://export.arxiv.org/api/query?id_list=" + arxivId;
    json entries = fetch_arxiv_api(apiUrl);
    if (entries.empty()) {
        if (cm.is_ready()) cm.put("arxiv", cache_key, "", "json", 1, "", "failed", "paper not found");
        return McpError(std::string("ERROR: ") + kLogPrefix + " paper " + arxivId + " not found");
    }
    json meta = entries[0];

    std::string title = meta.value("title", "");
    std::string authors = meta.value("authors", "");
    std::string category = meta.value("primary_category", "");
    std::string abstractFull = meta.value("abstract", "");
    std::string submittedDate = meta.value("published", "");
    std::string pdfUrl = meta.value("pdf_url", "");

    // ── Step 2 (optional): fetch HTML full text from arxiv.org/html/{id}v1 ──
    std::string fullText;
    std::string fullTextStatus = "skipped";
    if (fetchFullText) {
        std::string htmlUrl = "https://arxiv.org/html/" + arxivId + "v1";
        CurlHttpClient& curl = get_arxiv_curl();
        std::map<std::string, std::string> hdrs;
        hdrs["User-Agent"] = "ResearchMCP/1.0";
        HttpResponse resp = curl.get(htmlUrl, hdrs);
        if (resp.status_code == 200 && !resp.body.empty()) {
            fullText = html_to_plaintext(resp.body);
            if (fullText.empty()) {
                fullTextStatus = "no_text";
            } else {
                if ((int)fullText.size() > textLimitChars)
                    fullText = fullText.substr(0, textLimitChars);
                fullTextStatus = "ok";
            }
        } else {
            fullTextStatus = "fetch_failed";
            std::cerr << kLogPrefix << " HTML fetch failed: HTTP " << resp.status_code << std::endl;
        }
    }

    // ── Step 3 (optional): extract references from full text ──
    json references = json::array();
    std::string refStatus = "skipped";
    if (fetchReferences && !fullText.empty()) {
        size_t refPos = fullText.rfind("References");
        if (refPos == std::string::npos) refPos = fullText.rfind("REFERENCES");
        if (refPos != std::string::npos) {
            std::string refSection = fullText.substr(refPos);
            std::istringstream iss(refSection);
            std::string line;
            int refCount = 0;
            while (std::getline(iss, line)) {
                if (refCount >= 50) break;
                size_t s = line.find_first_not_of(" \t");
                if (s == std::string::npos) continue;
                line = line.substr(s);
                if (line == "References" || line == "REFERENCES") continue;
                if (line.empty()) continue;
                if (line.size() >= 3) {
                    bool looksLikeRef =
                        (line[0] == '[' && line[1] >= '0' && line[1] <= '9') ||
                        (line[0] >= '0' && line[0] <= '9' && (line[1] == '.' || line[1] == ' '));
                    if (looksLikeRef) {
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

    // ── Cache write ──
    if (cm.is_ready()) {
        cm.put("arxiv", cache_key, payload.dump(), "json", 72, "", "ok", "");
    }

    // ── entity_mapper ──
    if (cm.is_ready() && !title.empty()) {
        std::string paper_eid = cm.register_entity(
            "paper", arxivId, {title}, {category},
            {{"primary_category", category}, {"submitted_date", submittedDate}, {"pdf_url", pdfUrl}},
            title);
        if (!authors.empty()) {
            std::istringstream iss(authors);
            std::string author;
            int ac = 0;
            while (std::getline(iss, author, ',') && ac < 20) {
                size_t s = author.find_first_not_of(" \t");
                size_t e = author.find_last_not_of(" \t");
                if (s == std::string::npos) continue;
                author = author.substr(s, e - s + 1);
                if (author.empty()) continue;
                ++ac;
                std::string person_eid = cm.register_entity("person", author, {}, {}, json::object(), author);
                cm.add_relation(paper_eid, person_eid, "authored_by", 1.0, "arxiv", arxivId);
            }
        }
        if (references.size() > 0) {
            cm.record_metric(paper_eid, "reference_count", (double)references.size(), "arxiv");
        }
    }

    return WrapMcpResult(payload);
}

} // namespace github_research
