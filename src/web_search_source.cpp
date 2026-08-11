#include "github_research/web_search_source.hpp"
#include "github_research/webview_helpers.hpp"

namespace github_research {

// =============================================================
// WebSearchSource (priority 7, online)
//
// Generic search-engine fallback using WebView2.
// canonical_uri scheme: web://{url}  (results are fetched via
// the web crawler scheme so WebCrawlerSource can also fetch them)
// =============================================================

namespace {

constexpr const char* kLogPrefix = "[web_search]";

// JS: extract result links + titles from DuckDuckGo HTML endpoint.
// Returns a JSON array of {title, url}.
constexpr const char* kJsDdgExtract = R"(
(function(){
    var results = [];
    var links = document.querySelectorAll('a.result__a');
    var max = 30;
    for (var i = 0; i < links.length && i < max; i++) {
        var link = links[i];
        var title = (link.textContent || '').trim();
        var href = link.href || '';
        var actualUrl = href;
        try {
            var m = href.match(/[?&]uddg=([^&]+)/);
            if (m) { actualUrl = decodeURIComponent(m[1]); }
        } catch(e) {}
        if (title && actualUrl) {
            results.push({ title: title, url: actualUrl });
        }
    }
    return JSON.stringify(results);
})();
)";

} // namespace

WebSearchSource::WebSearchSource(WebViewSession* session)
    : session_(session) {}

bool WebSearchSource::healthCheck() {
    return session_ != nullptr;
}

std::vector<SearchResult> WebSearchSource::search(const SearchQuery& query) {
    std::vector<SearchResult> out;
    if (!session_ || query.query.empty()) {
        return out;
    }

    std::string url = "https://duckduckgo.com/html/?q=" + UrlEncodeComponent(query.query);

    json raw = NavigateAndExecuteRaw(
        *session_,
        std::wstring(url.begin(), url.end()),
        kJsDdgExtract,
        kLogPrefix);

    if (raw.is_null() || !raw.is_array()) {
        return out;
    }

    int max_results = query.max_results > 0 ? query.max_results : 20;
    int count = 0;
    for (const auto& item : raw) {
        if (count >= max_results) break;
        if (!item.is_object()) continue;

        std::string result_url;
        std::string result_title;
        if (item.contains("url") && item["url"].is_string()) {
            result_url = item["url"].get<std::string>();
        }
        if (item.contains("title") && item["title"].is_string()) {
            result_title = item["title"].get<std::string>();
        }
        if (result_url.empty() || result_title.empty()) continue;

        SearchResult sr;
        sr.canonical_uri = "web://" + result_url;
        sr.title = result_title;
        sr.resource_kind = ResourceKind::SEARCH_RESULT;
        sr.source_id = sourceId();
        out.push_back(std::move(sr));
        ++count;
    }

    return out;
}

std::optional<FetchResult> WebSearchSource::fetch(const std::string& canonical_uri) {
    if (!session_) {
        return std::nullopt;
    }

    // Strip "web://" scheme prefix to recover the actual URL.
    static const std::string kScheme = "web://";
    std::string url;
    if (canonical_uri.size() >= kScheme.size() &&
        canonical_uri.compare(0, kScheme.size(), kScheme) == 0) {
        url = canonical_uri.substr(kScheme.size());
    } else {
        url = canonical_uri;
    }

    if (url.empty()) {
        return std::nullopt;
    }

    json raw = NavigateAndExecuteRaw(
        *session_,
        std::wstring(url.begin(), url.end()),
        kJsExtractRawPage,
        kLogPrefix);

    if (raw.is_null() || !raw.is_object()) {
        return std::nullopt;
    }

    if (!raw.value("success", false)) {
        return std::nullopt;
    }

    FetchResult result;
    result.canonical_uri = canonical_uri;
    result.source_id = sourceId();
    result.resource_kind = ResourceKind::WEB_PAGE;

    if (raw.contains("title") && raw["title"].is_string()) {
        result.title = raw["title"].get<std::string>();
    }
    if (raw.contains("text") && raw["text"].is_string()) {
        result.content_markdown = raw["text"].get<std::string>();
    }

    return result;
}

} // namespace github_research
