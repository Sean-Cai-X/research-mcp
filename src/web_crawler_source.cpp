#include "github_research/web_crawler_source.hpp"
#include "github_research/webview_helpers.hpp"

namespace github_research {

// =============================================================
// WebCrawlerSource (priority 4, online)
//
// Generic web page crawler using WebView2 (JS-rendered).
// canonical_uri scheme: web://{url}
// This is a fetch-only source; search() returns empty.
// =============================================================

WebCrawlerSource::WebCrawlerSource(WebViewSession* session)
    : session_(session) {}

bool WebCrawlerSource::healthCheck() {
    return session_ != nullptr;
}

std::vector<SearchResult> WebCrawlerSource::search(const SearchQuery& query) {
    // Web crawler is fetch-only; it does not perform search.
    (void)query;
    return {};
}

std::optional<FetchResult> WebCrawlerSource::fetch(const std::string& canonical_uri) {
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

    // Navigate + execute the unified raw-page extraction JS.
    json raw = NavigateAndExecuteRaw(
        *session_,
        std::wstring(url.begin(), url.end()),
        kJsExtractRawPage,
        "[web_crawler]");

    if (raw.is_null() || !raw.is_object()) {
        return std::nullopt;
    }

    // The JS returns {success, url, title, text, html}.
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
