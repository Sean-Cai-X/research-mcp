#pragma once

#include "github_research/datasource.hpp"
#include "github_research/webview_session.hpp"

namespace github_research {

// =============================================================
// WebCrawlerSource (priority 4, online)
//
// Generic web page crawler using WebView2 (JS-rendered).
// canonical_uri scheme: web://{normalized_url}
// Example: web://https://vuepress.vuejs.org/guide/
// =============================================================
class WebCrawlerSource : public IDataSource {
public:
    explicit WebCrawlerSource(WebViewSession* session = nullptr);

    std::string sourceId() const override { return "web_crawler"; }
    int priority() const override { return 4; }
    bool isOnline() const override { return true; }

    bool healthCheck() override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::optional<FetchResult> fetch(const std::string& canonical_uri) override;

private:
    WebViewSession* session_;
};

} // namespace github_research
