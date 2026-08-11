#include "github_research/arxiv_source.hpp"
#include "github_research/webview_helpers.hpp"
#include "github_research/string_utils.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace github_research {

namespace {

constexpr const char* kLogPrefix = "[arxiv]";

// arXiv search results page structured extraction JS.
// Parses li.arxiv-result rows from arxiv.org/search and returns a JSON array
// of {arxiv_id, title, authors, abstract_short, pdf_url}.
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
    if (!id) {
      var absLink = r.querySelector('a[href*="/abs/"]');
      if (absLink) {
        var m2 = absLink.href.match(/\/abs\/([0-9]{4}\.[0-9]+)/);
        if (m2) id = m2[1];
      }
    }
    if (!id) continue;

    var title = '';
    var titleEl = r.querySelector('.title');
    if (titleEl) title = titleEl.textContent.trim();

    var authors = '';
    var authorsEl = r.querySelector('.authors');
    if (authorsEl) {
      var names = [];
      authorsEl.querySelectorAll('a').forEach(function(a) {
        names.push(a.textContent.trim());
      });
      authors = names.join(', ');
    }

    var abstract = '';
    var absEl = r.querySelector('.abstract-short, .abstract');
    if (absEl) {
      abstract = absEl.textContent.trim();
      abstract = abstract.replace(/\s*△ Less.*$/, '').trim();
    }
    if (abstract.length > 500) abstract = abstract.substring(0, 500) + '...';

    var pdfUrl = '';
    var pdfEl = r.querySelector('a[href*="/pdf/"]');
    if (pdfEl) pdfUrl = pdfEl.href;

    items.push({
      arxiv_id: id,
      title: title,
      authors: authors,
      abstract_short: abstract,
      pdf_url: pdfUrl
    });
  }
  return JSON.stringify(items);
})();
)";

// Strip a trailing .pdf suffix from an arxiv id
std::string cleanArxivId(const std::string& raw) {
    std::string id = raw;
    if (id.size() > 4 &&
        id.compare(id.size() - 4, 4, ".pdf") == 0) {
        id = id.substr(0, id.size() - 4);
    }
    return id;
}

} // anonymous namespace

// =============================================================
// ArxivSource (priority 6, online)
//
// arXiv paper search and full text retrieval.
// canonical_uri scheme: arxiv://{arxiv_id}
// Example: arxiv://2401.12345
// =============================================================

ArxivSource::ArxivSource(WebViewSession* session)
    : session_(session) {}

bool ArxivSource::healthCheck() {
    return session_ != nullptr;
}

std::vector<SearchResult> ArxivSource::search(const SearchQuery& query) {
    std::vector<SearchResult> results;
    if (!session_) return results;
    if (query.query.empty()) return results;

    std::string encoded = UrlEncodeComponent(query.query);
    std::string url_str = "https://arxiv.org/search/?query=" + encoded +
                          "&searchtype=all&start=0";
    std::wstring url = to_wstring(url_str);

    json raw = NavigateAndExecuteRaw(*session_, url, kJsArxivSearchIndex,
                                     kLogPrefix, 2500, 30000);
    if (raw.is_null() || !raw.is_array()) {
        return results;
    }

    int count = 0;
    for (const auto& item : raw) {
        if (count >= query.max_results) break;
        if (!item.is_object()) continue;
        std::string arxiv_id = item.value("arxiv_id", "");
        if (arxiv_id.empty()) continue;
        arxiv_id = cleanArxivId(arxiv_id);

        SearchResult r;
        r.canonical_uri = "arxiv://" + arxiv_id;
        r.title = item.value("title", arxiv_id);
        r.resource_kind = ResourceKind::ARXIV_PAPER;
        r.snippet = item.value("abstract_short", "");
        if (r.snippet.empty()) {
            r.snippet = item.value("authors", "");
        }
        r.source_id = sourceId();
        results.push_back(std::move(r));
        ++count;
    }
    return results;
}

std::optional<FetchResult> ArxivSource::fetch(const std::string& canonical_uri) {
    if (!session_) return std::nullopt;

    static const std::string scheme = "arxiv://";
    if (canonical_uri.compare(0, scheme.size(), scheme) != 0) {
        return std::nullopt;
    }
    std::string arxiv_id = cleanArxivId(canonical_uri.substr(scheme.size()));
    if (arxiv_id.empty()) return std::nullopt;

    std::string url_str = "https://arxiv.org/abs/" + arxiv_id;
    std::wstring url = to_wstring(url_str);

    json raw = NavigateAndExecuteRaw(*session_, url, kJsExtractRawPage,
                                     kLogPrefix, 2000, 30000);
    if (raw.is_null() || !raw.is_object()) {
        return std::nullopt;
    }

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    fr.source_id = sourceId();
    fr.resource_kind = ResourceKind::ARXIV_PAPER;
    fr.title = raw.value("title", arxiv_id);
    if (fr.title.empty()) fr.title = arxiv_id;
    fr.content_markdown = raw.value("text", "");
    if (fr.content_markdown.empty()) {
        // Fall back to html if text extraction returned nothing
        fr.content_markdown = raw.value("html", "");
    }
    return fr;
}

} // namespace github_research
