#pragma once

#include "github_research/datasource.hpp"
#include "github_research/webview_session.hpp"

namespace github_research {

// =============================================================
// WebSearchSource (priority 7, online)
//
// Generic search engine fallback using WebView2.
// canonical_uri scheme: search://{query}
// Example: search://C++ async programming tutorial
// =============================================================
class WebSearchSource : public IDataSource {
public:
    explicit WebSearchSource(WebViewSession* session = nullptr);

    std::string sourceId() const override { return "web_search"; }
    int priority() const override { return 7; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;

private:
    WebViewSession* session_;
};

} // namespace github_research
